// DLSS-G input pipeline: snoop the game's DLSS-SR EvaluateFeature for depth/mvec/jitter, tag
// Depth, MotionVectors and HUDLessColor (= the DLSS-SR output, PureDark's recipe), push
// sl::Constants (synthetic perspective, identity reprojection - also his recipe), and run the
// PCL/Reflex marker ladder across the eval thread and the present thread on ONE frame token per
// frame (see the frame-token section below for why, and how the two threads hand it over).
// Full Vulkan resource descriptions are mandatory (an undefined format aborts inside SL).
#include "inputs.h"
#include "mvecscale.h"
#include "log.h"
#include "slboot.h"
#include "vkhooks.h"
#include "config.h"
#include <windows.h>
#include <detours.h>
#include <sl.h>
#include <sl_consts.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <cstring>
#include <atomic>
#include <mutex>

// ---- minimal NGX surface (no vendored NGX SDK; layouts match the driver ABI) ----------
typedef int NVSDK_NGX_Result;
static const NVSDK_NGX_Result NGX_Success = 0x1;
struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;
typedef void (*PFN_NVSDK_NGX_ProgressCallback_C)(float, bool*);
typedef NVSDK_NGX_Result (__cdecl *PFN_NgxGetVoidPointer)(NVSDK_NGX_Parameter*, const char*, void**);
typedef NVSDK_NGX_Result (__cdecl *PFN_NgxGetF)(NVSDK_NGX_Parameter*, const char*, float*);
typedef NVSDK_NGX_Result (__cdecl *PFN_NgxGetUI)(NVSDK_NGX_Parameter*, const char*, unsigned int*);
typedef NVSDK_NGX_Result (__cdecl *PFN_NgxGetI)(NVSDK_NGX_Parameter*, const char*, int*);
// NVSDK_NGX_DLSS_Feature_Flags (nvsdk_ngx_defs.h) - read from the game's own DLSS-SR create call
enum : int { kNgxFlagIsHDR=1<<0, kNgxFlagMVLowRes=1<<1, kNgxFlagMVJittered=1<<2, kNgxFlagDepthInverted=1<<3,
             kNgxFlagDoSharpening=1<<4, kNgxFlagAutoExposure=1<<5, kNgxFlagAlphaUpscaling=1<<6 };
typedef NVSDK_NGX_Result (__cdecl *PFN_NgxEvaluateFeature)(VkCommandBuffer, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback_C);

struct NVSDK_NGX_ImageViewInfo_VK {
  VkImageView ImageView; VkImage Image; VkImageSubresourceRange SubresourceRange;
  VkFormat Format; unsigned int Width; unsigned int Height;
};
struct NVSDK_NGX_BufferInfo_VK { VkBuffer Buffer; unsigned int SizeInBytes; };
struct NVSDK_NGX_Resource_VK {
  union { NVSDK_NGX_ImageViewInfo_VK ImageViewInfo; NVSDK_NGX_BufferInfo_VK BufferInfo; } Resource;
  int Type; bool ReadWrite;
};

