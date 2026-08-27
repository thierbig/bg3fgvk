#include "log.h"
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "slboot.h"
#include "vkhooks.h"

#include <sl.h>
#include <sl_helpers_vk.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_consts.h>

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

// Initializes Streamline once (idempotent). Loads DLSS-G, Reflex, and PCL.
static bool EnsureSlInit(){
  static bool s_tried = false;
  static bool s_armed = false;
  if(s_tried) return s_armed;
  s_tried = true;

  sl::Preferences p{};
  static const sl::Feature feats[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
  p.featuresToLoad = feats;
  p.numFeaturesToLoad = 3;
  p.flags |= sl::PreferenceFlags::eUseManualHooking;
  p.renderAPI = sl::RenderAPI::eVulkan;

  sl::Result r = slInit(p, sl::kSDKVersion);
  Log("slInit -> %d", (int)r);
  s_armed = (r == sl::Result::eOk);
  return s_armed;
}

void OnDeviceCreated(){
  if(!EnsureSlInit()) { Log("OnDeviceCreated: slInit failed, skipping SL device setup"); return; }

  sl::VulkanInfo vi{};
  vi.device = fgvk::gDevice;
  vi.instance = fgvk::gInstance;
  vi.physicalDevice = fgvk::gPhysicalDevice;
  vi.graphicsQueueIndex = 0;
  vi.graphicsQueueFamily = fgvk::gGraphicsFamily;
  sl::Result rVk = slSetVulkanInfo(vi);
  Log("slSetVulkanInfo -> %d", (int)rVk);

  // DLSS-G requires Reflex ON. Set Reflex before enabling DLSS-G.
  sl::ReflexOptions ro{};
  ro.mode = sl::ReflexMode::eLowLatency;
  sl::Result rReflex = slReflexSetOptions(ro);
  Log("slReflexSetOptions -> %d", (int)rReflex);

  sl::DLSSGOptions o{};
  o.mode = sl::DLSSGMode::eOn;
  o.numFramesToGenerate = 1;
  sl::ViewportHandle vp{0};
  sl::Result rDlssg = slDLSSGSetOptions(vp, o);
  Log("slDLSSGSetOptions -> %d", (int)rDlssg);
}

void PollDLSSGState(){
  sl::DLSSGState st{};
  sl::DLSSGOptions o{};
  o.mode = sl::DLSSGMode::eOn;
  o.numFramesToGenerate = 1;
  sl::ViewportHandle vp{0};
  if(slDLSSGGetState(vp, st, &o) == sl::Result::eOk){
    static uint32_t last = 0xFFFFFFFF;
    if((uint32_t)st.status != last){
      last = (uint32_t)st.status;
      Log("DLSSG status=%u framesMax=%u", (unsigned)st.status, st.numFramesToGenerateMax);
    }
  }
}
}
