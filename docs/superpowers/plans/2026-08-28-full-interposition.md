# fgvk Full-Interposition Rework — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (inline) or subagent-driven-development. Steps use checkbox (`- [ ]`) tracking. This is native Windows/Vulkan code: the implementer WRITES + self-checks; BUILD (cmake) and LAUNCH (BG3) are user-run. Every task's runtime gate is a user launch + log read.

**Goal:** Replace fgvk's hybrid Vulkan hook layer with a single `vkGetInstanceProcAddr` hook that hands the game Streamline's interposer functions, so the interposer uniformly owns instance→device→queues→swapchain→present (PureDark's confirmed architecture) — eliminating the `Invalid VK app queue` pacer rejection that blocked generated frames.

**Architecture:** fgvk's DllMain Detours `vulkan-1.dll`'s `vkGetInstanceProcAddr`. Our hook forwards every resolution to `sl.interposer.dll`'s `vkGetInstanceProcAddr` (so SL owns the dispatch), except for a few names we thin-wrap to capture handles (`vkCreateInstance`/`vkCreateDevice`) and emit per-present PCL markers + the NGX probe (`vkQueuePresentKHR` via a wrapped `vkGetDeviceProcAddr`). The interposer does its own device surgery and queue registration. All proven fgvk units — NGX DLSS-SR snoop, four required tags, recipe, PCL markers, watchdog, slboot's slInit/DLSSGSetOptions/Reflex/state-poll — carry forward unchanged.

**Tech Stack:** C++17, MSVC x64, CMake, Vulkan (VK_NO_PROTOTYPES), Microsoft Detours, Streamline SDK 2.12 headers, matched 2.12.0 SL/NGX runtime.

**Spec:** `docs/superpowers/specs/2026-08-28-full-interposition-design.md`

## Global Constraints
- Interposer owns creation: NO device surgery, NO slSetVulkanInfo, NO driver-pointer detours, NO re-entry guards/stack-walk. Our wrappers call the INTERPOSER's function (never the native driver).
- slInit MUST run before the game's first `vkCreateInstance` (call it in the GIPA hook's first invocation, before forwarding).
- Recipe carried forward: x4 (numFramesToGenerate=3), DepthInverted=eTrue, Reflex eLowLatencyWithBoost no cap, MvecScale −1, real jitter, all four tags. OTA off, matched 2.12.0 stack.
- Build: `cmd.exe /c "C:\Dev\fgvk\build.bat"` → `C:/Dev/fgvk/build/Release/fgvk.dll`. Deploy to `bin/NativeMods/fgvk.dll` (rename-around if the game holds it).
- Every run keeps SL verbose log + the watchdog. Phase-1 tests fgvk standalone (BG3SE Streamline off).

---

### Task 1: Rewrite the hook layer as GIPA interposition

**Files:**
- Rewrite: `src/vkhooks.cpp` (fully replaces the hybrid hooks)
- Modify: `src/vkhooks.h` (interface unchanged except NativeCreateSwapchain/Device removal is fine — keep DeviceFn/LoaderFn decls used by inputs.cpp)

**Interfaces:**
- Consumes: `fgvk::EnsureStreamlineInit()`, `fgvk::OnDeviceCreated()`, `fgvk::PollDLSSGState()`, `fgvk::SlProxyFn(const char*)` (slboot); `fgvk::NgxProbeTick()`, `fgvk::PresentMarkersBegin()`, `fgvk::PresentMarkersEnd()` (inputs).
- Produces: globals `gInstance/gPhysicalDevice/gDevice/gGraphicsFamily`; `void* DeviceFn(const char*)`, `void* LoaderFn(const char*)` for inputs.cpp; `InstallVkHooks()`/`RemoveVkHooks()`.

- [ ] **Step 1: Replace `src/vkhooks.cpp` entirely with the GIPA layer**

