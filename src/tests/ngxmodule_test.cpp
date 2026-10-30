// Which loaded module's NVSDK_NGX_VULKAN_EvaluateFeature bg3fgvk hooks: NGX's core, never a
// feature snippet that happens to export the same symbol.
#include "ngxmodule.h"
#include <cstdio>
static int fails = 0;
static void expectRank(const char* name, int got, int want){
  if (got != want){ printf("FAIL %s: got %d want %d\n", name, got, want); fails++; } else printf("ok   %s\n", name);
}
int test_ngxmodule(){
  using fgvk::NgxCoreRank;
  // The driver's NGX core: every DLSS-SR and DLSS-G evaluate passes through it.
  expectRank("_nvngx.dll (DriverStore) -> core, first choice",
    NgxCoreRank(L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_feedb8c0271ca811\\_nvngx.dll"), 0);
  // NGX's loader name (Proton ships both): only when the core is not loaded under its own name.
  expectRank("nvngx.dll -> core, second choice", NgxCoreRank(L"C:\\windows\\system32\\nvngx.dll"), 1);
  // Issue #10: the DLSS-SR snippet exports NVSDK_NGX_VULKAN_EvaluateFeature too, and when it
  // loaded before the core it was hooked instead - DLSS-G's own evaluates never pass through it.
  expectRank("DLSS-SR snippet -> never", NgxCoreRank(L"D:\\BG3\\bin\\nvngx_dlss.dll"), -1);
  expectRank("DLSS-G snippet -> never", NgxCoreRank(L"D:\\BG3\\bin\\NativeMods\\Streamline\\nvngx_dlssg.dll"), -1);
  expectRank("DLSS-NR snippet -> never", NgxCoreRank(L"nvngx_dlssnr.dll"), -1);
  // OptiScaler as dxgi.dll exports the NGX runtime entry points as well, and its NR forwarder
  // merely starts with the core's name.
  expectRank("OptiScaler dxgi.dll -> never", NgxCoreRank(L"D:\\BG3\\bin\\dxgi.dll"), -1);
  expectRank("OptiScaler NR forwarder -> never", NgxCoreRank(L"D:\\BG3\\bin\\nvngx.dll_dlssnr.dll"), -1);
  expectRank("Streamline interposer -> never", NgxCoreRank(L"sl.interposer.dll"), -1);
  expectRank("case is ignored", NgxCoreRank(L"C:\\X\\_NVNGX.DLL"), 0);
  expectRank("forward slashes", NgxCoreRank(L"C:/X/_nvngx.dll"), 0);
  expectRank("null path -> never", NgxCoreRank(nullptr), -1);
  return fails;
}
