#pragma once
#include <cstring>
namespace fgvk {
// True when a module owns part of Streamline's device dispatch table: the interposer, its
// plugins (sl.common, sl.dlss_g, sl.pcl, sl.reflex), DLSS-G's NGX snippet, and the Reflex
// helper. Those call the vulkan-1 exports expecting the driver; everyone else gets the game's
// view. Matched on the file name, so a full path or a bare name both work.
// Deliberately NOT a bare "nvngx" prefix: OptiScaler ships an nvngx.dll DLSS shim next to the
// exe, and it is a third party whose hooks must keep landing on our wrappers.
inline bool IsStreamlineModule(const char* path){
  if (!path) return false;
  const char* base = path;
  for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
  return _strnicmp(base, "sl.", 3) == 0
      || _strnicmp(base, "nvngx_dlssg", 11) == 0
      || _strnicmp(base, "NvLowLatencyVk", 14) == 0;
}
}
