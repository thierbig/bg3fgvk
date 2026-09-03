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
  // eUseManualHooking IS REQUIRED for the GIPA-interposition model: WE hook vkGetInstanceProcAddr
  // and drive the interposer. Without this flag, slInit tries to install its OWN Vulkan
  // interception, which collides with our in-place GIPA Detour and HANGS slInit (observed: log
  // stops at 'resolved slInit=', no sl.log, game hangs). Manual hooking = slInit skips self-
  // hooking because we're driving. (The earlier BG3SE crash was manual-hooking + a DIFFERENT
  // hybrid that also called slSetVulkanInfo; here we own the whole GIPA chain instead.)
  // Explicit flags: default includes eAllowOTA|eLoadDownloadedPlugins - OTA off (matched 2.12.0).
  p.flags = sl::PreferenceFlags::eDisableCLStateTracking
          | sl::PreferenceFlags::eUseManualHooking
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
void* SlProxyFn(const char* name){
  if(!g_sl){ g_sl=GetModuleHandleA("sl.interposer.dll"); if(!g_sl) g_sl=LoadLibraryA("sl.interposer.dll"); }
  return g_sl ? (void*)GetProcAddress(g_sl, name) : nullptr;
}

void OnDeviceCreated(){
  // Full interposition: the interposer created + owns the device and its queues (its own
  // surgery). NO slSetVulkanInfo, NO device surgery - those double-register an interposer
  // device and caused every crash/queue-rejection before. We only enable the features.
  if(!EnsureSlInit()) { Log("OnDeviceCreated: slInit not ready"); return; }
  if(!EnsureFeatureFunctions()) { Log("OnDeviceCreated: feature function resolution failed"); return; }

  // DLSS-G requires Reflex ON. Set Reflex now; DLSS-G is enabled LATER by the stability gate.
  sl::ReflexOptions ro{};
  ro.mode = sl::ReflexMode::eLowLatencyWithBoost;   // PureDark config: mReflexMode=2 (Boost), no cap
  sl::Result rReflex = p_slReflexSetOptions(ro);
  Log("slReflexSetOptions -> %d (DLSS-G deferred to the stability gate)", (int)rReflex);
}

// Enable/disable DLSS-G generation. Driven by the present-thread stability gate (vkhooks):
// enabling DLSS-G during the world-load's frame-time hitches freezes the game (user confirmed:
// alt-tab, which deactivates DLSS-G, is the ONLY way it loads). So we hold generation OFF until
// the frame rate is smooth (in-world) and turn it off again if it goes hitchy.
void SetDLSSGeneration(bool on){
  if(!p_slDLSSGSetOptions) return;
  sl::DLSSGOptions o{};
  o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
  o.numFramesToGenerate = 3;   // x4
  sl::ViewportHandle vp{0};
  sl::Result r = p_slDLSSGSetOptions(vp, o);
  Log("SetDLSSGeneration(%d) -> %d", (int)on, (int)r);
}

void PollDLSSGState(){
  if(!p_slDLSSGGetState) return;
  sl::DLSSGState st{};
  sl::DLSSGOptions o{};
  o.mode = sl::DLSSGMode::eOn;
  o.numFramesToGenerate = 3;   // x4
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