```cpp
#include "vkhooks.h"
#include "log.h"
#include "slboot.h"
#include "inputs.h"
#include <windows.h>
#include <detours.h>
#include <cstring>

namespace fgvk {
VkInstance gInstance{};
VkPhysicalDevice gPhysicalDevice{};
VkDevice gDevice{};
VkQueue gGraphicsQueue{};
uint32_t gGraphicsFamily{};

// The game's own vkGetInstanceProcAddr (real loader), captured at hook install.
static PFN_vkGetInstanceProcAddr o_GIPA{};
// The interposer's GIPA/GDPA — SL owns every function these hand back.
static PFN_vkGetInstanceProcAddr ip_GIPA{};
static PFN_vkGetDeviceProcAddr   ip_GDPA{};
// Interposer targets we thin-wrap (resolved lazily via the interposer's GIPA/GDPA).
static PFN_vkCreateInstance     t_CreateInstance{};
static PFN_vkCreateDevice       t_CreateDevice{};
static PFN_vkQueuePresentKHR    t_QueuePresentKHR{};
static PFN_vkCreateSwapchainKHR t_CreateSwapchainKHR{};

// ---- thin wrappers: call the INTERPOSER target, capture/act, return ----------------------
static VKAPI_ATTR VkResult VKAPI_CALL w_CreateInstance(
    const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out){
  VkResult r = t_CreateInstance(ci, a, out);
  if (r == VK_SUCCESS){ gInstance = *out; Log("w_CreateInstance ok instance=%p", (void*)gInstance); }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_CreateDevice(
    VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
    const VkAllocationCallbacks* a, VkDevice* out){
  // Interposer owns the device (its own surgery/queues). We only capture + kick off SL setup.
  VkResult r = t_CreateDevice(pd, ci, a, out);
  if (r == VK_SUCCESS){
    gPhysicalDevice = pd; gDevice = *out;
    for (uint32_t i=0;i<ci->queueCreateInfoCount;i++){ gGraphicsFamily = ci->pQueueCreateInfos[i].queueFamilyIndex; break; }
    Log("w_CreateDevice ok device=%p phys=%p gfxFamily=%u", (void*)gDevice,(void*)pd,gGraphicsFamily);
    OnDeviceCreated();   // slboot: Reflex + slDLSSGSetOptions + frame fns (NO surgery/slSetVulkanInfo)
  }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_CreateSwapchainKHR(
    VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
    const VkAllocationCallbacks* a, VkSwapchainKHR* out){
  VkResult r = t_CreateSwapchainKHR(dev, ci, a, out);
  static bool logged=false; if(!logged){ logged=true;
    Log("w_CreateSwapchainKHR %ux%u fmt=%d -> %d sc=%p", ci->imageExtent.width, ci->imageExtent.height,
        (int)ci->imageFormat, (int)r, out?(void*)*out:nullptr); }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* pi){
  PollDLSSGState();
  NgxProbeTick();
  PresentMarkersBegin();
  static bool logged=false; if(!logged){ logged=true; Log("w_QueuePresentKHR live queue=%p", (void*)q); }
  VkResult r = t_QueuePresentKHR(q, pi);   // interposer present = DLSS-G generation
  PresentMarkersEnd();
  return r;
}

// ---- wrapped device-proc-addr: hand the game interposer device fns, wrap present/swapchain --
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL w_GetDeviceProcAddr(VkDevice dev, const char* name){
  PFN_vkVoidFunction ip = ip_GDPA ? ip_GDPA(dev, name) : nullptr;
  if (!ip) return nullptr;
  if (!strcmp(name, "vkQueuePresentKHR"))   { t_QueuePresentKHR   = (PFN_vkQueuePresentKHR)ip;   return (PFN_vkVoidFunction)w_QueuePresentKHR; }
  if (!strcmp(name, "vkCreateSwapchainKHR")){ t_CreateSwapchainKHR= (PFN_vkCreateSwapchainKHR)ip; return (PFN_vkVoidFunction)w_CreateSwapchainKHR; }
  return ip;   // everything else: the interposer's own device function
}

// ---- the single entry hook: vkGetInstanceProcAddr ---------------------------------------
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL h_GetInstanceProcAddr(VkInstance inst, const char* name){
  // slInit BEFORE the game creates its instance, so the interposer is initialized when the
  // game resolves vkCreateInstance from it.
  static bool inited=false;
  if(!inited){ inited=true; EnsureStreamlineInit();
    if(!ip_GIPA){ ip_GIPA = (PFN_vkGetInstanceProcAddr)SlProxyFn("vkGetInstanceProcAddr"); }
    if(!ip_GDPA){ ip_GDPA = (PFN_vkGetDeviceProcAddr)SlProxyFn("vkGetDeviceProcAddr"); }
    Log("h_GetInstanceProcAddr first call: ip_GIPA=%p ip_GDPA=%p", (void*)ip_GIPA,(void*)ip_GDPA);
  }
  if(!name) return nullptr;
  // Forward to the interposer's GIPA (SL owns the dispatch); fall back to the real loader.
  PFN_vkVoidFunction ip = ip_GIPA ? ip_GIPA(inst, name) : nullptr;
  if(!ip) ip = o_GIPA ? o_GIPA(inst, name) : nullptr;
  if(!ip) return nullptr;
  if(!strcmp(name,"vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)h_GetInstanceProcAddr;
  if(!strcmp(name,"vkCreateInstance"))  { t_CreateInstance = (PFN_vkCreateInstance)ip; return (PFN_vkVoidFunction)w_CreateInstance; }
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
```

