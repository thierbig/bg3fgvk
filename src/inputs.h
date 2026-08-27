#pragma once
#include <vulkan/vulkan.h>

namespace fgvk {
// Called once per present (cheap): probes lazily-loaded NGX modules and installs the
// NVSDK_NGX_VULKAN_EvaluateFeature detour when found.
void NgxProbeTick();
// PCL frame markers + Reflex sleep around the present call.
void PresentMarkersBegin();
void PresentMarkersEnd();
// Vulkan function access provided by vkhooks (device-level via vkGetDeviceProcAddr).
void* DeviceFn(const char* name);
void* LoaderFn(const char* name);
}
