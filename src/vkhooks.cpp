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

static thread_local bool g_inCreate = false;
static PFN_vkCreateInstance o_CreateInstance{};
static PFN_vkCreateDevice o_CreateDevice{};
static PFN_vkCreateSwapchainKHR o_CreateSwapchainKHR{};
static PFN_vkQueuePresentKHR o_QueuePresentKHR{};

static VKAPI_ATTR VkResult VKAPI_CALL h_CreateInstance(
    const VkInstanceCreateInfo* ci,
    const VkAllocationCallbacks* a,
    VkInstance* pInstance) {
  // Re-entrant call (Streamline's own internal vkCreateInstance during slInit / while the
  // interposer wraps ours) -> native, never re-route.
  if (g_inCreate) return o_CreateInstance(ci, a, pInstance);
  g_inCreate = true;
  // slInit BEFORE creation so SL can wrap the instance (its internal creates re-enter here
  // guarded -> native).
  fgvk::EnsureStreamlineInit();
  PFN_vkCreateInstance proxy = fgvk::SlProxyCreateInstance();
  VkResult r = proxy ? proxy(ci, a, pInstance) : o_CreateInstance(ci, a, pInstance);
  g_inCreate = false;
  if (r == VK_SUCCESS) {
    gInstance = *pInstance;
    Log("vkCreateInstance ok (proxy=%p) instance=%p", (void*)proxy, (void*)gInstance);
  }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL h_CreateDevice(
    VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
    const VkAllocationCallbacks* a, VkDevice* out) {
  if (g_inCreate) return o_CreateDevice(pd, ci, a, out);
  g_inCreate = true;
  // SL device create-info surgery + interposer create (adds SL's extensions/features/queues).
  VkResult r = fgvk::CreateDeviceWithSL(pd, ci, a, out);
  g_inCreate = false;
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
  if (!vk) { Log("InstallVkHooks: vulkan-1.dll not found - aborting hook install"); return; }
  o_CreateInstance = (PFN_vkCreateInstance)GetProcAddress(vk,"vkCreateInstance");
  o_CreateDevice = (PFN_vkCreateDevice)GetProcAddress(vk,"vkCreateDevice");
  o_CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)GetProcAddress(vk,"vkCreateSwapchainKHR");
  o_QueuePresentKHR = (PFN_vkQueuePresentKHR)GetProcAddress(vk,"vkQueuePresentKHR");
  DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
  LONG a1 = DetourAttach(&(PVOID&)o_CreateInstance,(PVOID)h_CreateInstance);
  if(a1) Log("DetourAttach CreateInstance -> %ld", a1);
  LONG a2 = DetourAttach(&(PVOID&)o_CreateDevice,(PVOID)h_CreateDevice);
  if(a2) Log("DetourAttach CreateDevice -> %ld", a2);
  LONG a3 = DetourAttach(&(PVOID&)o_CreateSwapchainKHR,(PVOID)h_CreateSwapchainKHR);
  if(a3) Log("DetourAttach CreateSwapchainKHR -> %ld", a3);
  LONG a4 = DetourAttach(&(PVOID&)o_QueuePresentKHR,(PVOID)h_QueuePresentKHR);
  if(a4) Log("DetourAttach QueuePresentKHR -> %ld", a4);
  LONG r = DetourTransactionCommit();
  Log("InstallVkHooks: commit -> %ld", r);
  if (r != NO_ERROR) { Log("InstallVkHooks: COMMIT FAILED err=%ld", r); return; }
  Log("InstallVkHooks: hooked");
}

void RemoveVkHooks(){
  DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
  DetourDetach(&(PVOID&)o_CreateInstance,(PVOID)h_CreateInstance);
  DetourDetach(&(PVOID&)o_CreateDevice,(PVOID)h_CreateDevice);
  DetourDetach(&(PVOID&)o_CreateSwapchainKHR,(PVOID)h_CreateSwapchainKHR);
  DetourDetach(&(PVOID&)o_QueuePresentKHR,(PVOID)h_QueuePresentKHR);
  LONG r = DetourTransactionCommit();
  Log("RemoveVkHooks: commit -> %ld", r);
  if (r != NO_ERROR) Log("RemoveVkHooks: COMMIT FAILED err=%ld", r);
}

PFN_vkCreateSwapchainKHR NativeCreateSwapchain(){ return o_CreateSwapchainKHR; }
}