- [ ] **Step 2: Trim `src/vkhooks.h`** — keep exactly: the extern globals, `InstallVkHooks()`, `RemoveVkHooks()`, `DeviceFn`, `LoaderFn`. Remove `NativeCreateSwapchain`/`NativeCreateDevice`/`InstallDeviceHooks` declarations (deleted from the .cpp). Keep `#include <vulkan/vulkan.h>`.

- [ ] **Step 3: Build** — `cmd.exe /c "C:\Dev\fgvk\build.bat"`. Expected: compiles, links (only detours.lib). Self-check: no reference to removed symbols (CreateDeviceWithSL, hd_*, SwapGuard, g_inSwapFamily, o_GetDeviceProcAddr).

- [ ] **Step 4: Commit** — `feat: GIPA interposition hook layer (single vkGetInstanceProcAddr entry)`.

### Task 2: Strip slboot's surgery + slSetVulkanInfo; interposer-owned bring-up

**Files:**
- Modify: `src/slboot.cpp` (delete `CreateDeviceWithSL` + the SL-requirements surgery + the `slSetVulkanInfo` block in `OnDeviceCreated`), `src/slboot.h` (remove `CreateDeviceWithSL`, `SlProxyCreateDevice`, `SlProxyCreateSwapchain`, `SlProxyPresent`, `NativeCreateDevice` decls if present).

**Interfaces:**
- Consumes: `gDevice/gInstance/gPhysicalDevice` (vkhooks), `p_slReflexSetOptions/p_slDLSSGSetOptions` (already resolved), `EnsureFeatureFunctions()`.
- Produces: `OnDeviceCreated()` now = EnsureSlInit (already run) → EnsureFeatureFunctions → Reflex(eLowLatencyWithBoost) → DLSSGSetOptions(eOn, numFramesToGenerate=3) → resolve frame fns. `EnsureStreamlineInit()`, `SlProxyFn()`, `PollDLSSGState()`, `GetSlFns()` UNCHANGED.

- [ ] **Step 1: Delete `CreateDeviceWithSL` and its helpers** (the `FetchSLRequirements`, `SLReqs`, `SLSlots g_slots`, `addUniq`, the whole surgery function). Delete `SlProxyCreateDevice`/`SlProxyCreateSwapchain`/`SlProxyPresent` if present (the generic `SlProxyFn` replaces them). Keep `EnsureSlInit`, `EnsureFeatureFunctions`, `SlProxyFn`, `GetSlFns`, `PollDLSSGState`.

- [ ] **Step 2: Rewrite `OnDeviceCreated()` body** to interposer-owned bring-up:

