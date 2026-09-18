// Which callers of the vulkan-1 swapchain exports are Streamline and must reach the driver.
#include "slmodule.h"
#include <cstdio>
static int fails = 0;
static void expectBool(const char* name, bool got, bool want){
  if (got != want){ printf("FAIL %s: got %d want %d\n", name, (int)got, (int)want); fails++; } else printf("ok   %s\n", name);
}
int test_slmodule(){
  using fgvk::IsStreamlineModule;
  // The interposer and the plugins own the device table that aliases our detours on Wine.
  expectBool("sl.interposer -> streamline", IsStreamlineModule("C:\\BG3\\bin\\NativeMods\\Streamline\\sl.interposer.dll"), true);
  expectBool("sl.common -> streamline", IsStreamlineModule("sl.common.dll"), true);
  expectBool("sl.dlss_g -> streamline", IsStreamlineModule("sl.dlss_g.dll"), true);
  // DLSS-G's snippet and the Reflex helper ship in the same folder and call the same exports.
  expectBool("nvngx_dlssg -> streamline", IsStreamlineModule("nvngx_dlssg.dll"), true);
  expectBool("NvLowLatencyVk -> streamline", IsStreamlineModule("NvLowLatencyVk.dll"), true);
  // Third parties keep the game's view, or their overlays draw into buffers nobody presents.
  expectBool("the game -> not streamline", IsStreamlineModule("C:\\BG3\\bin\\bg3.exe"), false);
  expectBool("Script Extender -> not streamline", IsStreamlineModule("DWrite.dll"), false);
  expectBool("OptiScaler proxy -> not streamline", IsStreamlineModule("winmm.dll"), false);
  // OptiScaler's DLSS shim is called nvngx.dll; a bare "nvngx" prefix would capture it and put
  // its hooks on the driver's present under the pacer thread - the deadlock class already fixed.
  expectBool("OptiScaler nvngx shim -> not streamline", IsStreamlineModule("C:\\BG3\\bin\\nvngx.dll"), false);
  // Forward slashes and a bare name both resolve to the same file name.
  expectBool("forward slashes -> streamline", IsStreamlineModule("Streamline/sl.reflex.dll"), true);
  expectBool("case is ignored", IsStreamlineModule("SL.PCL.DLL"), true);
  expectBool("null path -> not streamline", IsStreamlineModule(nullptr), false);
  // A name that merely contains "sl." is not a Streamline module.
  expectBool("substring only -> not streamline", IsStreamlineModule("tools\\.dll"), false);
  return fails;
}
