#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <mutex>
namespace { std::mutex g_m; char g_path[MAX_PATH]{}; LARGE_INTEGER g_t0{}, g_freq{}; }
namespace fgvk {
void LogInit() {
  QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_t0);
  char exe[MAX_PATH]{}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
  char* slash = strrchr(exe, '\\'); if (slash) *(slash+1) = 0;
  snprintf(g_path, sizeof(g_path), "%sfgvk.log", exe);
  FILE* f=nullptr; if (fopen_s(&f,g_path,"w")==0 && f){ fputs("fgvk log start\n",f); fclose(f);} }
// Every line carries wall-clock time (to line up with sl.log's HH-MM-SS stamps) and seconds
// since DllMain.
void Log(const char* fmt, ...) {
  std::lock_guard<std::mutex> lk(g_m);
  FILE* f=nullptr; if (fopen_s(&f,g_path,"a")!=0 || !f) return;
  SYSTEMTIME st; GetLocalTime(&st);
  LARGE_INTEGER now; QueryPerformanceCounter(&now);
  double el = g_freq.QuadPart ? (double)(now.QuadPart-g_t0.QuadPart)/(double)g_freq.QuadPart : 0.0;
  fprintf(f, "[%02u:%02u:%02u.%03u +%8.3fs] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, el);
  va_list ap; va_start(ap,fmt); vfprintf(f,fmt,ap); va_end(ap); fputc('\n',f); fclose(f); }
}
