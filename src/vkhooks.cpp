#include "vkhooks.h"
#include "log.h"
#include "slboot.h"
#include "inputs.h"
#include <windows.h>
#include <detours.h>
#include <cstring>
#include <atomic>
#include <thread>

namespace fgvk {
VkInstance gInstance{};
VkPhysicalDevice gPhysicalDevice{};
VkDevice gDevice{};
VkQueue gGraphicsQueue{};
uint32_t gGraphicsFamily{};

// watchdog: counters + a position marker naming the in-flight call (inputs.cpp writes g_wdEvals/g_wdPos)
std::atomic<uint32_t> g_wdPresents{0};
std::atomic<uint32_t> g_wdEvals{0};
std::atomic<int> g_wdPos{0};   // 0 idle; 1 present-enter; 2 pre-proxy-present; 10/11/12 eval stages
static void StartWatchdog(){
  static std::atomic<bool> started{false};
  bool exp=false; if(!started.compare_exchange_strong(exp,true)) return;
  std::thread([]{
    uint32_t lastP=0;
    for(;;){ Sleep(2000);
      uint32_t p=g_wdPresents.load(), e=g_wdEvals.load(); int pos=g_wdPos.load();
      if(p!=lastP && pos==0){ lastP=p; continue; }
      Log("wd: presents=%u evals=%u pos=%d%s", p,e,pos,(p==lastP)?" STALLED":"");
      lastP=p; }
  }).detach();
}

// The game's own vkGetInstanceProcAddr (real loader), captured at hook install.
static PFN_vkGetInstanceProcAddr o_GIPA{};
// The interposer's GIPA/GDPA - SL owns every function these hand back.
static PFN_vkGetInstanceProcAddr ip_GIPA{};
static PFN_vkGetDeviceProcAddr   ip_GDPA{};
// Interposer targets we thin-wrap (resolved lazily via the interposer's GIPA/GDPA).
static PFN_vkCreateInstance     t_CreateInstance{};
static PFN_vkCreateDevice       t_CreateDevice{};
static PFN_vkQueuePresentKHR    t_QueuePresentKHR{};
static PFN_vkCreateSwapchainKHR t_CreateSwapchainKHR{};

// Re-entry guard: we Detoured vulkan-1's GIPA in place, so when the interposer forwards to
// the real loader (to reach the driver) it re-enters our hook. Without this, our hook routes
// that back into the interposer -> infinite recursion (the slInit hang). Calls made while
// g_reentry>0 originate INSIDE the interposer and must go straight to the real loader.
static thread_local int g_reentry = 0;
struct Reentry { Reentry(){ ++g_reentry; } ~Reentry(){ --g_reentry; } };

// ---- thin wrappers: call the INTERPOSER target, capture/act, return ----------------------
static VKAPI_ATTR VkResult VKAPI_CALL w_CreateInstance(
    const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out){
  VkResult r; { Reentry _; r = t_CreateInstance(ci, a, out); }
  if (r == VK_SUCCESS){ gInstance = *out; Log("w_CreateInstance ok instance=%p", (void*)gInstance); }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_CreateDevice(
    VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
    const VkAllocationCallbacks* a, VkDevice* out){
  // Interposer owns the device (its own surgery/queues). We only capture + kick off SL setup.
  VkResult r; { Reentry _; r = t_CreateDevice(pd, ci, a, out); }
  if (r == VK_SUCCESS){
    gPhysicalDevice = pd; gDevice = *out;
    for (uint32_t i=0;i<ci->queueCreateInfoCount;i++){ gGraphicsFamily = ci->pQueueCreateInfos[i].queueFamilyIndex; break; }
    Log("w_CreateDevice ok device=%p phys=%p gfxFamily=%u", (void*)gDevice,(void*)pd,gGraphicsFamily);
    { Reentry _; OnDeviceCreated(); }   // slboot: Reflex + slDLSSGSetOptions (SL calls -> guard)
  }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_CreateSwapchainKHR(
    VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
    const VkAllocationCallbacks* a, VkSwapchainKHR* out){
  VkResult r; { Reentry _; r = t_CreateSwapchainKHR(dev, ci, a, out); }
  static bool logged=false; if(!logged){ logged=true;
    Log("w_CreateSwapchainKHR %ux%u fmt=%d -> %d sc=%p", ci->imageExtent.width, ci->imageExtent.height,
        (int)ci->imageFormat, (int)r, out?(void*)*out:nullptr); }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* pi){
  StartWatchdog();
  g_wdPresents.fetch_add(1); g_wdPos.store(1);
  PollDLSSGState();
  NgxProbeTick();
  PresentMarkersBegin();
  static bool logged=false; if(!logged){ logged=true; Log("w_QueuePresentKHR live queue=%p", (void*)q); }
  g_wdPos.store(2);
  VkResult r; { Reentry _; r = t_QueuePresentKHR(q, pi); }   // interposer present = DLSS-G generation
  PresentMarkersEnd();
  g_wdPos.store(0);
  return r;
}

// ---- wrapped device-proc-addr: hand the game interposer device fns, wrap present/swapchain --
static PFN_vkGetDeviceProcAddr o_GDPA_real{};   // real loader GDPA, for re-entrant forwards
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL w_GetDeviceProcAddr(VkDevice dev, const char* name){
  if (!name) return nullptr;
  // Re-entrant (interposer forwarding to the driver): go to the real loader, not the interposer.
  if (g_reentry) return o_GDPA_real ? o_GDPA_real(dev, name) : nullptr;
  PFN_vkVoidFunction ip; { Reentry _; ip = ip_GDPA ? ip_GDPA(dev, name) : nullptr; }
  if (!ip) return nullptr;
  if (!strcmp(name, "vkQueuePresentKHR"))   { t_QueuePresentKHR   = (PFN_vkQueuePresentKHR)ip;   return (PFN_vkVoidFunction)w_QueuePresentKHR; }
  if (!strcmp(name, "vkCreateSwapchainKHR")){ t_CreateSwapchainKHR= (PFN_vkCreateSwapchainKHR)ip; return (PFN_vkVoidFunction)w_CreateSwapchainKHR; }
  return ip;   // everything else: the interposer's own device function
}

// ---- the single entry hook: vkGetInstanceProcAddr ---------------------------------------
// Init is DEFERRED to the vkCreateInstance resolution, NOT the first GIPA call: the game's
// first GIPA resolution happens in early loader-lock context, where slInit (which LoadLibrary's
// plugins) deadlocks (observed: log stops right after 'resolved slInit=...', game hangs).
// PureDark inits around instance creation for the same reason. We resolve the interposer's
// GIPA/GDPA FIRST so slInit's own re-entrant GIPA calls see the interposer, not the raw loader.
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL h_GetInstanceProcAddr(VkInstance inst, const char* name){
  if(!name) return nullptr;
  // Re-entrant (interposer forwarding to the driver): straight to the real loader.
  if(g_reentry) return o_GIPA ? o_GIPA(inst, name) : nullptr;

  if(!strcmp(name,"vkCreateInstance")){
    static bool inited=false;
    if(!inited){ inited=true;
      Reentry _;   // interposer load + slInit forward to the real loader, not back into us
      ip_GIPA = (PFN_vkGetInstanceProcAddr)SlProxyFn("vkGetInstanceProcAddr");  // loads interposer, set FIRST
      ip_GDPA = (PFN_vkGetDeviceProcAddr)SlProxyFn("vkGetDeviceProcAddr");
      EnsureStreamlineInit();   // slInit
      Log("SL init on vkCreateInstance resolve: ip_GIPA=%p ip_GDPA=%p", (void*)ip_GIPA,(void*)ip_GDPA);
    }
    PFN_vkVoidFunction ip; { Reentry _; ip = ip_GIPA ? ip_GIPA(inst,name) : (o_GIPA?o_GIPA(inst,name):nullptr); }
    if(ip){ t_CreateInstance=(PFN_vkCreateInstance)ip; return (PFN_vkVoidFunction)w_CreateInstance; }
    return nullptr;
  }

  // Everything else: interposer if ready, else the real loader (enumeration fns before
  // vkCreateInstance don't need the interposer).
  PFN_vkVoidFunction ip; { Reentry _; ip = ip_GIPA ? ip_GIPA(inst, name) : nullptr; }
  if(!ip) ip = o_GIPA ? o_GIPA(inst, name) : nullptr;
  if(!ip) return nullptr;
  if(!strcmp(name,"vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)h_GetInstanceProcAddr;
  if(!strcmp(name,"vkCreateDevice"))    { t_CreateDevice   = (PFN_vkCreateDevice)ip;   return (PFN_vkVoidFunction)w_CreateDevice; }
  if(!strcmp(name,"vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)w_GetDeviceProcAddr;
  return ip;
}

// ---- Vulkan function access for inputs.cpp (UI image, mem props) -------------------------
void* DeviceFn(const char* name){ return (ip_GDPA && gDevice) ? (void*)ip_GDPA(gDevice, name) : nullptr; }
void* LoaderFn(const char* name){
  // instance-scope functions (e.g. vkGetPhysicalDeviceMemoryProperties) via the interposer GIPA
  return (ip_GIPA && gInstance) ? (void*)ip_GIPA(gInstance, name) : nullptr;
}

void InstallVkHooks(){
  HMODULE vk = GetModuleHandleA("vulkan-1.dll");
  if(!vk) vk = LoadLibraryA("vulkan-1.dll");
  if(!vk){ Log("InstallVkHooks: vulkan-1.dll not found"); return; }
  o_GIPA = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
  o_GDPA_real = (PFN_vkGetDeviceProcAddr)GetProcAddress(vk, "vkGetDeviceProcAddr");
  if(!o_GIPA){ Log("InstallVkHooks: vkGetInstanceProcAddr export missing"); return; }
  DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
  DetourAttach(&(PVOID&)o_GIPA,(PVOID)h_GetInstanceProcAddr);
  LONG r = DetourTransactionCommit();
  Log("InstallVkHooks: GIPA hooked commit=%ld (o_GIPA=%p)", r,(void*)o_GIPA);
}

void RemoveVkHooks(){
  DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
  DetourDetach(&(PVOID&)o_GIPA,(PVOID)h_GetInstanceProcAddr);
  DetourTransactionCommit();
}
}
