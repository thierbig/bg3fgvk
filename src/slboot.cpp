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
#include <vector>
#include <string>
#include <algorithm>
#include <sl_core_api.h>

namespace fgvk {
static HMODULE g_sl{};
static SlFns g_slFns;
SlFns& GetSlFns(){ return g_slFns; }

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
  // EXPLICIT flags - the default includes eAllowOTA|eLoadDownloadedPlugins, and mixing
  // OTA-downloaded newer plugins with our version-matched 2.12.0 interposer crashes inside
  // initializePlugins with no log line (BG3SE hit and documented this exact crash; we ship
  // the full matched runtime, so OTA must stay off).
  p.flags = sl::PreferenceFlags::eDisableCLStateTracking
          | sl::PreferenceFlags::eUseFrameBasedResourceTagging;   // slSetTagForFrame needs this
  p.renderAPI = sl::RenderAPI::eVulkan;
  // applicationId 0xE658703: the NGX app-id family NVIDIA's driver serves on this machine
  // (the working PureDark/nvapp stack ran under it). The SDK sample id has no NGX min-spec
  // data, so plugins self-disable on this GPU and DLSS-G never arms.
  p.applicationId = 0xE658703;
  // Streamline's own verbose log - written next to the game exe; names the exact
  // internal step DLSS-G is on when something wedges.
  p.logLevel = sl::LogLevel::eVerbose;
  static const wchar_t* kLogPath = L"C:\\Games\\Baldurs Gate 3\\bin";
  p.pathToLogsAndData = kLogPath;

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

