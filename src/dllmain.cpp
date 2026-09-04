#include <windows.h>
#include "log.h"
#include "vkhooks.h"
#include "config.h"
BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(h);
    fgvk::LogInit();
    fgvk::Log("DllMain attach - installing hooks");
    fgvk::LoadConfig();
    fgvk::InstallVkHooks();
  } else if (reason == DLL_PROCESS_DETACH) {
    fgvk::RemoveVkHooks();
  }
  return TRUE;
}