```cpp
void OnDeviceCreated(){
  if(!EnsureStreamlineInit()) { Log("OnDeviceCreated: slInit not ready"); return; }
  if(!EnsureFeatureFunctions()) { Log("OnDeviceCreated: feature fns failed"); return; }
  // Interposer owns the device + queues; no slSetVulkanInfo, no surgery.
  sl::ReflexOptions ro{}; ro.mode = sl::ReflexMode::eLowLatencyWithBoost;
  Log("slReflexSetOptions -> %d", (int)p_slReflexSetOptions(ro));
  sl::DLSSGOptions o{}; o.mode = sl::DLSSGMode::eOn; o.numFramesToGenerate = 3;   // x4
  sl::ViewportHandle vp{0};
  Log("slDLSSGSetOptions(eOn,3) -> %d", (int)p_slDLSSGSetOptions(vp, o));
}
```
(Confirm `p_slReflexSetOptions`/`p_slDLSSGSetOptions` are the file's actual resolved-pointer names; adjust if different. `PollDLSSGState` keeps polling with `numFramesToGenerate=3`.)

- [ ] **Step 3: Fix `PollDLSSGState`'s options** to `numFramesToGenerate=3` to match (if it constructs DLSSGOptions).

- [ ] **Step 4: Build.** Expected: compiles; `g_slots`/surgery symbols gone. Commit — `refactor: interposer owns device - drop surgery + slSetVulkanInfo`.

### Task 3: Hook NGX CreateFeature (mirror PureDark)

**Files:**
- Modify: `src/inputs.cpp`, `src/inputs.h`

**Interfaces:**
- Produces: within `NgxProbeTick()`, also detour `NVSDK_NGX_VULKAN_CreateFeature`; a `h_NgxCreateFeature` that logs the feature id and passes through. Diagnostic parity with his `hk_NVSDK_NGX_VULKAN_CreateFeature FeatureID 1`.

- [ ] **Step 1: Add the CreateFeature typedef + detour in inputs.cpp** (alongside the existing EvaluateFeature hook), resolved from the same NGX module:

```cpp
typedef NVSDK_NGX_Result (__cdecl *PFN_NgxCreateFeature)(VkCommandBuffer, unsigned int /*featureId*/,
    NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
static PFN_NgxCreateFeature o_NgxCreateFeature{};
static NVSDK_NGX_Result __cdecl h_NgxCreateFeature(VkCommandBuffer cmd, unsigned int featureId,
    NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** outH){
  static bool logged=false; if(!logged){ logged=true; Log("NGX CreateFeature featureId=%u", featureId); }
  return o_NgxCreateFeature(cmd, featureId, p, outH);
}
```
In `NgxProbeTick()` where the module is found, also `GetProcAddress(found,"NVSDK_NGX_VULKAN_CreateFeature")` and DetourAttach it in the same transaction as EvaluateFeature. Guard against re-install with the existing `g_hooked` flag.

- [ ] **Step 2: Build + commit** — `feat: hook NGX CreateFeature for parity/diagnostics`.

### Task 4: Bring-up gate (build → deploy → launch → verify) [USER LAUNCH]

**Files:** none (integration).

- [ ] **Step 1: Build** the full DLL; confirm fresh `fgvk.dll`.
- [ ] **Step 2: Deploy** to `bin/NativeMods/fgvk.dll` (rename-around if locked). Ensure matched 2.12.0 SL stack + NvLowLatencyVk in bin; his UpscalerBasePlugin disabled; BG3SE Streamline off; clear `fgvk.log` + `sl.log`.
- [ ] **Step 3: USER LAUNCH** BG3, load into the 3D world ~20s.
- [ ] **Step 4: Verify the gate.** `fgvk.log`: `h_GetInstanceProcAddr first call` with non-null ip_GIPA/ip_GDPA; `w_CreateInstance`/`w_CreateDevice` ok; `slDLSSGSetOptions(eOn,3) -> 0`; `w_QueuePresentKHR live`; `DLSSG status=0`; NGX inputs + all four tags. `sl.log`: **NO `Invalid VK app queue`**, **NO `getHostQueueInfo` failure**; pacer meters instead of timing out. **Success = DLSS-G watermark visible + displayed fps ≈ 4× real.**
- [ ] **Step 5:** Record the result in `docs/M2-result.md` (paste the key log lines + verdict). Commit.

## Self-Review
- **Spec coverage:** GIPA single-hook (Task 1) ✓; strip surgery/slSetVulkanInfo/driver detours (Tasks 1+2) ✓; keep snoop+tags+recipe+slboot (untouched + Task 2 bring-up) ✓; NGX CreateFeature (Task 3) ✓; bring-up to frames (Task 4) ✓; interposer owns queues → no Invalid-queue (Task 1 architecture, verified Task 4) ✓.
- **Placeholder scan:** none — Task 1 is complete code; Tasks 2–4 name exact edits/values.
- **Type consistency:** `t_CreateInstance/t_CreateDevice/t_QueuePresentKHR/t_CreateSwapchainKHR` set in the GIPA/GDPA wrappers and consumed by the `w_*` wrappers; `ip_GIPA/ip_GDPA` resolved via `SlProxyFn`; `OnDeviceCreated` matches vkhooks' call; `DeviceFn/LoaderFn` signatures match inputs.h. `numFramesToGenerate=3` consistent between OnDeviceCreated and PollDLSSGState.
- **Open risk (verified in Task 4, not before):** if the game resolves core Vulkan via the loader's IAT rather than calling `vkGetInstanceProcAddr`, the GIPA hook may not catch `vkCreateInstance`. Mitigation if the log shows no `w_CreateInstance`: also Detour `vulkan-1.dll`'s `vkCreateInstance` export to seed `t_CreateInstance`+redirect (one-line fallback, added only if observed).
