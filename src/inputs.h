#pragma once
#include <vulkan/vulkan.h>

namespace fgvk {
// Called once per present (cheap): probes lazily-loaded NGX modules and installs the
// NVSDK_NGX_VULKAN_EvaluateFeature detour when found.
void NgxProbeTick();
// Frame-token lifecycle + the PCL/Reflex marker ladder, driven from the present thread:
//  PresentMarkersBegin(): (SimulationEnd/RenderSubmitStart if no DLSS-SR eval happened this
//                         frame) then RenderSubmitEnd + PresentStart with this frame's token.
//  PresentMarkersEnd():   PresentEnd with this frame's token, then start the NEXT frame:
//                         new token, slReflexSleep, SimulationStart.
void PresentMarkersBegin();
void PresentMarkersEnd();
// True if a DLSS-SR evaluate filed tags+constants since the last call (the 3D world is
// rendering). Clears the flag.
bool ConsumeEvalSeen();
// Vulkan function access provided by vkhooks (device-level via vkGetDeviceProcAddr).
void* DeviceFn(const char* name);
void* LoaderFn(const char* name);
}
