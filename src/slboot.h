#pragma once
#include <vulkan/vulkan.h>
namespace fgvk {
void OnDeviceCreated();   // Task 4 fills; called from the vkCreateDevice hook (Task 2)
void PollDLSSGState();    // Task 4 fills; called from the present hook
PFN_vkCreateSwapchainKHR SlProxyCreateSwapchain();
PFN_vkQueuePresentKHR SlProxyPresent();
}