  // Frame-data + marker functions (core exports + feature fns) for the input pipeline.
  g_slFns.getNewFrameToken = (PFun_slGetNewFrameToken*)GetProcAddress(g_sl,"slGetNewFrameToken");
  g_slFns.setTagForFrame   = (PFun_slSetTagForFrame*)  GetProcAddress(g_sl,"slSetTagForFrame");
  g_slFns.setConstants     = (PFun_slSetConstants*)    GetProcAddress(g_sl,"slSetConstants");
  { void* f=nullptr; p_slGetFeatureFunction(sl::kFeaturePCL,"slPCLSetMarker",f);
    g_slFns.pclSetMarker=(PFun_slPCLSetMarker*)f; }
  { void* f=nullptr; p_slGetFeatureFunction(sl::kFeatureReflex,"slReflexSleep",f);
    g_slFns.reflexSleep=(PFun_slReflexSleep*)f; }
  Log("frame fns: token=%p tag=%p consts=%p pcl=%p sleep=%p",
      (void*)g_slFns.getNewFrameToken,(void*)g_slFns.setTagForFrame,(void*)g_slFns.setConstants,
      (void*)g_slFns.pclSetMarker,(void*)g_slFns.reflexSleep);
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
void* SlProxyFn(const char* name){
  if(!g_sl){ g_sl=GetModuleHandleA("sl.interposer.dll"); if(!g_sl) g_sl=LoadLibraryA("sl.interposer.dll"); }
  return g_sl ? (void*)GetProcAddress(g_sl, name) : nullptr;
}
PFN_vkCreateDevice SlProxyCreateDevice(){
  if(!g_sl){ g_sl=GetModuleHandleA("sl.interposer.dll"); if(!g_sl) g_sl=LoadLibraryA("sl.interposer.dll"); }
  return g_sl ? (PFN_vkCreateDevice)GetProcAddress(g_sl,"vkCreateDevice") : nullptr;
}

// ---- SL device create-info surgery (ported from BG3SE Vulkan.inl) -------------------
// DLSS-G needs extra device extensions, Vulkan1.2/1.3 features, a compute queue and an
// optical-flow queue injected into VkDeviceCreateInfo before the interposer builds the
// device. Skipping this crashes inside the interposer's vkCreateDevice.
static PFun_slGetFeatureRequirements* p_slGetFeatureReqs = nullptr;

struct SLSlots { uint32_t gfxFamily=~0u, gfxIndex=0, compFamily=~0u, compIndex=0, ofaFamily=~0u, ofaIndex=0; };
SLSlots g_slots;

struct SLReqs { std::vector<std::string> devExt, f12, f13; uint32_t gfxQ=0, compQ=0, ofaQ=0; bool valid=false; };
static SLReqs g_reqs;
static void addUniq(std::vector<std::string>& v, const char* s){ for(auto&e:v) if(e==s) return; v.emplace_back(s); }

static void FetchSLRequirements(){
  if(g_reqs.valid) return;
  if(!p_slGetFeatureReqs){ if(!g_sl){ g_sl=GetModuleHandleA("sl.interposer.dll"); } if(g_sl) p_slGetFeatureReqs=(PFun_slGetFeatureRequirements*)GetProcAddress(g_sl,"slGetFeatureRequirements"); }
  if(!p_slGetFeatureReqs){ Log("FetchSLRequirements: slGetFeatureRequirements not resolved"); return; }
  sl::Feature feats[]={ sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
  for(auto f: feats){
    sl::FeatureRequirements req{};
    sl::Result r=p_slGetFeatureReqs(f, req);
    Log("slGetFeatureRequirements(%u) -> %d ext=%u f12=%u f13=%u gfxQ=%u compQ=%u ofaQ=%u",
        (unsigned)f,(int)r,req.vkNumDeviceExtensions,req.vkNumFeatures12,req.vkNumFeatures13,
        req.vkNumGraphicsQueuesRequired,req.vkNumComputeQueuesRequired,req.vkNumOpticalFlowQueuesRequired);
    if(r!=sl::Result::eOk) continue;
    for(uint32_t i=0;i<req.vkNumDeviceExtensions;i++) addUniq(g_reqs.devExt, req.vkDeviceExtensions[i]);
    for(uint32_t i=0;i<req.vkNumFeatures12;i++) addUniq(g_reqs.f12, req.vkFeatures12[i]);
    for(uint32_t i=0;i<req.vkNumFeatures13;i++) addUniq(g_reqs.f13, req.vkFeatures13[i]);
    g_reqs.gfxQ=(std::max)(g_reqs.gfxQ, req.vkNumGraphicsQueuesRequired);
    g_reqs.compQ=(std::max)(g_reqs.compQ, req.vkNumComputeQueuesRequired);
    g_reqs.ofaQ=(std::max)(g_reqs.ofaQ, req.vkNumOpticalFlowQueuesRequired);
    g_reqs.valid=true;
  }
  Log("SL reqs aggregated: devExt=%u f12=%u f13=%u gfxQ=%u compQ=%u ofaQ=%u",
      (unsigned)g_reqs.devExt.size(),(unsigned)g_reqs.f12.size(),(unsigned)g_reqs.f13.size(),
      g_reqs.gfxQ,g_reqs.compQ,g_reqs.ofaQ);
}

VkResult CreateDeviceWithSL(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                            const VkAllocationCallbacks* a, VkDevice* out){
  // PLAIN PASSTHROUGH to the interposer (PureDark parity). Its own vkCreateDevice does the
  // surgery itself - the SL log shows it adding plugin-requested extensions and finding queue
  // families - and our manual injection triggered VUID-02830 (the game already chains those
  // features individually), poisoning plugin init. Hand it the game's untouched create-info.
  PFN_vkCreateDevice proxy = SlProxyCreateDevice();
  if(!proxy){ Log("CreateDeviceWithSL: no interposer vkCreateDevice"); return VK_ERROR_INITIALIZATION_FAILED; }
  FetchSLRequirements();   // logging only - the interposer handles its own requirements
  Log("CreateDeviceWithSL: passthrough to interposer (no manual surgery)");
  VkResult r = proxy(pd, ci, a, out);
  Log("CreateDeviceWithSL: interposer vkCreateDevice -> %d", (int)r);
  return r;
}

void OnDeviceCreated(){
  if(!EnsureSlInit()) { Log("OnDeviceCreated: slInit failed, skipping SL device setup"); return; }

  // Interposer owns the device: it registered device+queues during its vkCreateDevice.
  // slSetVulkanInfo is for the manual path only and crashed when combined with interposer
  // ownership - skip it entirely.
  Log("interposer owns device - slSetVulkanInfo skipped");

  if(!EnsureFeatureFunctions()) { Log("OnDeviceCreated: feature function resolution failed"); return; }

  // DLSS-G requires Reflex ON. Set Reflex before enabling DLSS-G.
  sl::ReflexOptions ro{};
  ro.mode = sl::ReflexMode::eLowLatencyWithBoost;   // PureDark config: mReflexMode=2 (Boost), no cap
  sl::Result rReflex = p_slReflexSetOptions(ro);
  Log("slReflexSetOptions -> %d", (int)rReflex);

  sl::DLSSGOptions o{};
  o.mode = sl::DLSSGMode::eOn;
  o.numFramesToGenerate = 3;   // PureDark config: mDLSSGFrames=4 -> 3 generated (x4)
  sl::ViewportHandle vp{0};
  sl::Result rDlssg = p_slDLSSGSetOptions(vp, o);
  Log("slDLSSGSetOptions -> %d", (int)rDlssg);
}

void PollDLSSGState(){
  if(!p_slDLSSGGetState) return;
  sl::DLSSGState st{};
  sl::DLSSGOptions o{};
  o.mode = sl::DLSSGMode::eOn;
  o.numFramesToGenerate = 3;   // PureDark config: mDLSSGFrames=4 -> 3 generated (x4)
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
