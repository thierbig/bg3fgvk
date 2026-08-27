#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
namespace { std::mutex g_m; char g_path[MAX_PATH]{}; }
namespace fgvk {
void LogInit() {
  char exe[MAX_PATH]{}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
  char* slash = strrchr(exe, '\\'); if (slash) *(slash+1) = 0;
  snprintf(g_path, sizeof(g_path), "%sfgvk.log", exe);
  FILE* f=nullptr; if (fopen_s(&f,g_path,"w")==0 && f){ fputs("fgvk log start\n",f); fclose(f);} }
void Log(const char* fmt, ...) {
  std::lock_guard<std::mutex> lk(g_m);
  FILE* f=nullptr; if (fopen_s(&f,g_path,"a")!=0 || !f) return;
  va_list ap; va_start(ap,fmt); vfprintf(f,fmt,ap); va_end(ap); fputc('\n',f); fclose(f); }
}
