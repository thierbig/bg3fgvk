#include "vkhooks.h"
#include "log.h"
#include "slboot.h"
#include <windows.h>
#include <detours.h>

namespace fgvk {
VkInstance gInstance{};
VkPhysicalDevice gPhysicalDevice{};
VkDevice gDevice{};
VkQueue gGraphicsQueue{};
uint32_t gGraphicsFamily{};

static PFN_vkCreateInstance o_CreateInstance{};
static PFN_vkCreateDevice o_CreateDevice{};
static PFN_vkCreateSwapchainKHR o_CreateSwapchainKHR{};
static PFN_vkQueuePresentKHR o_QueuePresentKHR{};

static VKAPI_ATTR VkResult VKAPI_CALL h_CreateInstance(
    const VkInstanceCreateInfo* ci,
    const VkAllocationCallbacks* a,
    VkInstance* pInstance) {
  VkResult r = o_CreateInstance(ci, a, pInstance);
  if (r == VK_SUCCESS) {
    gInstance = *pInstance;
    Log("vkCreateInstance ok instance=%p", (void*)gInstance);
  }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL h_CreateDevice(
    VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
    const VkAllocationCallbacks* a, VkDevice* out) {
  VkResult r = o_CreateDevice(pd, ci, a, out);
  if (r == VK_SUCCESS) {
    gPhysicalDevice = pd; gDevice = *out;
    // first graphics queue from ci
    for (uint32_t i=0;i<ci->queueCreateInfoCount;i++){
      gGraphicsFamily = ci->pQueueCreateInfos[i].queueFamilyIndex; break; }
    Log("vkCreateDevice ok device=%p phys=%p gfxFamily=%u", (void*)gDevice,(void*)pd,gGraphicsFamily);
    OnDeviceCreated();   // slboot: slSetVulkanInfo + DLSS-G enable
  }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL h_CreateSwapchainKHR(
    VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
    const VkAllocationCallbacks* a, VkSwapchainKHR* out){
  auto proxy = SlProxyCreateSwapchain();
  Log("vkCreateSwapchainKHR %ux%u fmt=%d proxy=%p", ci->imageExtent.width,
      ci->imageExtent.height, (int)ci->imageFormat, (void*)proxy);
  if (proxy) return proxy(dev, ci, a, out);   // <-- DLSS-G proxy swapchain in the present path
  return o_CreateSwapchainKHR(dev, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL h_QueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
  PollDLSSGState();
  auto proxy = SlProxyPresent();
  if (proxy) return proxy(queue, pPresentInfo);
  return o_QueuePresentKHR(queue, pPresentInfo);
}

void InstallVkHooks(){
  HMODULE vk = GetModuleHandleA("vulkan-1.dll");
  if(!vk) vk = LoadLibraryA("vulkan-1.dll");
  o_CreateInstance = (PFN_vkCreateInstance)GetProcAddress(vk,"vkCreateInstance");
  o_CreateDevice = (PFN_vkCreateDevice)GetProcAddress(vk,"vkCreateDevice");
  o_CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)GetProcAddress(vk,"vkCreateSwapchainKHR");
  o_QueuePresentKHR = (PFN_vkQueuePresentKHR)GetProcAddress(vk,"vkQueuePresentKHR");
  DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
  DetourAttach(&(PVOID&)o_CreateInstance,(PVOID)h_CreateInstance);
  DetourAttach(&(PVOID&)o_CreateDevice,(PVOID)h_CreateDevice);
  DetourAttach(&(PVOID&)o_CreateSwapchainKHR,(PVOID)h_CreateSwapchainKHR);
  DetourAttach(&(PVOID&)o_QueuePresentKHR,(PVOID)h_QueuePresentKHR);
  DetourTransactionCommit();
  Log("InstallVkHooks: CreateInstance+CreateDevice+CreateSwapchainKHR+QueuePresentKHR hooked (oInst=%p oDev=%p oSwap=%p oPresent=%p)", (void*)o_CreateInstance, (void*)o_CreateDevice, (void*)o_CreateSwapchainKHR, (void*)o_QueuePresentKHR);
}

void RemoveVkHooks(){
  DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
  DetourDetach(&(PVOID&)o_CreateInstance,(PVOID)h_CreateInstance);
  DetourDetach(&(PVOID&)o_CreateDevice,(PVOID)h_CreateDevice);
  DetourDetach(&(PVOID&)o_CreateSwapchainKHR,(PVOID)h_CreateSwapchainKHR);
  DetourDetach(&(PVOID&)o_QueuePresentKHR,(PVOID)h_QueuePresentKHR);
  DetourTransactionCommit();
}

PFN_vkCreateSwapchainKHR NativeCreateSwapchain(){ return o_CreateSwapchainKHR; }
}
