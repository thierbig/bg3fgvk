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

// --- Streamline function pointers, resolved dynamically from sl.interposer.dll ---
// No SL import lib is linked (see CMakeLists: only detours.lib). Every SL entry
// point below is pulled via GetProcAddress instead of calling the header's
// extern "C" declarations directly, so nothing here forces the linker to resolve
// an SL symbol.
static PFun_slInit* p_slInit{};
static PFun_slSetVulkanInfo* p_slSetVulkanInfo{};
static PFun_slGetFeatureFunction* p_slGetFeatureFunction{};
static PFun_slReflexSetOptions* p_slReflexSetOptions{};
static PFun_slDLSSGSetOptions* p_slDLSSGSetOptions{};
static PFun_slDLSSGGetState* p_slDLSSGGetState{};

// Resolves slInit/slSetVulkanInfo/slGetFeatureFunction from the interposer module
// and calls slInit once. Returns true if slInit succeeded.
static bool EnsureSlInit(){
  static bool s_tried = false;
  static bool s_armed = false;
  if(s_tried) return s_armed;
  s_tried = true;

  if(!g_sl){ g_sl = GetModuleHandleA("sl.interposer.dll");
    if(!g_sl) g_sl = LoadLibraryA("sl.interposer.dll");
    Log("sl.interposer handle=%p", (void*)g_sl); }
  if(!g_sl){ Log("EnsureSlInit: sl.interposer.dll not loaded"); return false; }

  p_slInit = (PFun_slInit*)GetProcAddress(g_sl, "slInit");
  p_slSetVulkanInfo = (PFun_slSetVulkanInfo*)GetProcAddress(g_sl, "slSetVulkanInfo");
  p_slGetFeatureFunction = (PFun_slGetFeatureFunction*)GetProcAddress(g_sl, "slGetFeatureFunction");
  Log("resolved slInit=%p slSetVulkanInfo=%p slGetFeatureFunction=%p",
      (void*)p_slInit, (void*)p_slSetVulkanInfo, (void*)p_slGetFeatureFunction);
  if(!p_slInit || !p_slSetVulkanInfo || !p_slGetFeatureFunction){
    Log("EnsureSlInit: failed to resolve core SL functions");
    return false;
  }

  sl::Preferences p{};
  static const sl::Feature feats[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
  p.featuresToLoad = feats;
  p.numFeaturesToLoad = 3;
  // NO eUseManualHooking: BG3SE found the hybrid (manual-hooking flag + interposer-driven
  // instance/device creation) crashes inside initializePlugins during the interposer's
  // vkCreateDevice - our exact crash after slInit. Let the interposer own creation.
  p.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;  // needed by slSetTagForFrame (M2)
  p.renderAPI = sl::RenderAPI::eVulkan;
  // applicationId 0xE658703: the NGX app-id family NVIDIA's driver serves on this machine
  // (the working PureDark/nvapp stack ran under it). The SDK sample id has no NGX min-spec
  // data, so plugins self-disable on this GPU and DLSS-G never arms.
  p.applicationId = 0xE658703;

  sl::Result r = p_slInit(p, sl::kSDKVersion);
  Log("slInit -> %d", (int)r);
  s_armed = (r == sl::Result::eOk);
  return s_armed;
}

// Resolves the DLSS-G / Reflex feature functions via slGetFeatureFunction.
// Must be called AFTER slSetVulkanInfo (the header requires the device to be
// set first: "Macros which use slGetFeatureFunction can only be used AFTER
// device is set by calling either slSetD3DDevice or slSetVulkanInfo").
static bool EnsureFeatureFunctions(){
  static bool s_tried = false;
  static bool s_ok = false;
  if(s_tried) return s_ok;
  s_tried = true;

  if(!p_slGetFeatureFunction){ Log("EnsureFeatureFunctions: slGetFeatureFunction not resolved"); return false; }

  void* fn = nullptr;
  sl::Result r;

  fn = nullptr;
  r = p_slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", fn);
  Log("slGetFeatureFunction(slReflexSetOptions) -> %d fn=%p", (int)r, fn);
  p_slReflexSetOptions = (PFun_slReflexSetOptions*)fn;

  fn = nullptr;
  r = p_slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fn);
  Log("slGetFeatureFunction(slDLSSGSetOptions) -> %d fn=%p", (int)r, fn);
  p_slDLSSGSetOptions = (PFun_slDLSSGSetOptions*)fn;

  fn = nullptr;
  r = p_slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", fn);
  Log("slGetFeatureFunction(slDLSSGGetState) -> %d fn=%p", (int)r, fn);
  p_slDLSSGGetState = (PFun_slDLSSGGetState*)fn;

  s_ok = p_slReflexSetOptions && p_slDLSSGSetOptions && p_slDLSSGGetState;
  if(!s_ok) Log("EnsureFeatureFunctions: one or more feature functions failed to resolve");
  return s_ok;
}

