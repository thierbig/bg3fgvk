#include "vkhooks.h"
#include "log.h"
#include "slboot.h"
#include "inputs.h"
#include <windows.h>
#include <detours.h>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <thread>

namespace fgvk {
VkInstance gInstance{};
VkPhysicalDevice gPhysicalDevice{};
VkDevice gDevice{};
VkQueue gGraphicsQueue{};
uint32_t gGraphicsFamily{};

void InstallDeviceHooks(VkDevice dev);  // defined below; called from h_CreateDevice

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
    InstallDeviceHooks(gDevice);  // detour the DRIVER's swapchain-family pointers
  }
  return r;
}

// ---- device-level (driver-pointer) swapchain-family hooks -------------------------------
// BG3 resolves these via vkGetDeviceProcAddr (driver pointers), bypassing the vulkan-1.dll
// exports. We detour the DRIVER pointers after device creation. Once the game holds a PROXY
// swapchain handle from the interposer, the whole family (destroy/getImages/acquire/present)
// must also route to the interposer; a shared guard sends the interposer's own inner calls
// straight to the driver original.
static thread_local bool g_inSwapFamily = false;
static PFN_vkGetDeviceProcAddr o_GetDeviceProcAddr{};
static PFN_vkCreateSwapchainKHR d_CreateSwapchainKHR{};
static PFN_vkDestroySwapchainKHR d_DestroySwapchainKHR{};
static PFN_vkGetSwapchainImagesKHR d_GetSwapchainImagesKHR{};
static PFN_vkAcquireNextImageKHR d_AcquireNextImageKHR{};
static PFN_vkAcquireNextImage2KHR d_AcquireNextImage2KHR{};
static PFN_vkQueuePresentKHR d_QueuePresentKHR{};
static PFN_vkQueueSubmit d_QueueSubmit{};
static PFN_vkQueueSubmit2 d_QueueSubmit2{};

// watchdog state: counters + a position marker naming the call in flight
std::atomic<uint32_t> g_wdPresents{0}, g_wdSubmits{0}, g_wdEvals{0};
std::atomic<int> g_wdPos{0};   // 0 idle; 1 present-enter; 2 pre-proxy-present; 3 present-done; 10 eval-enter; 11 eval-orig; 12 eval-done
static void StartWatchdog(){
  static std::atomic<bool> started{false};
  bool exp=false;
  if(!started.compare_exchange_strong(exp,true)) return;
  std::thread([]{
    uint32_t lastP=0;
    for(;;){
      Sleep(2000);
      uint32_t p=g_wdPresents.load(), s=g_wdSubmits.load(), e=g_wdEvals.load();
      int pos=g_wdPos.load();
      if(p!=lastP && pos==0) { lastP=p; continue; }   // healthy and idle: stay quiet
      Log("wd: presents=%u submits=%u evals=%u pos=%d%s", p,s,e,pos,
          (p==lastP)?" STALLED":"");
      lastP=p;
    }
  }).detach();
}

struct SwapGuard {
  SwapGuard(){ g_inSwapFamily = true; }
  ~SwapGuard(){ g_inSwapFamily = false; }
};

// Streamline's OWN threads (DLSS-G pacer/generator) call the driver functions we detoured
// in place; the thread_local guard cannot cover them. Stack-walk: a call whose chain contains
// an sl.* module (or nvngx_dlssg) is SL-internal - pass it straight to the driver original.
// NvLowLatencyVk is deliberately NOT matched (the game's own Reflex library sits in normal
// game call chains). Ported from BG3SE (verdict cache per module).
static bool CallChainContainsStreamline(){
  void* frames[7]{};
  USHORT n = CaptureStackBackTrace(1, 7, frames, nullptr);
  static std::mutex lock;
  static std::unordered_map<HMODULE,bool> verdicts;
  for (USHORT i = 0; i < n; i++){
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
          | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(frames[i]), &mod) || mod == nullptr) continue;
    {
      std::lock_guard<std::mutex> _(lock);
      auto it = verdicts.find(mod);
      if (it != verdicts.end()) { if (it->second) return true; continue; }
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
    bool isSL = (_wcsnicmp(base, L"sl.", 3) == 0) || (_wcsnicmp(base, L"nvngx_dlssg", 11) == 0);
    {
      std::lock_guard<std::mutex> _(lock);
      verdicts[mod] = isSL;
    }
    if (isSL) return true;
  }
  return false;
}

static VKAPI_ATTR VkResult VKAPI_CALL hd_CreateSwapchainKHR(
    VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
    const VkAllocationCallbacks* a, VkSwapchainKHR* out){
  if (g_inSwapFamily || CallChainContainsStreamline()) return d_CreateSwapchainKHR(dev, ci, a, out);
  SwapGuard g;
  auto proxy = (PFN_vkCreateSwapchainKHR)SlProxyFn("vkCreateSwapchainKHR");
  Log("vkCreateSwapchainKHR(dev) %ux%u fmt=%d proxy=%p", ci->imageExtent.width,
      ci->imageExtent.height, (int)ci->imageFormat, (void*)proxy);
  VkResult r = proxy ? proxy(dev, ci, a, out) : d_CreateSwapchainKHR(dev, ci, a, out);
  Log("vkCreateSwapchainKHR(dev) -> %d swapchain=%p", (int)r, out?(void*)*out:nullptr);
  return r;
}

