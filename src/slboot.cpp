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
  PFN_vkCreateDevice proxy = SlProxyCreateDevice();
  if(!proxy){ Log("CreateDeviceWithSL: no interposer vkCreateDevice"); return VK_ERROR_INITIALIZATION_FAILED; }
  FetchSLRequirements();
  if(!g_reqs.valid){ Log("CreateDeviceWithSL: no SL reqs -> plain proxy create"); return proxy(pd,ci,a,out); }

  auto pQFP = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)GetProcAddress(
      GetModuleHandleA("vulkan-1.dll"), "vkGetPhysicalDeviceQueueFamilyProperties");
  uint32_t famCount=0; if(pQFP) pQFP(pd,&famCount,nullptr);
  std::vector<VkQueueFamilyProperties> fams(famCount); if(pQFP&&famCount) pQFP(pd,&famCount,fams.data());
  auto findFamily=[&](VkQueueFlags req, VkQueueFlags absent)->uint32_t{
    uint32_t fb=~0u;
    for(uint32_t i=0;i<famCount;i++){ if((fams[i].queueFlags&req)!=req) continue;
      if((fams[i].queueFlags&absent)==0) return i; if(fb==~0u) fb=i; }
    return fb; };

  // extensions (dedup-add)
  std::vector<const char*> exts(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames+ci->enabledExtensionCount);
  for(auto& w: g_reqs.devExt){ bool have=false; for(auto e: exts) if(w==e){have=true;break;} if(!have) exts.push_back(w.c_str()); }

  // queues (copy game's, append SL's compute + OFA)
  std::vector<VkDeviceQueueCreateInfo> queues(ci->pQueueCreateInfos, ci->pQueueCreateInfos+ci->queueCreateInfoCount);
  std::vector<std::vector<float>> prio;
  bool qfail=false;
  auto addQ=[&](uint32_t family, uint32_t extra, uint32_t& outIdx){
    if(extra==0||family==~0u){ if(extra>0) qfail=true; return; }
    for(auto& q: queues){ if(q.queueFamilyIndex!=family) continue;
      if(q.queueCount+extra>fams[family].queueCount){ qfail=true; return; }
      outIdx=q.queueCount; prio.emplace_back(q.queueCount+extra,1.0f);
      if(q.pQueuePriorities) std::copy(q.pQueuePriorities,q.pQueuePriorities+q.queueCount,prio.back().begin());
      q.queueCount+=extra; q.pQueuePriorities=prio.back().data(); return; }
    if(extra>fams[family].queueCount){ qfail=true; return; }
    outIdx=0; prio.emplace_back(extra,1.0f);
    queues.push_back(VkDeviceQueueCreateInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,nullptr,0,family,extra,prio.back().data()}); };

  g_slots = SLSlots{};
  g_slots.gfxFamily = findFamily(VK_QUEUE_GRAPHICS_BIT,0);
  if(g_reqs.gfxQ>0) addQ(g_slots.gfxFamily, g_reqs.gfxQ, g_slots.gfxIndex);
  if(g_reqs.compQ>0){ g_slots.compFamily=findFamily(VK_QUEUE_COMPUTE_BIT,VK_QUEUE_GRAPHICS_BIT); addQ(g_slots.compFamily,g_reqs.compQ,g_slots.compIndex); }
  if(g_reqs.ofaQ>0){ g_slots.ofaFamily=findFamily(VK_QUEUE_OPTICAL_FLOW_BIT_NV,0); addQ(g_slots.ofaFamily,g_reqs.ofaQ,g_slots.ofaIndex); }

  // features 1.2/1.3 (prepend SL's; game rarely chains these in BG3) + OFA feature.
  // NOTE: getVkPhysicalDeviceVulkan1x Features dereferences the names array - never pass
  // nullptr with a nonzero count (that was a crash right after 'SL reqs aggregated').
  std::vector<const char*> n12,n13;
  for(auto&x:g_reqs.f12) n12.push_back(x.c_str());
  for(auto&x:g_reqs.f13) n13.push_back(x.c_str());
  auto slF12 = sl::getVkPhysicalDeviceVulkan12Features((uint32_t)n12.size(), n12.empty()?nullptr:n12.data());
  auto slF13 = sl::getVkPhysicalDeviceVulkan13Features((uint32_t)n13.size(), n13.empty()?nullptr:n13.data());
  VkPhysicalDeviceOpticalFlowFeaturesNV slOFA{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV };
  bool wantOFA = (g_reqs.ofaQ>0) && (g_slots.ofaFamily!=~0u); if(wantOFA) slOFA.opticalFlow=VK_TRUE;

  VkDeviceCreateInfo ext = *ci;
  if(!g_reqs.f12.empty()){ slF12.pNext=const_cast<void*>(ext.pNext); ext.pNext=&slF12; }
  if(!g_reqs.f13.empty()){ slF13.pNext=const_cast<void*>(ext.pNext); ext.pNext=&slF13; }
  if(wantOFA){ slOFA.pNext=const_cast<void*>(ext.pNext); ext.pNext=&slOFA; }
  ext.enabledExtensionCount=(uint32_t)exts.size(); ext.ppEnabledExtensionNames=exts.data();
  ext.queueCreateInfoCount=(uint32_t)queues.size(); ext.pQueueCreateInfos=queues.data();

  if(qfail){ Log("CreateDeviceWithSL: queue surgery failed limits -> plain proxy create"); return proxy(pd,ci,a,out); }
  Log("CreateDeviceWithSL: extended (+%u ext) g=%u@%u c=%u@%u ofa=%u@%u",
      (unsigned)(exts.size()-ci->enabledExtensionCount), g_slots.gfxFamily,g_slots.gfxIndex,
      g_slots.compFamily,g_slots.compIndex,g_slots.ofaFamily,g_slots.ofaIndex);
  VkResult r = proxy(pd,&ext,a,out);
  Log("CreateDeviceWithSL: interposer vkCreateDevice -> %d", (int)r);
  if(r!=VK_SUCCESS){ Log("CreateDeviceWithSL: extended failed -> plain proxy create"); return proxy(pd,ci,a,out); }
  return r;
}

void OnDeviceCreated(){
  if(!EnsureSlInit()) { Log("OnDeviceCreated: slInit failed, skipping SL device setup"); return; }

  sl::VulkanInfo vi{};
  vi.device = fgvk::gDevice;
  vi.instance = fgvk::gInstance;
  vi.physicalDevice = fgvk::gPhysicalDevice;
  vi.graphicsQueueIndex = (g_slots.gfxFamily!=~0u) ? g_slots.gfxIndex : 0;
  vi.graphicsQueueFamily = (g_slots.gfxFamily!=~0u) ? g_slots.gfxFamily : fgvk::gGraphicsFamily;
  vi.computeQueueIndex = g_slots.compIndex; vi.computeQueueFamily = (g_slots.compFamily!=~0u)?g_slots.compFamily:fgvk::gGraphicsFamily;
  vi.opticalFlowQueueIndex = g_slots.ofaIndex; vi.opticalFlowQueueFamily = (g_slots.ofaFamily!=~0u)?g_slots.ofaFamily:0;
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
