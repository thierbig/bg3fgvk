#pragma once
#include <vulkan/vulkan.h>
#include <sl.h>
#include <sl_core_api.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

namespace fgvk {
struct SlFns {
  PFun_slGetNewFrameToken* getNewFrameToken{};
  PFun_slSetTagForFrame* setTagForFrame{};
  PFun_slSetConstants* setConstants{};
  PFun_slPCLSetMarker* pclSetMarker{};
  PFun_slReflexSleep* reflexSleep{};
};
SlFns& GetSlFns();
bool EnsureStreamlineInit();
void* SlProxyFn(const char* name);
void OnDeviceCreated();
void SetDLSSGeneration(bool on);   // Task 4 fills; called from the vkCreateDevice hook (Task 2)
void PollDLSSGState();    // Task 4 fills; called from the present hook
}
