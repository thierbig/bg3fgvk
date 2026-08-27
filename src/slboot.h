#pragma once
#include <vulkan/vulkan.h>
namespace fgvk {
bool EnsureStreamlineInit();
PFN_vkCreateInstance SlProxyCreateInstance();
PFN_vkCreateDevice SlProxyCreateDevice();
void* SlProxyFn(const char* name);
VkResult CreateDeviceWithSL(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out);
void OnDeviceCreated();   // Task 4 fills; called from the vkCreateDevice hook (Task 2)
void PollDLSSGState();    // Task 4 fills; called from the present hook
PFN_vkCreateSwapchainKHR SlProxyCreateSwapchain();
PFN_vkQueuePresentKHR SlProxyPresent();
}
