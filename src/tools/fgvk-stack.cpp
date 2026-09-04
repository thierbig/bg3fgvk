// fgvk-stack: out-of-process thread stack dumper for a hung bg3.exe.
// Usage: fgvk-stack.exe <pid> <out-file>
// Spawned by fgvk.dll's watchdog when the present loop stalls; can also be run by hand.
// Out-of-process on purpose: walking stacks from inside the frozen process risks deadlocking on
// the heap/loader locks the suspended threads may hold.
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#pragma comment(lib, "dbghelp.lib")

typedef LONG (NTAPI *PFN_NtQueryInformationThread)(HANDLE, int, PVOID, ULONG, PULONG);

static std::string ModuleAndOffset(HANDLE hp, DWORD64 addr, bool& isSystem){
  char buf[512];
  DWORD64 base = SymGetModuleBase64(hp, addr);
  IMAGEHLP_MODULE64 mi{}; mi.SizeOfStruct = sizeof(mi);
  std::string mod = "?";
  if(base && SymGetModuleInfo64(hp, base, &mi)) mod = mi.ModuleName;
  isSystem = (_stricmp(mod.c_str(),"ntdll")==0 || _stricmp(mod.c_str(),"KERNELBASE")==0 ||
              _stricmp(mod.c_str(),"kernel32")==0 || _stricmp(mod.c_str(),"win32u")==0 ||
              _stricmp(mod.c_str(),"user32")==0 || _stricmp(mod.c_str(),"combase")==0 ||
              _stricmp(mod.c_str(),"RPCRT4")==0 || _stricmp(mod.c_str(),"ucrtbase")==0);
  char symbuf[sizeof(SYMBOL_INFO)+512]{}; SYMBOL_INFO* si=(SYMBOL_INFO*)symbuf;
  si->SizeOfStruct=sizeof(SYMBOL_INFO); si->MaxNameLen=511; DWORD64 disp=0;
  if(SymFromAddr(hp, addr, &disp, si))
    snprintf(buf,sizeof(buf),"%s!%s+0x%llx", mod.c_str(), si->Name, (unsigned long long)disp);
  else
    snprintf(buf,sizeof(buf),"%s+0x%llx", mod.c_str(), (unsigned long long)(base ? addr-base : addr));
  return buf;
}

int main(int argc, char** argv){
  if(argc < 3){ fprintf(stderr,"usage: fgvk-stack <pid> <out-file>\n"); return 2; }
  DWORD pid = (DWORD)strtoul(argv[1], nullptr, 10);
  FILE* out = fopen(argv[2], "a"); if(!out){ fprintf(stderr,"cannot open %s\n", argv[2]); return 2; }
  HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_SUSPEND_RESUME, FALSE, pid);
  if(!hp){ fprintf(out,"OpenProcess(%lu) failed: %lu\n", pid, GetLastError()); fclose(out); return 1; }
  SYSTEMTIME st; GetLocalTime(&st);
  fprintf(out,"==== fgvk-stack pid=%lu at %02u:%02u:%02u.%03u ====\n", pid, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS|SYMOPT_NO_PROMPTS);
  if(!SymInitialize(hp, nullptr, TRUE)) fprintf(out,"SymInitialize failed: %lu (module names may be missing)\n", GetLastError());
  auto pNtQIT = (PFN_NtQueryInformationThread)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if(snap == INVALID_HANDLE_VALUE){ fprintf(out,"thread snapshot failed\n"); fclose(out); return 1; }
  THREADENTRY32 te{}; te.dwSize = sizeof(te);
  int dumped=0, skipped=0;
  for(BOOL ok = Thread32First(snap,&te); ok; ok = Thread32Next(snap,&te)){
    if(te.th32OwnerProcessID != pid) continue;
    HANDLE ht = OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
    if(!ht){ skipped++; continue; }
    std::string startMod = "?";
    if(pNtQIT){ PVOID start=nullptr; if(pNtQIT(ht, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof(start), nullptr) == 0 && start){ bool s; startMod = ModuleAndOffset(hp,(DWORD64)start,s); } }
    std::vector<DWORD64> pcs;
    if(SuspendThread(ht) != (DWORD)-1){
      CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
      if(GetThreadContext(ht,&ctx)){
        STACKFRAME64 sf{};
        sf.AddrPC.Offset=ctx.Rip; sf.AddrPC.Mode=AddrModeFlat;
        sf.AddrFrame.Offset=ctx.Rbp; sf.AddrFrame.Mode=AddrModeFlat;
        sf.AddrStack.Offset=ctx.Rsp; sf.AddrStack.Mode=AddrModeFlat;
        for(int i=0;i<48;i++){
          if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hp, ht, &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
          if(!sf.AddrPC.Offset) break;
          pcs.push_back(sf.AddrPC.Offset);
        }
      }
      ResumeThread(ht);
    }
    CloseHandle(ht);
    // Skip threads that never leave the system DLLs (idle pool threads) to keep the dump readable.
    bool interesting=false; std::vector<std::string> lines;
    for(auto pc : pcs){ bool sys=false; lines.push_back(ModuleAndOffset(hp,pc,sys)); if(!sys) interesting=true; }
    if(!interesting){ skipped++; continue; }
    dumped++;
    fprintf(out,"-- tid=%lu start=%s frames=%zu\n", te.th32ThreadID, startMod.c_str(), pcs.size());
    for(auto& l : lines) fprintf(out,"     %s\n", l.c_str());
  }
  CloseHandle(snap);
  fprintf(out,"==== %d threads dumped, %d skipped (system-only or inaccessible) ====\n\n", dumped, skipped);
  fclose(out);
  SymCleanup(hp); CloseHandle(hp);
  return 0;
}
