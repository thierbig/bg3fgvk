#pragma once
#include <vulkan/vulkan.h>

namespace fgvk {
extern VkInstance gInstance;
extern VkPhysicalDevice gPhysicalDevice;
extern VkDevice gDevice;
extern VkQueue gGraphicsQueue;
extern uint32_t gGraphicsFamily;

PFN_vkCreateSwapchainKHR NativeCreateSwapchain();
PFN_vkCreateDevice NativeCreateDevice();

void InstallVkHooks();
void RemoveVkHooks();
}
