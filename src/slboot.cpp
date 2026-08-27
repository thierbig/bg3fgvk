#include "log.h"
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "slboot.h"

namespace fgvk {
static HMODULE g_sl{};

PFN_vkCreateSwapchainKHR SlProxyCreateSwapchain(){
  if(!g_sl){ g_sl = GetModuleHandleA("sl.interposer.dll");
    if(!g_sl) g_sl = LoadLibraryA("sl.interposer.dll");
    Log("sl.interposer handle=%p", (void*)g_sl); }
  return g_sl ? (PFN_vkCreateSwapchainKHR)GetProcAddress(g_sl,"vkCreateSwapchainKHR") : nullptr;
}

PFN_vkQueuePresentKHR SlProxyPresent(){
  if(!g_sl){ g_sl = GetModuleHandleA("sl.interposer.dll");
    if(!g_sl) g_sl = LoadLibraryA("sl.interposer.dll");
    Log("sl.interposer handle=%p", (void*)g_sl); }
  return g_sl ? (PFN_vkQueuePresentKHR)GetProcAddress(g_sl,"vkQueuePresentKHR") : nullptr;
}

void OnDeviceCreated(){ fgvk::Log("OnDeviceCreated stub"); }
void PollDLSSGState(){}
}
