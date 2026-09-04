#include "vkhooks.h"
#include "log.h"
#include "slboot.h"
#include "inputs.h"
#include "config.h"
#include <windows.h>
#include <detours.h>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <thread>

namespace fgvk {
VkInstance gInstance{};
VkPhysicalDevice gPhysicalDevice{};
VkDevice gDevice{};
VkQueue gGraphicsQueue{};
uint32_t gGraphicsFamily{};

// watchdog: counters + a position marker naming the in-flight call (inputs.cpp writes g_wdEvals/g_wdPos)
std::atomic<uint32_t> g_wdPresents{0};
std::atomic<uint32_t> g_wdEvals{0};
// pos: 0 idle (inside the game); 1 present-enter; 2 inside proxy present; 3 post-present
// markers/Reflex sleep; 4 game vkAcquireNextImageKHR; 5 vkWaitForFences; 6 vkQueueSubmit(2);
// 7 vkDeviceWaitIdle; 8 vkQueueWaitIdle; 10/11/12 eval stages
std::atomic<int> g_wdPos{0};
static std::atomic<long long> g_wdPosSinceUs{0};
static std::atomic<bool> g_genOn{false};   // DLSS-G currently requested on (gate state)
static inline long long NowUs(){ LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (long long)((double)c.QuadPart*1e6/(double)f.QuadPart); }
struct PosScope { int prev; PosScope(int code){ prev=g_wdPos.exchange(code); g_wdPosSinceUs.store(NowUs()); } ~PosScope(){ g_wdPos.store(prev); g_wdPosSinceUs.store(NowUs()); } };

// On a stall the watchdog spawns fgvk-stack.exe (next to this DLL) to dump every thread's stack
// out-of-process into bin\fgvk-stacks.log - the only way to see what the present thread and
// Streamline's pacer are blocked on inside the driver.
static void SpawnStackDump(const char* tag){
  char dll[MAX_PATH]{}; HMODULE self{};
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)&SpawnStackDump,&self);
  GetModuleFileNameA(self, dll, MAX_PATH); char* s=strrchr(dll,'\\'); if(s) *(s+1)=0;
  char exe[MAX_PATH]{}; GetModuleFileNameA(nullptr, exe, MAX_PATH); s=strrchr(exe,'\\'); if(s) *(s+1)=0;
  char cmd[1024]; snprintf(cmd,sizeof(cmd),"\"%sfgvk-stack.exe\" %lu \"%sfgvk-stacks.log\"", dll, GetCurrentProcessId(), exe);
  STARTUPINFOA si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  Log("wd: stack dump (%s) -> %s%s", tag, ok?"spawned ":"CreateProcess failed ", ok?cmd:"");
  if(ok){ CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}

static void StartWatchdog(){
  static std::atomic<bool> started{false};
  bool exp=false; if(!started.compare_exchange_strong(exp,true)) return;
  std::thread([]{
    uint32_t lastP=0; int stalledTicks=0; int dumps=0;
    for(;;){ Sleep(2000);
      uint32_t p=g_wdPresents.load(), e=g_wdEvals.load(); int pos=g_wdPos.load();
      if(p!=lastP && pos==0){ lastP=p; stalledTicks=0; continue; }
      bool stalled = (p==lastP);
      double inPos = (NowUs()-g_wdPosSinceUs.load())/1e6;
      Log("wd: presents=%u evals=%u pos=%d (%.1fs there) gen=%d%s", p,e,pos,inPos,(int)g_genOn.load(),stalled?" STALLED":"");
      stalledTicks = stalled ? stalledTicks+1 : 0;
      // first dump after ~6s of no presents, a second one ~10s later to see what moved
      if(p>0 && ((stalledTicks==3 && dumps==0) || (stalledTicks==8 && dumps==1))){ dumps++; SpawnStackDump(dumps==1?"first":"second"); }
      lastP=p; }
  }).detach();
}

