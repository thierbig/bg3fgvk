#pragma once
#include <cwchar>
namespace fgvk {
// Preference of a loaded module as the target of bg3fgvk's NGX evaluate hook (lower is better,
// -1 = never), by file name; a full path or a bare name both work.
// Only NGX's core qualifies: every DLSS-SR and DLSS-G evaluate passes through it, so the hook
// sees the game's frame and DLSS-G's own calls in every session. The feature snippets
// (nvngx_dlss.dll, nvngx_dlssg.dll, ...) and OptiScaler's dxgi.dll export the same
// NVSDK_NGX_VULKAN_EvaluateFeature; hooking the first one loaded made the hook point depend on
// load order (issue #10: nvngx_dlss.dll in one session, _nvngx.dll in the next).
inline int NgxCoreRank(const wchar_t* path){
  if (!path) return -1;
  const wchar_t* base = path;
  for (const wchar_t* p = path; *p; ++p) if (*p == L'\\' || *p == L'/') base = p + 1;
  if (_wcsicmp(base, L"_nvngx.dll") == 0) return 0;
  if (_wcsicmp(base, L"nvngx.dll") == 0) return 1;   // NGX's loader name (Proton ships both)
  return -1;
}
}