bool EnsureStreamlineInit(){ return EnsureSlInit(); }
PFN_vkCreateInstance SlProxyCreateInstance(){
  if(!g_sl){ g_sl=GetModuleHandleA("sl.interposer.dll"); if(!g_sl) g_sl=LoadLibraryA("sl.interposer.dll"); }
  return g_sl ? (PFN_vkCreateInstance)GetProcAddress(g_sl,"vkCreateInstance") : nullptr;
}
PFN_vkCreateDevice SlProxyCreateDevice(){
  if(!g_sl){ g_sl=GetModuleHandleA("sl.interposer.dll"); if(!g_sl) g_sl=LoadLibraryA("sl.interposer.dll"); }
  return g_sl ? (PFN_vkCreateDevice)GetProcAddress(g_sl,"vkCreateDevice") : nullptr;
}

void OnDeviceCreated(){
  if(!EnsureSlInit()) { Log("OnDeviceCreated: slInit failed, skipping SL device setup"); return; }

  sl::VulkanInfo vi{};
  vi.device = fgvk::gDevice;
  vi.instance = fgvk::gInstance;
  vi.physicalDevice = fgvk::gPhysicalDevice;
  vi.graphicsQueueIndex = 0;
  vi.graphicsQueueFamily = fgvk::gGraphicsFamily;
  sl::Result rVk = p_slSetVulkanInfo(vi);
  Log("slSetVulkanInfo -> %d", (int)rVk);
  if (rVk != sl::Result::eOk) { Log("OnDeviceCreated: slSetVulkanInfo failed (%d) - aborting SL setup", (int)rVk); return; }

  if(!EnsureFeatureFunctions()) { Log("OnDeviceCreated: feature function resolution failed"); return; }

  // DLSS-G requires Reflex ON. Set Reflex before enabling DLSS-G.
  sl::ReflexOptions ro{};
  ro.mode = sl::ReflexMode::eLowLatency;
  sl::Result rReflex = p_slReflexSetOptions(ro);
  Log("slReflexSetOptions -> %d", (int)rReflex);

  sl::DLSSGOptions o{};
  o.mode = sl::DLSSGMode::eOn;
  o.numFramesToGenerate = 1;
  sl::ViewportHandle vp{0};
  sl::Result rDlssg = p_slDLSSGSetOptions(vp, o);
  Log("slDLSSGSetOptions -> %d", (int)rDlssg);
}

void PollDLSSGState(){
  if(!p_slDLSSGGetState) return;
  sl::DLSSGState st{};
  sl::DLSSGOptions o{};
  o.mode = sl::DLSSGMode::eOn;
  o.numFramesToGenerate = 1;
  sl::ViewportHandle vp{0};
  if(p_slDLSSGGetState(vp, st, &o) == sl::Result::eOk){
    static uint32_t last = 0xFFFFFFFF;
    if((uint32_t)st.status != last){
      last = (uint32_t)st.status;
      Log("DLSSG status=%u framesMax=%u", (unsigned)st.status, st.numFramesToGenerateMax);
    }
  }
}
}