// DLSS-G gate: driven by whether the game's DLSS-SR evaluated this frame (= the 3D world is
// rendering), NEVER by frame time. The previous frame-time gate manufactured a full FG
// release + re-create at every in-world streaming burst; both fatal WaitSemaphores timeouts
// (19:51 and 21:23 runs) sit inside that cycle, and PureDark rides through the same hitches
// with FG left on. Menus, loading screens and videos run no DLSS-SR, so "no eval for a while"
// is the guide's own "turn FG off when not rendering game frames" signal (17.0) - and with
// eRetainResourcesWhenOff those transitions free nothing. Loading screens also call
// vkDeviceWaitIdle from a loader thread (sl.log tid 540); with FG suspended there that flush
// meets an idle pacer.
static uint32_t g_evalStreak = 0, g_idleStreak = 0;
#define kOnAfterEvalFrames  (Cfg().onAfterEvalFrames)    // ~2s of world rendering before enabling
#define kOffAfterIdleFrames (Cfg().offAfterIdleFrames)   // ~0.5-1s of presents without DLSS-SR before suspending
static void EvalGate(bool evalThisFrame){
  if(evalThisFrame){
    g_idleStreak = 0;
    if(g_evalStreak < 100000) g_evalStreak++;
    if(!g_genOn.load() && g_evalStreak >= kOnAfterEvalFrames){
      g_genOn.store(true); Log("gate: %u consecutive DLSS-SR frames -> DLSS-G ON", g_evalStreak); SetDLSSGeneration(true); }
  } else {
    g_evalStreak = 0;
    if(g_idleStreak < 100000) g_idleStreak++;
    if(g_genOn.load() && g_idleStreak >= kOffAfterIdleFrames){
      g_genOn.store(false); Log("gate: %u presents without DLSS-SR (menu/loading/video) -> DLSS-G suspended (resources retained)", g_idleStreak); SetDLSSGeneration(false); }
  }
}
// Guide 17.0: DLSS-G off before any swap-chain manipulation "to avoid potential deadlocks".
static void GateOffForSwapchain(){
  g_evalStreak = 0; g_idleStreak = 0;
  if(g_genOn.load()){ g_genOn.store(false); Log("gate: swapchain re-create -> DLSS-G suspended first"); SetDLSSGeneration(false); }
}

// The game's own vkGetInstanceProcAddr (real loader), captured at hook install.
static PFN_vkGetInstanceProcAddr o_GIPA{};
// The interposer's GIPA/GDPA - SL owns every function these hand back.
static PFN_vkGetInstanceProcAddr ip_GIPA{};
static PFN_vkGetDeviceProcAddr   ip_GDPA{};
// Interposer targets we thin-wrap (resolved lazily via the interposer's GIPA/GDPA).
static PFN_vkCreateInstance     t_CreateInstance{};
static PFN_vkCreateDevice       t_CreateDevice{};
static PFN_vkQueuePresentKHR    t_QueuePresentKHR{};
static PFN_vkCreateSwapchainKHR t_CreateSwapchainKHR{};

// Re-entry guard: we Detoured vulkan-1's GIPA in place, so when the interposer forwards to
// the real loader (to reach the driver) it re-enters our hook. Without this, our hook routes
// that back into the interposer -> infinite recursion (the slInit hang). Calls made while
// g_reentry>0 originate INSIDE the interposer and must go straight to the real loader.
static thread_local int g_reentry = 0;
struct Reentry { Reentry(){ ++g_reentry; } ~Reentry(){ --g_reentry; } };
// Global guard for slInit: SL's plugins spawn WORKER THREADS during init that call the hooked
// GIPA (where thread_local g_reentry is 0). The game makes no Vulkan calls during slInit, so
// forcing ALL threads to the real loader for that window is safe and breaks the cross-thread
// recursion that hangs slInit.
static std::atomic<int> g_globalReal{0};
struct GlobalReal { GlobalReal(){ ++g_globalReal; } ~GlobalReal(){ --g_globalReal; } };
static inline bool ForceReal(){ return g_reentry || g_globalReal.load(std::memory_order_acquire); }