static VKAPI_ATTR void VKAPI_CALL hd_DestroySwapchainKHR(
    VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks* a){
  if (g_inSwapFamily || CallChainContainsStreamline()) { d_DestroySwapchainKHR(dev, sc, a); return; }
  SwapGuard g;
  auto proxy = (PFN_vkDestroySwapchainKHR)SlProxyFn("vkDestroySwapchainKHR");
  if (proxy) proxy(dev, sc, a); else d_DestroySwapchainKHR(dev, sc, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL hd_GetSwapchainImagesKHR(
    VkDevice dev, VkSwapchainKHR sc, uint32_t* count, VkImage* images){
  if (g_inSwapFamily || CallChainContainsStreamline()) return d_GetSwapchainImagesKHR(dev, sc, count, images);
  SwapGuard g;
  auto proxy = (PFN_vkGetSwapchainImagesKHR)SlProxyFn("vkGetSwapchainImagesKHR");
  return proxy ? proxy(dev, sc, count, images) : d_GetSwapchainImagesKHR(dev, sc, count, images);
}

static VKAPI_ATTR VkResult VKAPI_CALL hd_AcquireNextImageKHR(
    VkDevice dev, VkSwapchainKHR sc, uint64_t timeout,
    VkSemaphore sem, VkFence fence, uint32_t* idx){
  if (g_inSwapFamily || CallChainContainsStreamline()) return d_AcquireNextImageKHR(dev, sc, timeout, sem, fence, idx);
  SwapGuard g;
  auto proxy = (PFN_vkAcquireNextImageKHR)SlProxyFn("vkAcquireNextImageKHR");
  return proxy ? proxy(dev, sc, timeout, sem, fence, idx)
               : d_AcquireNextImageKHR(dev, sc, timeout, sem, fence, idx);
}

static VKAPI_ATTR VkResult VKAPI_CALL hd_AcquireNextImage2KHR(
    VkDevice dev, const VkAcquireNextImageInfoKHR* info, uint32_t* idx){
  if (g_inSwapFamily || CallChainContainsStreamline()) return d_AcquireNextImage2KHR(dev, info, idx);
  SwapGuard g;
  auto proxy2 = (PFN_vkAcquireNextImage2KHR)SlProxyFn("vkAcquireNextImage2KHR");
  if (proxy2) return proxy2(dev, info, idx);
  // interposer without the 2-variant: translate to the 1-variant proxy so the proxy
  // swapchain handle still reaches the interposer, never the raw driver.
  auto proxy1 = (PFN_vkAcquireNextImageKHR)SlProxyFn("vkAcquireNextImageKHR");
  if (proxy1) return proxy1(dev, info->swapchain, info->timeout, info->semaphore, info->fence, idx);
  return d_AcquireNextImage2KHR(dev, info, idx);
}

static VKAPI_ATTR VkResult VKAPI_CALL hd_QueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
  if (g_inSwapFamily || CallChainContainsStreamline()) return d_QueuePresentKHR(queue, pPresentInfo);
  SwapGuard g;
  StartWatchdog();
  g_wdPresents.fetch_add(1); g_wdPos.store(1);
  PollDLSSGState();
  NgxProbeTick();           // install the DLSS-SR snoop once NGX modules are loaded
  PresentMarkersBegin();    // reflex sleep + all six PCL markers, one token
  static bool logged = false;
  auto proxy = (PFN_vkQueuePresentKHR)SlProxyFn("vkQueuePresentKHR");
  if (!logged) { logged = true; Log("first present: proxy=%p", (void*)proxy); }
  g_wdPos.store(2);
  VkResult pr = proxy ? proxy(queue, pPresentInfo) : d_QueuePresentKHR(queue, pPresentInfo);
  g_wdPos.store(0);
  return pr;
}

static VKAPI_ATTR VkResult VKAPI_CALL hd_QueueSubmit(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence){
  if (g_inSwapFamily || CallChainContainsStreamline()) return d_QueueSubmit(queue, submitCount, pSubmits, fence);
  SwapGuard g;
  g_wdSubmits.fetch_add(1);
  auto proxy = (PFN_vkQueueSubmit)SlProxyFn("vkQueueSubmit");
  return proxy ? proxy(queue, submitCount, pSubmits, fence) : d_QueueSubmit(queue, submitCount, pSubmits, fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL hd_QueueSubmit2(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence){
  if (g_inSwapFamily || CallChainContainsStreamline()) return d_QueueSubmit2(queue, submitCount, pSubmits, fence);
  SwapGuard g;
  g_wdSubmits.fetch_add(1);
  auto proxy = (PFN_vkQueueSubmit2)SlProxyFn("vkQueueSubmit2");
  return proxy ? proxy(queue, submitCount, pSubmits, fence) : d_QueueSubmit2(queue, submitCount, pSubmits, fence);
}

void InstallDeviceHooks(VkDevice dev){
  static bool installed = false;
  if (installed) return;
  if (!o_GetDeviceProcAddr) { Log("InstallDeviceHooks: no vkGetDeviceProcAddr"); return; }
  d_CreateSwapchainKHR   = (PFN_vkCreateSwapchainKHR)  o_GetDeviceProcAddr(dev, "vkCreateSwapchainKHR");
  d_DestroySwapchainKHR  = (PFN_vkDestroySwapchainKHR) o_GetDeviceProcAddr(dev, "vkDestroySwapchainKHR");
  d_GetSwapchainImagesKHR= (PFN_vkGetSwapchainImagesKHR)o_GetDeviceProcAddr(dev, "vkGetSwapchainImagesKHR");
  d_AcquireNextImageKHR  = (PFN_vkAcquireNextImageKHR) o_GetDeviceProcAddr(dev, "vkAcquireNextImageKHR");
  d_AcquireNextImage2KHR = (PFN_vkAcquireNextImage2KHR)o_GetDeviceProcAddr(dev, "vkAcquireNextImage2KHR");
  d_QueuePresentKHR      = (PFN_vkQueuePresentKHR)     o_GetDeviceProcAddr(dev, "vkQueuePresentKHR");
  d_QueueSubmit          = (PFN_vkQueueSubmit)         o_GetDeviceProcAddr(dev, "vkQueueSubmit");
  d_QueueSubmit2         = (PFN_vkQueueSubmit2)        o_GetDeviceProcAddr(dev, "vkQueueSubmit2");
  Log("driver ptrs: createSC=%p destroySC=%p getImgs=%p acq=%p acq2=%p present=%p",
      (void*)d_CreateSwapchainKHR,(void*)d_DestroySwapchainKHR,(void*)d_GetSwapchainImagesKHR,
      (void*)d_AcquireNextImageKHR,(void*)d_AcquireNextImage2KHR,(void*)d_QueuePresentKHR);
  DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
  if (d_CreateSwapchainKHR)   DetourAttach(&(PVOID&)d_CreateSwapchainKHR,(PVOID)hd_CreateSwapchainKHR);
  if (d_DestroySwapchainKHR)  DetourAttach(&(PVOID&)d_DestroySwapchainKHR,(PVOID)hd_DestroySwapchainKHR);
  if (d_GetSwapchainImagesKHR)DetourAttach(&(PVOID&)d_GetSwapchainImagesKHR,(PVOID)hd_GetSwapchainImagesKHR);
  if (d_AcquireNextImageKHR)  DetourAttach(&(PVOID&)d_AcquireNextImageKHR,(PVOID)hd_AcquireNextImageKHR);
  if (d_AcquireNextImage2KHR) DetourAttach(&(PVOID&)d_AcquireNextImage2KHR,(PVOID)hd_AcquireNextImage2KHR);
  if (d_QueuePresentKHR)      DetourAttach(&(PVOID&)d_QueuePresentKHR,(PVOID)hd_QueuePresentKHR);
  if (d_QueueSubmit)          DetourAttach(&(PVOID&)d_QueueSubmit,(PVOID)hd_QueueSubmit);
  if (d_QueueSubmit2)         DetourAttach(&(PVOID&)d_QueueSubmit2,(PVOID)hd_QueueSubmit2);
  LONG r = DetourTransactionCommit();
  Log("InstallDeviceHooks: commit -> %ld", r);
  installed = (r == NO_ERROR);
}

// legacy loader-export hooks (kept: some callers may still use the exports; same guard+routing)
static VKAPI_ATTR VkResult VKAPI_CALL h_CreateSwapchainKHR(
    VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
    const VkAllocationCallbacks* a, VkSwapchainKHR* out){
  if (g_inSwapFamily) return o_CreateSwapchainKHR(dev, ci, a, out);
  SwapGuard g;
  auto proxy = SlProxyCreateSwapchain();
  Log("vkCreateSwapchainKHR(loader) %ux%u fmt=%d proxy=%p", ci->imageExtent.width,
      ci->imageExtent.height, (int)ci->imageFormat, (void*)proxy);
  if (proxy) return proxy(dev, ci, a, out);
  return o_CreateSwapchainKHR(dev, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL h_QueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
  if (g_inSwapFamily) return o_QueuePresentKHR(queue, pPresentInfo);
  SwapGuard g;
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
  o_GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)GetProcAddress(vk,"vkGetDeviceProcAddr");
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

void* DeviceFn(const char* name){
  return (o_GetDeviceProcAddr && gDevice) ? (void*)o_GetDeviceProcAddr(gDevice, name) : nullptr;
}
void* LoaderFn(const char* name){
  HMODULE vk = GetModuleHandleA("vulkan-1.dll");
  return vk ? (void*)GetProcAddress(vk, name) : nullptr;
}

PFN_vkCreateSwapchainKHR NativeCreateSwapchain(){ return o_CreateSwapchainKHR; }
PFN_vkCreateDevice NativeCreateDevice(){ return o_CreateDevice; }
}