namespace fgvk {

// ---- state ----------------------------------------------------------------------------
static PFN_NgxEvaluateFeature o_NgxEvaluate{};
typedef NVSDK_NGX_Result (__cdecl *PFN_NgxCreateFeature)(VkCommandBuffer, unsigned int /*featureId*/,
    NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
static PFN_NgxCreateFeature o_NgxCreateFeature{};
static PFN_NgxGetVoidPointer p_GetVoidPointer{};
static PFN_NgxGetF p_GetF{};
static PFN_NgxGetUI p_GetUI{};
static PFN_NgxGetI p_GetI{};
static std::atomic<int> g_srCreateFlags{-1};   // the game's DLSS-SR feature flags (-1 = not seen yet)
static bool g_hooked=false; static int g_probeDelay=0;
static bool g_logHookLive=false, g_logFirstInputs=false, g_logTagFail=false, g_logConstFail=false,
            g_logHudTag=false, g_logTokenShift=false, g_logSubmitOk=false;

struct FrameInputs {
  bool valid=false;
  VkImage depthImage{}, mvecImage{};
  VkImageView depthView{}, mvecView{};
  VkFormat depthFormat{}, mvecFormat{};
  uint32_t depthW=0, depthH=0, mvecW=0, mvecH=0;
  float jitterX=0, jitterY=0, mvScaleX=0, mvScaleY=0;
  unsigned int reset=0;
};
static FrameInputs g_in;
static inline double NowMs(){ LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return 1000.0*(double)c.QuadPart/(double)f.QuadPart; }
static double g_maxTagMs=0, g_maxConstMs=0, g_maxHudMs=0, g_lastEvalMs=0, g_maxGapMs=0, g_maxSleepMs=0; static uint32_t g_evalN=0;
static sl::FrameToken* g_lastToken=nullptr;

// ---- frame token lifecycle ------------------------------------------------------------
// ONE token per frame, shared by the marker ladder, the tags and the constants. Verified in
// the SL 2.12 source: slGetNewFrameToken(nullptr) advances a global counter on EVERY call, and
// sl.reflex publishes the ePresentStart marker's index as latency.markerPresentFrame - the
// index DLSS-G uses at present time to look up that frame's tags+constants (sl.common's
// frame-based tag store has no fallback). The old "+1 shift" dates from calling the token
// function twice per frame; with one call it made every present consume the PREVIOUS frame's
// inputs (sl.log: "Unable to find 'common' constants for frame 1 ... using last set for frame 2").
//
// BG3 runs the DLSS-SR evaluate and the present on DIFFERENT worker threads. A queue of frame
// slots makes the hand-off order-independent: the eval thread starts a new frame (new token)
// whenever the newest slot already carries a submission (i.e. it ran ahead of the previous
// frame's present), and the present thread always presents the OLDEST slot. Depth is capped
// at 3 pending frames - Streamline keeps MAX_FRAMES_IN_FLIGHT=6 handles alive, so nothing we
// hold can be recycled under us.
static std::atomic<bool> g_evalSeen{false};     // DLSS-SR filed tags+constants since the last present
static std::atomic<uint32_t> g_evalTid{0};
// Reflex sleep is REQUIRED by the Reflex checklist regardless of mode; it was dropped earlier on
// the (wrong) belief that BG3 drives Reflex natively - bg3.exe has no Reflex integration at all.
// fgvk.ini ReflexSleep=0 is the kill switch if the driver's pacing misbehaves.
#define kReflexSleep (Cfg().reflexSleep)

struct FrameSlot { sl::FrameToken* tok=nullptr; uint32_t index=0; bool submitted=false; bool simStarted=false; bool midMarkers=false; };
static std::mutex g_qMutex;                       // guards the ring below (eval thread vs present thread)
static const uint32_t kQCap = 4;
static FrameSlot g_q[kQCap]; static uint32_t g_qHead=0, g_qCount=0;   // head = next to present
static FrameSlot* QFront(){ return g_qCount ? &g_q[g_qHead] : nullptr; }
static FrameSlot* QBack(){ return g_qCount ? &g_q[(g_qHead+g_qCount-1)%kQCap] : nullptr; }
static FrameSlot* QFind(uint32_t index){ for(uint32_t i=0;i<g_qCount;i++){ FrameSlot& s=g_q[(g_qHead+i)%kQCap]; if(s.index==index) return &s; } return nullptr; }
static void QPop(){ if(g_qCount){ g_qHead=(g_qHead+1)%kQCap; g_qCount--; } }
// New SL frame token (advances SL's counter) pushed as the newest slot; drops the oldest if the
// evals are outrunning the presents (would mean two DLSS-SR evaluates per presented frame).
static FrameSlot* QPushNew(){
  auto& fns = GetSlFns(); sl::FrameToken* t=nullptr;
  if(!fns.getNewFrameToken || fns.getNewFrameToken(t,nullptr)!=sl::Result::eOk || !t) return nullptr;
  if(g_qCount>=3){ static bool l=false; if(!l){ l=true; Log("frame token queue full - evals outrunning presents, dropping oldest frame %u", g_q[g_qHead].index); } QPop(); }
  FrameSlot& s = g_q[(g_qHead+g_qCount)%kQCap]; s = FrameSlot{ t, (uint32_t)*t, false, false, false }; g_qCount++;
  return &s;
}
static void SimStart(FrameSlot& s){
  if(s.simStarted) return; s.simStarted=true;
  auto& fns = GetSlFns(); if(fns.pclSetMarker && s.tok) fns.pclSetMarker(sl::PCLMarker::eSimulationStart,*s.tok);
}
// SimulationEnd + RenderSubmitStart: at the DLSS-SR evaluate when there is one (mid render
// submit), otherwise just before present so the ladder stays complete and ordered.
static void MidMarkers(FrameSlot& s){
  if(s.midMarkers) return; s.midMarkers=true;
  auto& fns = GetSlFns(); if(!fns.pclSetMarker || !s.tok) return;
  fns.pclSetMarker(sl::PCLMarker::eSimulationEnd,*s.tok);
  fns.pclSetMarker(sl::PCLMarker::eRenderSubmitStart,*s.tok);
}

// UIColorAndAlpha transparent image (session lifetime)
static VkImage g_uiImage{}; static VkDeviceMemory g_uiMem{}; static VkImageView g_uiView{};
static uint32_t g_uiW=0, g_uiH=0; static bool g_uiFailed=false;

// ---- module probe ---------------------------------------------------------------------
template <typename Fn> static bool forEachNgxModule(Fn&& fn){
  static const wchar_t* known[] = { L"sl.interposer.dll", L"sl.dlss.dll", L"sl.dlss_g.dll",
                                    L"nvngx_dlss.dll", L"nvngx.dll", L"_nvngx.dll" };
  for (auto n : known){ HMODULE m=GetModuleHandleW(n); if(m && fn(m,n)) return true; }
  using PEnum = BOOL(WINAPI*)(HANDLE,HMODULE*,DWORD,LPDWORD);
  static PEnum pEnum = (PEnum)GetProcAddress(GetModuleHandleA("kernel32.dll"),"K32EnumProcessModules");
  if(!pEnum) return false;
  DWORD needed=0; if(!pEnum(GetCurrentProcess(),nullptr,0,&needed)) return false;
  HMODULE mods[1024]; DWORD cap = needed < sizeof(mods) ? needed : (DWORD)sizeof(mods);
  if(!pEnum(GetCurrentProcess(),mods,cap,&needed)) return false;
  DWORD count = (needed<cap?needed:cap)/sizeof(HMODULE);
  for(DWORD i=0;i<count;i++){ wchar_t p[MAX_PATH]{}; if(!GetModuleFileNameW(mods[i],p,MAX_PATH)) continue;
    if(fn(mods[i],p)) return true; }
  return false;
}

// ---- resource helpers (ported) --------------------------------------------------------
static void DescribeResource(sl::Resource& r, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage){
  r.width=w; r.height=h; r.nativeFormat=(uint32_t)fmt; r.mipLevels=1; r.arrayLayers=1; r.flags=0;
  r.usage=(uint32_t)usage;
}

static bool EnsureUIColorImage(VkCommandBuffer cmd, uint32_t w, uint32_t h){
  if(g_uiFailed) return false;
  if(g_uiImage && g_uiW==w && g_uiH==h) return true;
  if(!gDevice || !gPhysicalDevice) return false;
  if(g_uiImage){ g_uiImage=VK_NULL_HANDLE; g_uiView=VK_NULL_HANDLE; } // leak on resize (rare, safe)

  auto vkCreateImage_ = (PFN_vkCreateImage)DeviceFn("vkCreateImage");
  auto vkGetImageMemoryRequirements_ = (PFN_vkGetImageMemoryRequirements)DeviceFn("vkGetImageMemoryRequirements");
  auto vkAllocateMemory_ = (PFN_vkAllocateMemory)DeviceFn("vkAllocateMemory");
  auto vkBindImageMemory_ = (PFN_vkBindImageMemory)DeviceFn("vkBindImageMemory");
  auto vkCreateImageView_ = (PFN_vkCreateImageView)DeviceFn("vkCreateImageView");
  auto vkDestroyImage_ = (PFN_vkDestroyImage)DeviceFn("vkDestroyImage");
  auto vkCmdPipelineBarrier_ = (PFN_vkCmdPipelineBarrier)DeviceFn("vkCmdPipelineBarrier");
  auto vkCmdClearColorImage_ = (PFN_vkCmdClearColorImage)DeviceFn("vkCmdClearColorImage");
  auto vkGetPDMemProps_ = (PFN_vkGetPhysicalDeviceMemoryProperties)LoaderFn("vkGetPhysicalDeviceMemoryProperties");
  if(!vkCreateImage_||!vkGetImageMemoryRequirements_||!vkAllocateMemory_||!vkBindImageMemory_
     ||!vkCreateImageView_||!vkCmdPipelineBarrier_||!vkCmdClearColorImage_||!vkGetPDMemProps_){
    g_uiFailed=true; Log("UI image: vulkan fns unresolved - UI tag disabled"); return false; }

  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType=VK_IMAGE_TYPE_2D; ici.format=VK_FORMAT_R8G8B8A8_UNORM; ici.extent={w,h,1};
  ici.mipLevels=1; ici.arrayLayers=1; ici.samples=VK_SAMPLE_COUNT_1_BIT; ici.tiling=VK_IMAGE_TILING_OPTIMAL;
  ici.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ici.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage img{};
  if(vkCreateImage_(gDevice,&ici,nullptr,&img)!=VK_SUCCESS){ g_uiFailed=true; Log("UI image create failed"); return false; }
  VkMemoryRequirements mr{}; vkGetImageMemoryRequirements_(gDevice,img,&mr);
  VkPhysicalDeviceMemoryProperties mp{}; vkGetPDMemProps_(gPhysicalDevice,&mp);
  uint32_t idx=UINT32_MAX;
  for(uint32_t i=0;i<mp.memoryTypeCount;i++)
    if((mr.memoryTypeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){ idx=i; break; }
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize=mr.size; mai.memoryTypeIndex=idx;
  VkDeviceMemory mem{};
  if(idx==UINT32_MAX || vkAllocateMemory_(gDevice,&mai,nullptr,&mem)!=VK_SUCCESS
     || vkBindImageMemory_(gDevice,img,mem,0)!=VK_SUCCESS){
    vkDestroyImage_(gDevice,img,nullptr); g_uiFailed=true; Log("UI image mem alloc/bind failed"); return false; }
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image=img; vci.viewType=VK_IMAGE_VIEW_TYPE_2D; vci.format=ici.format;
  vci.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  VkImageView view{};
  if(vkCreateImageView_(gDevice,&vci,nullptr,&view)!=VK_SUCCESS){ g_uiFailed=true; Log("UI image view failed"); return false; }

  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toDst.srcAccessMask=0; toDst.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
  toDst.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; toDst.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toDst.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; toDst.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  toDst.image=img; toDst.subresourceRange=range;
  vkCmdPipelineBarrier_(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&toDst);
  VkClearColorValue clear{};  // fully transparent
  vkCmdClearColorImage_(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
  VkImageMemoryBarrier toRead=toDst;
  toRead.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; toRead.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
  toRead.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; toRead.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier_(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&toRead);
  g_uiImage=img; g_uiMem=mem; g_uiView=view; g_uiW=w; g_uiH=h;
  Log("UIColorAndAlpha transparent layer created %ux%u (4th required DLSS-G tag live)", w, h);
  return true;
}

// ---- constants (identity reprojection; synthetic perspective) -------------------------
static sl::float4x4 MakeIdentity(){
  sl::float4x4 m; m.setRow(0,{1,0,0,0}); m.setRow(1,{0,1,0,0}); m.setRow(2,{0,0,1,0}); m.setRow(3,{0,0,0,1});
  return m;
}
static sl::float4x4 MakePerspective(float fovRad, float aspect, float zn, float zf){
  // Row-major D3D-style perspective (viewToClip). Identity reprojection carries the temporal
  // load in phase A; this feeds SL's validation + OFA setup.
  float f = 1.0f / tanf(fovRad*0.5f);
  sl::float4x4 m{};
  m.setRow(0,{f/aspect,0,0,0});
  m.setRow(1,{0,f,0,0});
  m.setRow(2,{0,0,zf/(zf-zn),1});
  m.setRow(3,{0,0,-zn*zf/(zf-zn),0});
  return m;
}
static sl::float4x4 MakePerspectiveInv(float fovRad, float aspect, float zn, float zf){
  float f = 1.0f / tanf(fovRad*0.5f);
  float A = zf/(zf-zn), B = -zn*zf/(zf-zn);
  sl::float4x4 m{};
  m.setRow(0,{aspect/f,0,0,0});
  m.setRow(1,{0,1.0f/f,0,0});
  m.setRow(2,{0,0,0,1.0f/B});
  m.setRow(3,{0,0,1,-A/B});
  return m;
}

// ---- frame submission (ported SubmitFrameData) ----------------------------------------
static bool SubmitFrameData(VkCommandBuffer cmd){
  g_lastToken=nullptr;
  auto& fns = GetSlFns();
  if(!fns.getNewFrameToken || !fns.setTagForFrame || !fns.setConstants) return false;

  // Take the newest frame slot; if it already carries a submission we have run ahead of the
  // previous frame's present (pipelined engine) and this evaluate belongs to a NEW frame.
  sl::FrameToken* token=nullptr; uint32_t slotIndex=0;
  { std::lock_guard<std::mutex> lk(g_qMutex);
    FrameSlot* b = QBack();
    if(!b || b->submitted){ b = QPushNew(); if(!b) return false; SimStart(*b); }
    MidMarkers(*b);   // SimulationEnd + RenderSubmitStart (we are inside the render submit)
    token = b->tok; slotIndex = b->index; }
  g_evalTid.store((uint32_t)GetCurrentThreadId());
  if(!g_logTokenShift){ g_logTokenShift=true;
    Log("FG frame token %u: tags+constants+present markers share one token per frame (no +1 shift) evalTid=%lu",
        (uint32_t)*token, (unsigned long)GetCurrentThreadId()); }
  g_lastToken=token;

  sl::ViewportHandle vp{0};
  sl::Resource depthRes(sl::ResourceType::eTex2d,(void*)g_in.depthImage,(void*)nullptr,
      (void*)g_in.depthView,(uint32_t)VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
  sl::Resource mvecRes(sl::ResourceType::eTex2d,(void*)g_in.mvecImage,(void*)nullptr,
      (void*)g_in.mvecView,(uint32_t)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  DescribeResource(depthRes,g_in.depthW,g_in.depthH,g_in.depthFormat,
      VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
  DescribeResource(mvecRes,g_in.mvecW,g_in.mvecH,g_in.mvecFormat,
      VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  sl::Extent depthExtent{0,0,g_in.depthW,g_in.depthH};
  sl::Extent mvecExtent{0,0,g_in.mvecW,g_in.mvecH};
  sl::ResourceTag tags[] = {
    sl::ResourceTag(&depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &depthExtent),
    sl::ResourceTag(&mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &mvecExtent),
  };
  double _t0=NowMs();
  auto tagRes = fns.setTagForFrame(*token, vp, tags, 2, reinterpret_cast<sl::CommandBuffer*>(cmd));
  double _tag=NowMs()-_t0; if(_tag>g_maxTagMs) g_maxTagMs=_tag;
  if(tagRes!=sl::Result::eOk && !g_logTagFail){ g_logTagFail=true; Log("slSetTagForFrame(depth/mvec) failed: %d",(int)tagRes); }

  float aspect = g_in.mvecH ? (float)g_in.mvecW/(float)g_in.mvecH : 1.7778f;
  sl::Constants c{};
  c.cameraViewToClip = MakePerspective(0.84f, aspect, 0.05f, 10000.f);
  c.clipToCameraView = MakePerspectiveInv(0.84f, aspect, 0.05f, 10000.f);
  c.clipToPrevClip = MakeIdentity();     // identity reprojection: proven clean baseline
  c.prevClipToClip = MakeIdentity();
  c.cameraPinholeOffset = sl::float2(0.f,0.f);
  c.jitterOffset = sl::float2(g_in.jitterX, g_in.jitterY);
  // Streamline normalises: it multiplies this by the mvec extent before the driver sees it, so
  // raw NGX pixel-space values (-1,-1) must be divided by the extent (see mvecscale.h).
  MvecScale ms = MvecScaleForStreamline(g_in.mvScaleX, g_in.mvScaleY, g_in.mvecW, g_in.mvecH, Cfg().mvecScaleNormalized);
  c.mvecScale = sl::float2(ms.x, ms.y);
  c.cameraPos = sl::float3(0,0,0);
  c.cameraUp = sl::float3(0,1,0);
  c.cameraRight = sl::float3(1,0,0);
  c.cameraFwd = sl::float3(0,0,1);
  c.cameraNear = 0.05f; c.cameraFar = 10000.f;
  c.cameraFOV = 0.84f; c.cameraAspectRatio = aspect;
  // Flags straight from the game's DLSS-SR create call when we saw it (PureDark's capture agrees:
  // DepthInverted=1); until then the captured recipe's values.
  int srf = g_srCreateFlags.load();
  c.depthInverted = (srf < 0 || (srf & kNgxFlagDepthInverted)) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c.cameraMotionIncluded = sl::Boolean::eTrue;
  c.motionVectors3D = sl::Boolean::eFalse;
  c.motionVectorsJittered = (srf > 0 && (srf & kNgxFlagMVJittered)) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c.reset = g_in.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  double _c0=NowMs();
  auto constRes = fns.setConstants(c, *token, vp);
  double _cms=NowMs()-_c0; if(_cms>g_maxConstMs) g_maxConstMs=_cms;
  if(constRes!=sl::Result::eOk && !g_logConstFail){ g_logConstFail=true; Log("slSetConstants failed: %d",(int)constRes); }
  if(tagRes==sl::Result::eOk && constRes==sl::Result::eOk && !g_logSubmitOk){
    g_logSubmitOk=true; Log("FG inputs submitted ok (depth %ux%u, mvec %ux%u, jitter %.4f/%.4f mvScale %.1f/%.1f)",
      g_in.depthW,g_in.depthH,g_in.mvecW,g_in.mvecH,g_in.jitterX,g_in.jitterY,g_in.mvScaleX,g_in.mvScaleY); }
  double _now=NowMs(); double _gap=_now-g_lastEvalMs; if(g_lastEvalMs>0 && _gap>g_maxGapMs) g_maxGapMs=_gap; g_lastEvalMs=_now;
  if((++g_evalN % 120)==0){ Log("TIMING: maxTag=%.1fms maxConst=%.1fms maxHud=%.1fms maxReflexSleep=%.1fms maxFrameGap=%.1fms (over 120 evals)",
      g_maxTagMs,g_maxConstMs,g_maxHudMs,g_maxSleepMs,g_maxGapMs); g_maxTagMs=g_maxConstMs=g_maxHudMs=g_maxSleepMs=g_maxGapMs=0; }
  bool ok = tagRes==sl::Result::eOk && constRes==sl::Result::eOk;
  if(ok){
    { std::lock_guard<std::mutex> lk(g_qMutex); if(FrameSlot* s=QFind(slotIndex)) s->submitted=true; }
    g_evalSeen.store(true);
  }
  return ok;
}

// Optional tags (fgvk.ini TagHUDLess / TagUI). Default OFF = backbuffer only, PureDark parity:
// his captured recipe carries no UI buffer and only occasionally a HUD-less one. The DLSS-SR
// output is pre-post-processing (linear/HDR before tonemap); the guide requires HUD-less to be
// in the SAME color space as the backbuffer, so feeding it can produce halos/ghosting.
static void TagHUDLessColor(VkCommandBuffer cmd, VkImage image, VkImageView view, VkFormat fmt,
                            uint32_t w, uint32_t h){
  auto& fns = GetSlFns();
  if(!fns.setTagForFrame || !g_lastToken) return;
  if(!g_logHudTag){ g_logHudTag=true;
    Log("DLSS-SR output %ux%u fmt=%d; optional tags: HUDLess=%d UI=%d", w, h, (int)fmt, (int)Cfg().tagHudless, (int)Cfg().tagUI); }
  if(!Cfg().tagHudless && !Cfg().tagUI) return;
  sl::Resource colorRes(sl::ResourceType::eTex2d,(void*)image,(void*)nullptr,
      (void*)view,(uint32_t)VK_IMAGE_LAYOUT_GENERAL);   // NGX writes Output as compute storage
  DescribeResource(colorRes,w,h,fmt,
      VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  sl::Extent extent{0,0,w,h};
  bool haveUI = Cfg().tagUI && EnsureUIColorImage(cmd,w,h);
  sl::Resource uiRes(sl::ResourceType::eTex2d,(void*)g_uiImage,(void*)g_uiMem,
      (void*)g_uiView,(uint32_t)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  DescribeResource(uiRes,w,h,VK_FORMAT_R8G8B8A8_UNORM,
      VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  sl::ResourceTag tags[2]; uint32_t n=0;
  if(Cfg().tagHudless) tags[n++] = sl::ResourceTag(&colorRes, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &extent);
  if(haveUI)           tags[n++] = sl::ResourceTag(&uiRes, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &extent);
  if(!n) return;
  sl::ViewportHandle vp{0};
  double _h0=NowMs();
  auto res = fns.setTagForFrame(*g_lastToken, vp, tags, n, reinterpret_cast<sl::CommandBuffer*>(cmd));
  double _hms=NowMs()-_h0; if(_hms>g_maxHudMs) g_maxHudMs=_hms;
  static bool l2=false; if(!l2){ l2=true;
    if(res==sl::Result::eOk) Log("optional tags submitted (%u) %ux%u", n, w, h);
    else Log("slSetTagForFrame(HUDLess/UI) failed: %d",(int)res); }
}

// ---- NGX inputs read (ported readNgxFrameInputs) --------------------------------------
static void ReadFrameInputs(const NVSDK_NGX_Parameter* params){
  g_in.valid=false;
  auto* p = const_cast<NVSDK_NGX_Parameter*>(params);
  auto readRes=[&](const char* name, VkImage& img, VkImageView& view, VkFormat& fmt, uint32_t& w, uint32_t& h)->bool{
    void* ptr=nullptr;
    if(p_GetVoidPointer(p,name,&ptr)!=NGX_Success || !ptr) return false;
    auto* r=reinterpret_cast<NVSDK_NGX_Resource_VK*>(ptr);
    auto& iv=r->Resource.ImageViewInfo;
    if(!iv.Image||!iv.ImageView||iv.Format==VK_FORMAT_UNDEFINED) return false;
    img=iv.Image; view=iv.ImageView; fmt=iv.Format; w=iv.Width; h=iv.Height; return true;
  };
  bool depthOk=readRes("Depth",g_in.depthImage,g_in.depthView,g_in.depthFormat,g_in.depthW,g_in.depthH);
  bool mvecOk=readRes("MotionVectors",g_in.mvecImage,g_in.mvecView,g_in.mvecFormat,g_in.mvecW,g_in.mvecH);
  g_in.jitterX=g_in.jitterY=g_in.mvScaleX=g_in.mvScaleY=0; g_in.reset=0;
  if(p_GetF){ p_GetF(p,"Jitter.Offset.X",&g_in.jitterX); p_GetF(p,"Jitter.Offset.Y",&g_in.jitterY);
              p_GetF(p,"MV.Scale.X",&g_in.mvScaleX); p_GetF(p,"MV.Scale.Y",&g_in.mvScaleY); }
  if(p_GetUI){ p_GetUI(p,"Reset",&g_in.reset); }
  if(!depthOk||!mvecOk) return;
  g_in.valid=true;
  if(!g_logFirstInputs){ g_logFirstInputs=true;
    Log("NGX inputs first valid: depth %ux%u fmt=%d, mvec %ux%u fmt=%d, jitter=(%.4f,%.4f) mvScale=(%.1f,%.1f)",
        g_in.depthW,g_in.depthH,(int)g_in.depthFormat,g_in.mvecW,g_in.mvecH,(int)g_in.mvecFormat,
        g_in.jitterX,g_in.jitterY,g_in.mvScaleX,g_in.mvScaleY); }
}

// ---- the evaluate detour --------------------------------------------------------------
static thread_local bool g_inEval=false;
static NVSDK_NGX_Result __cdecl h_NgxEvaluate(VkCommandBuffer cmd, const NVSDK_NGX_Handle* h,
    const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback_C cb){
  if(g_inEval) return o_NgxEvaluate(cmd,h,params,cb);
  g_inEval=true;
  extern std::atomic<uint32_t> g_wdEvals; extern std::atomic<int> g_wdPos;
  g_wdEvals.fetch_add(1); g_wdPos.store(10);
  if(!g_logHookLive){ g_logHookLive=true; Log("NGX EvaluateFeature hook live"); }

  // Filter out sl.dlss_g's OWN evaluates (they carry DLSSG params; ours must only snoop
  // the game's DLSS-SR). Without this, generation re-enters the snoop with its own frames.
  bool isDlssg=false;
  if(params && p_GetVoidPointer){
    void* bb=nullptr;
    if(p_GetVoidPointer(const_cast<NVSDK_NGX_Parameter*>(params),"DLSSG.Backbuffer",&bb)==NGX_Success && bb)
      isDlssg=true;
  }
  // Readback: what the driver's DLSS-G feature is actually told about the motion-vector scale
  // (after Streamline's multiply by the mvec extent). Expected -1,-1 for BG3 (PureDark's recipe).
  if(isDlssg && p_GetF){
    static float lastX=0.f, lastY=0.f; static int logged=0;
    float sx=0.f, sy=0.f; auto* pp=const_cast<NVSDK_NGX_Parameter*>(params);
    if(p_GetF(pp,"DLSSG.MvecScaleX",&sx)==NGX_Success && p_GetF(pp,"DLSSG.MvecScaleY",&sy)==NGX_Success){
      if((sx!=lastX || sy!=lastY) && logged<5){ logged++; lastX=sx; lastY=sy;
        Log("DLSS-G receives MvecScale=(%.4f,%.4f) at the driver (mvec %ux%u, ini MvecScaleNormalized=%d)",
            sx, sy, g_in.mvecW, g_in.mvecH, (int)Cfg().mvecScaleNormalized); }
    }
  }
  if(!isDlssg && params){
    ReadFrameInputs(params);
    if(cmd && g_in.valid) SubmitFrameData(cmd);
  }

  g_wdPos.store(11);
  NVSDK_NGX_Result r = o_NgxEvaluate(cmd,h,params,cb);
  g_wdPos.store(12);

  // AFTER orig: SR just wrote Output (upscaled pre-UI color) - DLSS-G's required color source.
  if(!isDlssg && r==NGX_Success && cmd && params && g_in.valid && p_GetVoidPointer){
    void* outPtr=nullptr;
    if(p_GetVoidPointer(const_cast<NVSDK_NGX_Parameter*>(params),"Output",&outPtr)==NGX_Success && outPtr){
      auto* res=reinterpret_cast<NVSDK_NGX_Resource_VK*>(outPtr);
      auto& iv=res->Resource.ImageViewInfo;
      if(iv.Image && iv.ImageView && iv.Format!=VK_FORMAT_UNDEFINED)
        TagHUDLessColor(cmd,iv.Image,iv.ImageView,iv.Format,iv.Width,iv.Height);
    }
  }
  g_inEval=false;
  g_wdPos.store(0);
  return r;
}

// Diagnostic parity with PureDark's hk_NVSDK_NGX_VULKAN_CreateFeature: log the SR feature id.
static NVSDK_NGX_Result __cdecl h_NgxCreateFeature(VkCommandBuffer cmd, unsigned int featureId,
    NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** outH){
  // featureId 1 = DLSS super resolution: its creation flags tell us how the game renders its
  // depth/mvecs (inverted depth, jittered mvecs, HDR) - the ground truth for our SL constants.
  if(featureId==1 && p && p_GetI){
    int flags=0;
    if(p_GetI(p,"DLSS.Feature.Create.Flags",&flags)==NGX_Success){
      g_srCreateFlags.store(flags);
      unsigned w=0,h=0,ow=0,oh=0; if(p_GetUI){ p_GetUI(p,"Width",&w); p_GetUI(p,"Height",&h); p_GetUI(p,"OutWidth",&ow); p_GetUI(p,"OutHeight",&oh); }
      Log("DLSS-SR create: flags=0x%x [HDR=%d MVLowRes=%d MVJittered=%d DepthInverted=%d AutoExposure=%d] render %ux%u -> %ux%u",
          flags, !!(flags&kNgxFlagIsHDR), !!(flags&kNgxFlagMVLowRes), !!(flags&kNgxFlagMVJittered), !!(flags&kNgxFlagDepthInverted),
          !!(flags&kNgxFlagAutoExposure), w,h,ow,oh);
    }
  }
  static bool logged=false; if(!logged){ logged=true; Log("NGX CreateFeature featureId=%u", featureId); }
  return o_NgxCreateFeature(cmd, featureId, p, outH);
}

void NgxProbeTick(){
  if(g_hooked) return;
  if(g_probeDelay-->0) return;
  g_probeDelay=120;
  HMODULE found{}; const wchar_t* foundName=nullptr;
  forEachNgxModule([&](HMODULE m, const wchar_t* name){
    if(!GetProcAddress(m,"NVSDK_NGX_VULKAN_EvaluateFeature")) return false;
    found=m; foundName=name; return true; });
  if(!found) return;
  o_NgxEvaluate=(PFN_NgxEvaluateFeature)GetProcAddress(found,"NVSDK_NGX_VULKAN_EvaluateFeature");
  o_NgxCreateFeature=(PFN_NgxCreateFeature)GetProcAddress(found,"NVSDK_NGX_VULKAN_CreateFeature");
  // param getters can live in a different module - probe independently
  forEachNgxModule([&](HMODULE m, const wchar_t*){
    auto gp=(PFN_NgxGetVoidPointer)GetProcAddress(m,"NVSDK_NGX_Parameter_GetVoidPointer");
    if(!gp) return false;
    p_GetVoidPointer=gp;
    p_GetF=(PFN_NgxGetF)GetProcAddress(m,"NVSDK_NGX_Parameter_GetF");
    p_GetUI=(PFN_NgxGetUI)GetProcAddress(m,"NVSDK_NGX_Parameter_GetUI");
    p_GetI=(PFN_NgxGetI)GetProcAddress(m,"NVSDK_NGX_Parameter_GetI");
    return true; });
  DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
  DetourAttach(&(PVOID&)o_NgxEvaluate,(PVOID)h_NgxEvaluate);
  if(o_NgxCreateFeature) DetourAttach(&(PVOID&)o_NgxCreateFeature,(PVOID)h_NgxCreateFeature);
  LONG rc=DetourTransactionCommit();
  g_hooked=(rc==NO_ERROR);
  Log("NGX hooks: module=%S commit=%ld eval=%p create=%p getters=%p/%p/%p",
      foundName?foundName:L"?",rc,(void*)o_NgxEvaluate,(void*)o_NgxCreateFeature,
      (void*)p_GetVoidPointer,(void*)p_GetF,(void*)p_GetUI);
}

// ---- present-thread half of the marker ladder ------------------------------------------
// Present markers MUST be on the present thread with the frame's token: sl.reflex turns our
// ePresentStart into latency.markerPresentFrame, which is how DLSS-G finds this frame's
// tags+constants (guide 8.0: "markers ePresentStart/ePresentEnd must provide correct frame
// index so that it can be matched" to the constants).
void PresentMarkersBegin(){
  auto& fns = GetSlFns();
  if(!fns.pclSetMarker) return;
  std::lock_guard<std::mutex> lk(g_qMutex);
  FrameSlot* f = QFront();
  if(!f){ f = QPushNew(); if(!f) return; }   // no DLSS-SR this frame (menu/loading/video)
  SimStart(*f); MidMarkers(*f);              // keep the ladder complete and ordered
  fns.pclSetMarker(sl::PCLMarker::eRenderSubmitEnd,*f->tok);
  fns.pclSetMarker(sl::PCLMarker::ePresentStart,*f->tok);   // -> latency.markerPresentFrame = this frame
  static bool tidChecked=false;
  if(!tidChecked && g_evalTid.load()){ tidChecked=true;
    uint32_t p=(uint32_t)GetCurrentThreadId(), e=g_evalTid.load();
    Log("threads: present tid=%lu, DLSS-SR eval tid=%lu%s", (unsigned long)p, (unsigned long)e,
        p==e ? " (same thread)" : " (DIFFERENT threads - token hand-off is cross-thread)"); }
}
void PresentMarkersEnd(){
  auto& fns = GetSlFns();
  sl::FrameToken* next=nullptr; uint32_t nextIndex=0;
  { std::lock_guard<std::mutex> lk(g_qMutex);
    if(FrameSlot* f = QFront()){
      if(fns.pclSetMarker && f->tok) fns.pclSetMarker(sl::PCLMarker::ePresentEnd,*f->tok);   // AFTER the real present
      QPop(); }
    // Next frame: reuse the slot a pipelined eval already opened, else open it here.
    FrameSlot* n = QFront(); if(!n) n = QPushNew();
    if(n){ next = n->tok; nextIndex = n->index; } }
  if(!next) return;
  // Reflex sleep outside the lock (it can block ~1 frame) so an eval thread never waits on us.
  if(kReflexSleep && fns.reflexSleep){ double t0=NowMs(); fns.reflexSleep(*next); double d=NowMs()-t0; if(d>g_maxSleepMs) g_maxSleepMs=d; }
  { std::lock_guard<std::mutex> lk(g_qMutex); if(FrameSlot* s=QFind(nextIndex)) SimStart(*s); }
}
bool ConsumeEvalSeen(){ return g_evalSeen.exchange(false); }

}