// ---- thin wrappers: call the INTERPOSER target, capture/act, return ----------------------
// slInit runs HERE (in the actual vkCreateInstance CALL), never in the GIPA resolve: the
// resolve runs inside the game's vkGetInstanceProcAddr which holds the Vulkan loader lock, and
// slInit's plugin LoadLibrary -> DllMain needs that same lock = deadlock (the slInit hang, no
// sl.log). The create CALL runs in normal execution, lock released.
static void EnsureSlAndInterposer(){
  static bool inited=false; if(inited) return; inited=true;
  Reentry _; GlobalReal _g;   // interposer load + slInit forward to the real loader (all threads)
  ip_GIPA = (PFN_vkGetInstanceProcAddr)SlProxyFn("vkGetInstanceProcAddr");
  ip_GDPA = (PFN_vkGetDeviceProcAddr)SlProxyFn("vkGetDeviceProcAddr");
  EnsureStreamlineInit();   // slInit
  Log("SL init in w_CreateInstance: ip_GIPA=%p ip_GDPA=%p", (void*)ip_GIPA,(void*)ip_GDPA);
}

static VKAPI_ATTR VkResult VKAPI_CALL w_CreateInstance(
    const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out){
  EnsureSlAndInterposer();   // slInit here (loader lock released), NOT in the GIPA resolve
  // Resolve the interposer's vkCreateInstance now that SL is initialized.
  if(!t_CreateInstance){ Reentry _; t_CreateInstance = (PFN_vkCreateInstance)(ip_GIPA ? ip_GIPA(nullptr,"vkCreateInstance") : nullptr); }
  if(!t_CreateInstance){ Log("w_CreateInstance: no interposer vkCreateInstance"); return VK_ERROR_INITIALIZATION_FAILED; }
  VkResult r; { Reentry _; r = t_CreateInstance(ci, a, out); }
  if (r == VK_SUCCESS){ gInstance = *out; Log("w_CreateInstance ok instance=%p", (void*)gInstance); }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_CreateDevice(
    VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
    const VkAllocationCallbacks* a, VkDevice* out){
  // Interposer owns the device (its own surgery/queues). We only capture + kick off SL setup.
  VkResult r; { Reentry _; r = t_CreateDevice(pd, ci, a, out); }
  if (r == VK_SUCCESS){
    gPhysicalDevice = pd; gDevice = *out;
    for (uint32_t i=0;i<ci->queueCreateInfoCount;i++){ gGraphicsFamily = ci->pQueueCreateInfos[i].queueFamilyIndex; break; }
    Log("w_CreateDevice ok device=%p phys=%p gfxFamily=%u", (void*)gDevice,(void*)pd,gGraphicsFamily);
    { Reentry _; OnDeviceCreated(); }   // slboot: Reflex options (DLSS-G itself waits for the gate)
    StartWatchdog();
  }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_CreateSwapchainKHR(
    VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
    const VkAllocationCallbacks* a, VkSwapchainKHR* out){
  // GAME'S EXACT swapchain (PureDark parity - his log: 'created original SwapChain'). The
  // earlier 7-image bump likely broke HW flip metering (shallow flip-queue depth limits) ->
  // 'FC feedback' warnings -> CPU-pacer fallback whose 100ms semaphore waits are the freeze.
  // The acquire starvation the bump addressed came from the (now-fixed) marker-mutex era.
  static bool logged=false; if(!logged){ logged=true;
    Log("w_CreateSwapchainKHR %ux%u fmt=%d mode=%d minImg=%u (game's own)",
        ci->imageExtent.width, ci->imageExtent.height, (int)ci->imageFormat,
        (int)ci->presentMode, ci->minImageCount); }
  GateOffForSwapchain();
  VkResult r; { Reentry _; r = t_CreateSwapchainKHR(dev, ci, a, out); }
  static bool l2=false; if(!l2){ l2=true; Log("w_CreateSwapchainKHR -> %d sc=%p", (int)r, out?(void*)*out:nullptr); }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* pi){
  StartWatchdog();
  g_wdPresents.fetch_add(1);
  PosScope _p(1);
  NgxProbeTick();                  // cheap after hooked (one bool)
  EvalGate(ConsumeEvalSeen());     // slDLSSGSetOptions on the present thread, before this present (guide 6.0)
  PresentMarkersBegin();           // RenderSubmitEnd + PresentStart with this frame's token (present thread)
  PollDLSSGState();                // slDLSSGGetState on the present thread: status + generated-frame stats
  static bool logged=false; if(!logged){ logged=true;
    Log("w_QueuePresentKHR live queue=%p tid=%lu waitSems=%u swapchains=%u pNext=%s (eval-driven gate)", (void*)q,
        (unsigned long)GetCurrentThreadId(), pi?pi->waitSemaphoreCount:0u, pi?pi->swapchainCount:0u, (pi&&pi->pNext)?"yes":"no"); }
  static int oddLogged=0; if(pi && pi->waitSemaphoreCount!=1 && oddLogged<5){ oddLogged++; Log("present with waitSemaphoreCount=%u", pi->waitSemaphoreCount); }
  VkResult r;
  { PosScope _q(2); Reentry _; r = t_QueuePresentKHR(q, pi); }   // interposer present = DLSS-G generation
  static int badLogged=0; if(r!=VK_SUCCESS && badLogged<10){ badLogged++; Log("present -> %d", (int)r); }
  { PosScope _m(3); PresentMarkersEnd(); }   // PresentEnd, then next frame: new token + Reflex sleep + SimulationStart
  return r;
}

// ---- stall attribution: thin pass-through wrappers on every call the game can block in ------
static PFN_vkAcquireNextImageKHR t_AcquireNextImageKHR{};
static PFN_vkWaitForFences       t_WaitForFences{};
static PFN_vkQueueSubmit         t_QueueSubmit{};
static PFN_vkQueueSubmit2        t_QueueSubmit2{};      // vkQueueSubmit2 and vkQueueSubmit2KHR share the prototype
static PFN_vkDeviceWaitIdle      t_DeviceWaitIdle{};
static PFN_vkQueueWaitIdle       t_QueueWaitIdle{};
static PFN_vkSetHdrMetadataEXT   t_SetHdrMetadataEXT{};
static VKAPI_ATTR VkResult VKAPI_CALL w_AcquireNextImageKHR(VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t* idx){
  static bool logged=false; if(!logged){ logged=true; Log("game acquire: swapchain=%p timeout=%llu semaphore=%p fence=%p", (void*)sc, (unsigned long long)timeout, (void*)sem, (void*)fence); }
  VkResult r; { PosScope _(4); Reentry _r; r = t_AcquireNextImageKHR(d,sc,timeout,sem,fence,idx); }
  static int n=0; if(r!=VK_SUCCESS && n<10){ n++; Log("game acquire -> %d (index=%u)", (int)r, idx?*idx:0u); }
  return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_WaitForFences(VkDevice d, uint32_t n, const VkFence* f, VkBool32 all, uint64_t timeout){
  PosScope _(5); return t_WaitForFences(d,n,f,all,timeout);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f){
  PosScope _(6); return t_QueueSubmit(q,n,s,f);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f){
  PosScope _(6); return t_QueueSubmit2(q,n,s,f);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_DeviceWaitIdle(VkDevice d){
  Log("game vkDeviceWaitIdle (tid=%lu)", (unsigned long)GetCurrentThreadId());
  VkResult r; { PosScope _(7); Reentry _r; r = t_DeviceWaitIdle(d); } return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_QueueWaitIdle(VkQueue q){
  static int n=0; if(n<5){ n++; Log("game vkQueueWaitIdle queue=%p (tid=%lu)", (void*)q, (unsigned long)GetCurrentThreadId()); }
  PosScope _(8); return t_QueueWaitIdle(q);
}
static VKAPI_ATTR void VKAPI_CALL w_SetHdrMetadataEXT(VkDevice d, uint32_t n, const VkSwapchainKHR* sc, const VkHdrMetadataEXT* md){
  Log("game vkSetHdrMetadataEXT swapchains=%u first=%p maxLum=%.1f (tid=%lu)", n, (n&&sc)?(void*)sc[0]:nullptr, md?md->maxLuminance:0.f, (unsigned long)GetCurrentThreadId());
  t_SetHdrMetadataEXT(d,n,sc,md);
}
// Swap in a wrapper for the names above; everything else passes straight through.
static PFN_vkVoidFunction WrapDeviceFn(const char* name, PFN_vkVoidFunction ip){
  if(!strcmp(name,"vkAcquireNextImageKHR")){ t_AcquireNextImageKHR=(PFN_vkAcquireNextImageKHR)ip; return (PFN_vkVoidFunction)w_AcquireNextImageKHR; }
  if(!strcmp(name,"vkWaitForFences"))      { t_WaitForFences=(PFN_vkWaitForFences)ip;             return (PFN_vkVoidFunction)w_WaitForFences; }
  if(!strcmp(name,"vkQueueSubmit"))        { t_QueueSubmit=(PFN_vkQueueSubmit)ip;                 return (PFN_vkVoidFunction)w_QueueSubmit; }
  if(!strcmp(name,"vkQueueSubmit2") || !strcmp(name,"vkQueueSubmit2KHR")){ t_QueueSubmit2=(PFN_vkQueueSubmit2)ip; return (PFN_vkVoidFunction)w_QueueSubmit2; }
  if(!strcmp(name,"vkDeviceWaitIdle"))     { t_DeviceWaitIdle=(PFN_vkDeviceWaitIdle)ip;           return (PFN_vkVoidFunction)w_DeviceWaitIdle; }
  if(!strcmp(name,"vkQueueWaitIdle"))      { t_QueueWaitIdle=(PFN_vkQueueWaitIdle)ip;             return (PFN_vkVoidFunction)w_QueueWaitIdle; }
  if(!strcmp(name,"vkSetHdrMetadataEXT"))  { t_SetHdrMetadataEXT=(PFN_vkSetHdrMetadataEXT)ip;     return (PFN_vkVoidFunction)w_SetHdrMetadataEXT; }
  return ip;
}

// ---- wrapped device-proc-addr: hand the game interposer device fns, wrap present/swapchain --
static PFN_vkGetDeviceProcAddr o_GDPA_real{};   // real loader GDPA, for re-entrant forwards
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL w_GetDeviceProcAddr(VkDevice dev, const char* name){
  if (!name) return nullptr;
  // Re-entrant (interposer forwarding to the driver): go to the real loader, not the interposer.
  if (ForceReal()) return o_GDPA_real ? o_GDPA_real(dev, name) : nullptr;
  PFN_vkVoidFunction ip; { Reentry _; ip = ip_GDPA ? ip_GDPA(dev, name) : nullptr; }
  if (!ip) return nullptr;
  if (!strcmp(name, "vkQueuePresentKHR"))   { t_QueuePresentKHR   = (PFN_vkQueuePresentKHR)ip;   return (PFN_vkVoidFunction)w_QueuePresentKHR; }   // wrap: PollDLSSGState MUST be on the present thread
  if (!strcmp(name, "vkCreateSwapchainKHR")){ t_CreateSwapchainKHR= (PFN_vkCreateSwapchainKHR)ip; return (PFN_vkVoidFunction)w_CreateSwapchainKHR; }
  return WrapDeviceFn(name, ip);   // stall-attribution wrappers, else the interposer's own device function
}

// ---- the single entry hook: vkGetInstanceProcAddr ---------------------------------------
// Init is DEFERRED to the vkCreateInstance resolution, NOT the first GIPA call: the game's
// first GIPA resolution happens in early loader-lock context, where slInit (which LoadLibrary's
// plugins) deadlocks (observed: log stops right after 'resolved slInit=...', game hangs).
// PureDark inits around instance creation for the same reason. We resolve the interposer's
// GIPA/GDPA FIRST so slInit's own re-entrant GIPA calls see the interposer, not the raw loader.
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL h_GetInstanceProcAddr(VkInstance inst, const char* name){
  if(!name) return nullptr;
  // Re-entrant (interposer forwarding to the driver): straight to the real loader.
  if(ForceReal()) return o_GIPA ? o_GIPA(inst, name) : nullptr;

  // vkCreateInstance: return our wrapper WITHOUT initializing SL here (this runs under the
  // loader lock). slInit + interposer resolution happen inside w_CreateInstance (the CALL).
  if(!strcmp(name,"vkCreateInstance")) return (PFN_vkVoidFunction)w_CreateInstance;

  // Everything else: interposer if ready, else the real loader. Before slInit (ip_GIPA null),
  // the game's dispatch build goes to the real loader - correct, we only need the interposer
  // for create/present, which we swap in below once ready.
  PFN_vkVoidFunction ip = nullptr;
  if(ip_GIPA){ Reentry _; ip = ip_GIPA(inst, name); }
  if(!ip) ip = o_GIPA ? o_GIPA(inst, name) : nullptr;
  if(!ip) return nullptr;
  if(!strcmp(name,"vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)h_GetInstanceProcAddr;
  if(!strcmp(name,"vkCreateDevice"))    { t_CreateDevice   = (PFN_vkCreateDevice)ip;   return (PFN_vkVoidFunction)w_CreateDevice; }
  if(!strcmp(name,"vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)w_GetDeviceProcAddr;
  if(!strcmp(name,"vkQueuePresentKHR"))   { t_QueuePresentKHR   = (PFN_vkQueuePresentKHR)ip;   return (PFN_vkVoidFunction)w_QueuePresentKHR; }
  if(!strcmp(name,"vkCreateSwapchainKHR")){ t_CreateSwapchainKHR= (PFN_vkCreateSwapchainKHR)ip; return (PFN_vkVoidFunction)w_CreateSwapchainKHR; }
  return WrapDeviceFn(name, ip);   // same wrappers if the game resolves device functions through GIPA
}

// ---- Vulkan function access for inputs.cpp (UI image, mem props) -------------------------
void* DeviceFn(const char* name){ return (ip_GDPA && gDevice) ? (void*)ip_GDPA(gDevice, name) : nullptr; }
void* LoaderFn(const char* name){
  // instance-scope functions (e.g. vkGetPhysicalDeviceMemoryProperties) via the interposer GIPA
  return (ip_GIPA && gInstance) ? (void*)ip_GIPA(gInstance, name) : nullptr;
}

void InstallVkHooks(){
  HMODULE vk = GetModuleHandleA("vulkan-1.dll");
  if(!vk) vk = LoadLibraryA("vulkan-1.dll");
  if(!vk){ Log("InstallVkHooks: vulkan-1.dll not found"); return; }
  o_GIPA = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
  o_GDPA_real = (PFN_vkGetDeviceProcAddr)GetProcAddress(vk, "vkGetDeviceProcAddr");
  if(!o_GIPA){ Log("InstallVkHooks: vkGetInstanceProcAddr export missing"); return; }
  DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
  DetourAttach(&(PVOID&)o_GIPA,(PVOID)h_GetInstanceProcAddr);
  LONG r = DetourTransactionCommit();
  Log("InstallVkHooks: GIPA hooked commit=%ld (o_GIPA=%p)", r,(void*)o_GIPA);
}

void RemoveVkHooks(){
  DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
  DetourDetach(&(PVOID&)o_GIPA,(PVOID)h_GetInstanceProcAddr);
  DetourTransactionCommit();
}
}
