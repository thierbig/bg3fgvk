# fgvk Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A standalone Vulkan DLL that routes BG3's swapchain through Streamline's DLSS-G proxy so the driver performs frame generation + pacing — a free replacement for PureDark's upscaler.dll that keeps the BG3SE MCM working.

**Architecture:** Detours-hooked Vulkan entry points put the Streamline interposer's proxy swapchain in the present path; Streamline (DLSS-G + Reflex + PCL) is initialized against the game's device; DLSS-G inputs are snooped from the game's DLSS-SR evaluate and tagged; the driver generates and paces. Our code is hooks + tagging + config only.

**Tech Stack:** C++17, CMake, MSVC x64, Vulkan (`External/VulkanSDK`), Streamline SDK 2.12 (`External/streamline`), Microsoft Detours (`External/Detours`). Reuse the BG3SE repo's copies of these to avoid re-vendoring.

**Spec:** `/mnt/c/Dev/fgvk/docs/2026-08-27-fgvk-design.md`

## Global Constraints

- Target: BG3, Vulkan, NVIDIA RTX, Windows, MSVC x64. Derive display refresh at runtime.
- DLSS-G recipe (captured): `numFramesToGenerate = N-1` (x2→1, x3→2, x4→3); `DepthInverted=1`; `ColorBuffersHDR=0`; `Enable.OFA=1`; `MvecScaleX=MvecScaleY=-1.0`; real per-subframe jitter; MVecs/Depth at render res (~1707×960), Backbuffer display res fmt 44; matrices ViewToClip/ClipToView/ClipToPrevClip/PrevClipToClip; his set has no HUD-less (backbuffer-only).
- Reflex: Low Latency **with Boost**, no fps cap.
- Streamline stack shipped alongside: `sl.interposer/common/dlss_g/pcl/reflex`, `nvngx_dlssg` — copy from `C:\Games\Baldurs Gate 3\bin\mods\UpscalerBasePlugin\Streamline\`.
- MCM coexistence: present the same proxy-swapchain interface PureDark's upscaler does; do NOT build our own ImGui.
- **BG3SE compatibility (first-class, every milestone):** chain shared Vulkan/NGX hooks (never clobber BG3SE's), tolerate either install order, load through BG3SE's native loader without displacing it, validate every milestone with BG3SE present.
- Reference API (verified in `External/streamline/include`): `slInit(const Preferences&, uint64_t sdkVersion)`; `slSetVulkanInfo(const VulkanInfo&)`; `slDLSSGSetOptions(const ViewportHandle&, const DLSSGOptions&)` where `DLSSGOptions{ mode = DLSSGMode::eOn, numFramesToGenerate }`; `slDLSSGGetState(const ViewportHandle&, DLSSGState&, const DLSSGOptions*)`; `slSetTagForFrame(...)`.

---

## Milestone 1 — Arming spike (THROWAWAY; the whole bet)

**Purpose:** prove that keeping the Streamline proxy swapchain in the present path makes DLSS-G *arm* (the state transition the old direct-NGX branch never achieved). Success is a log line, not image quality. If it does not arm, STOP and debug arming before M2.

**M1 files:**
- Create: `CMakeLists.txt` — build the DLL against Vulkan/SL/Detours.
- Create: `src/dllmain.cpp` — DllMain, install/remove hooks, wire logging.
- Create: `src/log.h` / `src/log.cpp` — file logger to `fgvk.log` next to the game exe.
- Create: `src/vkhooks.cpp` / `src/vkhooks.h` — Detours on `vkGetDeviceProcAddr`, `vkCreateDevice`, `vkCreateSwapchainKHR`; SL proxy routing.
- Create: `src/slboot.cpp` / `src/slboot.h` — `slInit`, `slSetVulkanInfo`, DLSS-G enable + state poll.

### Task 1: Project scaffold + file logger + DLL entry

**Files:**
- Create: `CMakeLists.txt`
- Create: `src/log.h`, `src/log.cpp`
- Create: `src/dllmain.cpp`

**Interfaces:**
- Produces: `fgvk::Log(const char* fmt, ...)` — appends a line to `<gamedir>/fgvk.log`; thread-safe (mutex). `fgvk::LogInit()` — resolves the log path (module dir of the host process).

- [ ] **Step 1: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20)
project(fgvk LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BG3SE "C:/Dev/bg3se-upscaler/External")
add_library(fgvk SHARED
  src/dllmain.cpp src/log.cpp src/vkhooks.cpp src/slboot.cpp)
target_include_directories(fgvk PRIVATE
  "${BG3SE}/VulkanSDK/Include" "${BG3SE}/streamline/include" "${BG3SE}/Detours/include" src)
target_link_libraries(fgvk PRIVATE "${BG3SE}/Detours/lib.X64/detours.lib")
target_compile_definitions(fgvk PRIVATE VK_NO_PROTOTYPES WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(fgvk PROPERTIES OUTPUT_NAME "fgvk")
```

- [ ] **Step 2: Write `src/log.h`**

```cpp
#pragma once
namespace fgvk { void LogInit(); void Log(const char* fmt, ...); }
```

- [ ] **Step 3: Write `src/log.cpp`**

```cpp
#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
namespace { std::mutex g_m; char g_path[MAX_PATH]{}; }
namespace fgvk {
void LogInit() {
  char exe[MAX_PATH]{}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
  char* slash = strrchr(exe, '\\'); if (slash) *(slash+1) = 0;
  snprintf(g_path, sizeof(g_path), "%sfgvk.log", exe);
  FILE* f=nullptr; if (fopen_s(&f,g_path,"w")==0 && f){ fputs("fgvk log start\n",f); fclose(f);} }
void Log(const char* fmt, ...) {
  std::lock_guard<std::mutex> lk(g_m);
  FILE* f=nullptr; if (fopen_s(&f,g_path,"a")!=0 || !f) return;
  va_list ap; va_start(ap,fmt); vfprintf(f,fmt,ap); va_end(ap); fputc('\n',f); fclose(f); }
}
```

- [ ] **Step 4: Write `src/dllmain.cpp` (stub hooks install; real bodies in later tasks)**

```cpp
#include <windows.h>
#include "log.h"
#include "vkhooks.h"
BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(h);
    fgvk::LogInit();
    fgvk::Log("DllMain attach - installing hooks");
    fgvk::InstallVkHooks();
  } else if (reason == DLL_PROCESS_DETACH) {
    fgvk::RemoveVkHooks();
  }
  return TRUE;
}
```

- [ ] **Step 5: Create stub `src/vkhooks.h` and empty-body `.cpp`/`slboot` so it links**

```cpp
// src/vkhooks.h
#pragma once
namespace fgvk { void InstallVkHooks(); void RemoveVkHooks(); }
```
Create `src/vkhooks.cpp`, `src/slboot.cpp`, `src/slboot.h` with empty `InstallVkHooks(){fgvk::Log("InstallVkHooks stub");}` / `RemoveVkHooks(){}` so Task 1 builds standalone.

- [ ] **Step 6: Build**

Run (from a VS x64 dev shell or via cmake):
```
cmake -S C:/Dev/fgvk -B C:/Dev/fgvk/build -A x64
cmake --build C:/Dev/fgvk/build --config Release
```
Expected: `fgvk.dll` in `C:/Dev/fgvk/build/Release`.

- [ ] **Step 7: Load-and-log test (user launches)**

Copy `fgvk.dll` next to `bg3.exe` and load it (temporarily via BG3SE's `LoadLibraryW` slot, or a manual injector). Launch BG3.
Expected: `<gamedir>/fgvk.log` contains `DllMain attach - installing hooks` and `InstallVkHooks stub`.

- [ ] **Step 8: Commit**

```
git add CMakeLists.txt src/
git commit -m "feat: fgvk scaffold - DLL entry, file logger, hook stubs"
```

### Task 2: Detour vkGetDeviceProcAddr + vkCreateDevice; capture device/queues

**Files:**
- Modify: `src/vkhooks.cpp`, `src/vkhooks.h`

**Interfaces:**
- Produces: `fgvk::gDevice` (VkDevice), `fgvk::gInstance`, `fgvk::gPhysicalDevice`, `fgvk::gGraphicsQueue` + family/index, captured at device creation. `fgvk::OnDeviceCreated()` hook point consumed by Task 4.

- [ ] **Step 1: Add Detours transaction helpers + real-function pointers in `vkhooks.cpp`**

Resolve the native `vkCreateDevice`, `vkCreateSwapchainKHR`, `vkGetDeviceProcAddr` from `vulkan-1.dll` via `GetProcAddress`, store originals, `DetourAttach` in `InstallVkHooks()`, `DetourDetach` in `RemoveVkHooks()`. Full code:

```cpp
#include "vkhooks.h"
#include "log.h"
#include "slboot.h"
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <detours.h>

namespace fgvk {
VkInstance gInstance{}; VkPhysicalDevice gPhysicalDevice{}; VkDevice gDevice{};
VkQueue gGraphicsQueue{}; uint32_t gGraphicsFamily{}; 

static PFN_vkCreateDevice o_CreateDevice{};
static PFN_vkCreateSwapchainKHR o_CreateSwapchainKHR{};

static VKAPI_ATTR VkResult VKAPI_CALL h_CreateDevice(
    VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
    const VkAllocationCallbacks* a, VkDevice* out) {
  VkResult r = o_CreateDevice(pd, ci, a, out);
  if (r == VK_SUCCESS) {
    gPhysicalDevice = pd; gDevice = *out;
    // first graphics queue from ci
    for (uint32_t i=0;i<ci->queueCreateInfoCount;i++){
      gGraphicsFamily = ci->pQueueCreateInfos[i].queueFamilyIndex; break; }
    Log("vkCreateDevice ok device=%p phys=%p gfxFamily=%u", (void*)gDevice,(void*)pd,gGraphicsFamily);
    OnDeviceCreated();   // slboot: slSetVulkanInfo + DLSS-G enable
  }
  return r;
}

void InstallVkHooks(){
  HMODULE vk = GetModuleHandleA("vulkan-1.dll");
  if(!vk) vk = LoadLibraryA("vulkan-1.dll");
  o_CreateDevice = (PFN_vkCreateDevice)GetProcAddress(vk,"vkCreateDevice");
  o_CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)GetProcAddress(vk,"vkCreateSwapchainKHR");
  DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
  DetourAttach(&(PVOID&)o_CreateDevice,(PVOID)h_CreateDevice);
  DetourTransactionCommit();
  Log("InstallVkHooks: CreateDevice hooked (o=%p)", (void*)o_CreateDevice);
}
void RemoveVkHooks(){
  DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
  DetourDetach(&(PVOID&)o_CreateDevice,(PVOID)h_CreateDevice);
  DetourTransactionCommit();
}
PFN_vkCreateSwapchainKHR NativeCreateSwapchain(){ return o_CreateSwapchainKHR; }
}
```

- [ ] **Step 2: Declare the externs in `vkhooks.h`** (gInstance/gPhysicalDevice/gDevice/gGraphicsQueue/gGraphicsFamily, `NativeCreateSwapchain()`).

- [ ] **Step 3: Build (same cmake as Task 1 Step 6).** Expected: links.

- [ ] **Step 4: In-game test (user launches).** Expected: `fgvk.log` shows `vkCreateDevice ok device=...`.

- [ ] **Step 5: Commit** `feat: hook vkCreateDevice, capture device/queues`.

### Task 3: Route swapchain creation through the Streamline interposer proxy

**Files:**
- Modify: `src/vkhooks.cpp`, `src/slboot.cpp`

**Interfaces:**
- Consumes: `NativeCreateSwapchain()`, `fgvk::gDevice`.
- Produces: the game's swapchain is created by `sl.interposer.dll`'s `vkCreateSwapchainKHR` (proxy), not the native driver — the core of the whole pivot.

- [ ] **Step 1: Resolve the interposer's vkCreateSwapchainKHR in `slboot.cpp`**

```cpp
// slboot.cpp
#include "slboot.h"
#include "log.h"
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
namespace fgvk {
static HMODULE g_sl{};
PFN_vkCreateSwapchainKHR SlProxyCreateSwapchain(){
  if(!g_sl){ g_sl = GetModuleHandleA("sl.interposer.dll");
    if(!g_sl) g_sl = LoadLibraryA("sl.interposer.dll");
    Log("sl.interposer handle=%p", (void*)g_sl); }
  return g_sl ? (PFN_vkCreateSwapchainKHR)GetProcAddress(g_sl,"vkCreateSwapchainKHR") : nullptr;
}
}
```

- [ ] **Step 2: Hook `vkCreateSwapchainKHR` in `vkhooks.cpp`; route to the proxy**

```cpp
static VKAPI_ATTR VkResult VKAPI_CALL h_CreateSwapchainKHR(
    VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
    const VkAllocationCallbacks* a, VkSwapchainKHR* out){
  auto proxy = SlProxyCreateSwapchain();
  Log("vkCreateSwapchainKHR %ux%u fmt=%d proxy=%p", ci->imageExtent.width,
      ci->imageExtent.height, (int)ci->imageFormat, (void*)proxy);
  if (proxy) return proxy(dev, ci, a, out);   // <-- DLSS-G proxy swapchain in the present path
  return o_CreateSwapchainKHR(dev, ci, a, out);
}
```
Attach/detach it in Install/RemoveVkHooks alongside CreateDevice.

- [ ] **Step 3: Build.** Expected: links.

- [ ] **Step 4: In-game test (user).** Expected: `fgvk.log` shows `vkCreateSwapchainKHR ... proxy=<nonnull>`; game still renders (proxy swapchain is a valid drop-in). If the game black-screens, note it — arming may still work; proceed to Task 4 to read state.

- [ ] **Step 5: Commit** `feat: route swapchain through the Streamline DLSS-G proxy`.

### Task 4: slInit + slSetVulkanInfo + slDLSSGSetOptions(eOn); poll and log arming state

**Files:**
- Modify: `src/slboot.cpp`, `src/slboot.h`

**Interfaces:**
- Consumes: `gInstance/gPhysicalDevice/gDevice/gGraphicsQueue`.
- Produces: `OnDeviceCreated()` (called from h_CreateDevice) does slInit(once)+slSetVulkanInfo+DLSSGSetOptions(eOn,1); `PollDLSSGState()` logs the DLSS-G status each time it changes.

- [ ] **Step 1: `slInit` at DLL attach (add to LogInit path or first OnDeviceCreated)**

```cpp
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_consts.h>
static bool g_slInit=false;
static void EnsureSlInit(){
  if(g_slInit) return;
  sl::Preferences p{};
  static const sl::Feature feats[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
  p.featuresToLoad = feats; p.numFeaturesToLoad = 3;
  p.flags |= sl::PreferenceFlags::eUseManualHooking;   // we own the vk hooks
  p.renderAPI = sl::RenderAPI::eVulkan;
  sl::Result r = slInit(p, sl::kSDKVersion);
  fgvk::Log("slInit -> %d", (int)r);
  g_slInit = (r == sl::Result::eOk);
}
```

- [ ] **Step 2: `slSetVulkanInfo` + enable DLSS-G in `OnDeviceCreated()`**

```cpp
void fgvk::OnDeviceCreated(){
  EnsureSlInit();
  sl::VulkanInfo vi{};
  vi.device = gDevice; vi.instance = gInstance; vi.physicalDevice = gPhysicalDevice;
  vi.graphicsQueueIndex = 0; vi.graphicsQueueFamily = gGraphicsFamily;
  sl::Result r = slSetVulkanInfo(vi);
  Log("slSetVulkanInfo -> %d", (int)r);
  sl::DLSSGOptions o{}; o.mode = sl::DLSSGMode::eOn; o.numFramesToGenerate = 1;
  sl::ViewportHandle vp{0};
  sl::Result r2 = slDLSSGSetOptions(vp, o);
  Log("slDLSSGSetOptions(eOn,1) -> %d", (int)r2);
}
```
(Note: `gInstance` must be captured — add a `vkCreateInstance` hook in this task or read it from `ci` chain; simplest: hook `vkCreateInstance` to store `gInstance`.)

- [ ] **Step 3: Poll DLSS-G state (call from the present hook or a 2s timer) and log transitions**

```cpp
void fgvk::PollDLSSGState(){
  sl::DLSSGState st{}; sl::DLSSGOptions o{}; o.mode=sl::DLSSGMode::eOn; o.numFramesToGenerate=1;
  sl::ViewportHandle vp{0};
  if (slDLSSGGetState(vp, st, &o) == sl::Result::eOk){
    static uint32_t last=0xFFFFFFFF;
    if (st.status != last){ last=st.status;
      Log("DLSSG state: status=%u framesMax=%u", (unsigned)st.status, st.numFramesToGenerateMax); }
  }
}
```
Call `PollDLSSGState()` from a lightweight `vkQueuePresentKHR` hook (add it, forwarding to the native/proxy present).

- [ ] **Step 4: Build.** Expected: links against SL headers (header-only import macros; no SL .lib needed — functions resolve from sl.interposer at runtime).

- [ ] **Step 5: THE ARMING TEST (user launches, plays ~20s).**

Expected in `fgvk.log`: `slInit -> 0`, `slSetVulkanInfo -> 0`, `slDLSSGSetOptions(eOn,1) -> 0`, and a `DLSSG state: status=...` line whose status indicates **ON/active** (per `sl_dlss_g.h` `DLSSGStatus`). Also check the interposer's own SL log for the DLSS-G active transition.
**GATE:** if status reaches the ON/active value → M1 succeeds, proceed to M2. If it stays off/failed → STOP; debug arming (missing tags, viewport, or Reflex prerequisite) before any M2 work.

- [ ] **Step 6: Commit** `feat: slInit + DLSS-G enable + arming-state poll (M1 spike)`.

### Task 5: M1 verdict + decision record

- [ ] **Step 1:** Record the arming result (status value + which SL log lines) in `docs/M1-result.md`.
- [ ] **Step 2:** If armed: write the M2 plan (task group below → full plan). If not armed: open a debugging sub-plan (arming prerequisites: Reflex options set, valid viewport, required tags present even to arm, correct `eUseManualHooking` interposer wiring).
- [ ] **Step 3: Commit** `docs: M1 arming verdict`.

---

## M2–M5 — Task groups (expand to full tasks once M1 arms)

### M2 — Inputs + first generated frames
- **T2.1** Hook `NVSDK_NGX_VULKAN_EvaluateFeature`; for SR feature (id 1) read depth/mvec/color from `InParameters` via NGX getters (proven pattern from the BG3SE NGX tap). Produces `fgvk::SrInputs{ depth, mvec, color, dims }`.
- **T2.2** Per frame, build `sl::ResourceTag[]` (depth, mvec, hud-less/backbuffer) and `slSetTagForFrame`; set constants (camera matrices, jitter, `MvecScale=-1`) via the SL constants/`slSetConstants` path with `eUseFrameBasedResourceTagging`.
- **T2.3** Reflex + PCL markers per frame (`slReflexSleep`, `slPCLSetMarker` sim/render/present) carrying the frame token.
- **Gate:** DLSS-G watermark visible; displayed fps ≈ N×real.

### M3 — Recipe correctness
- **T3.1** MvecScale −1 + real per-subframe jitter; verify motion clean vs PureDark.
- **T3.2** Confirm mvec/depth at render res, matrices correct (`ClipToPrevClip` etc.); fix any transpose.
- **T3.3** (Optional improvement) add HUD-less tagging to fix hover ghosting his build shows.
- **Gate:** motion quality matches or beats PureDark.

### M4 — Config, multiplier, Reflex Boost
- **T4.1** INI parser (`mDLSSGFrames`/`mReflexMode`/`mDLSSPreset`); map frames→`numFramesToGenerate`.
- **T4.2** `slReflexSetOptions(LowLatency+Boost)`, no cap.
- **T4.3** Hotkey to cycle multiplier / toggle; `slDLSSGSetOptions` live update.
- **Gate:** 2x/3x/4x switch in-game; fps/1%-lows match his behavior.

### M5 — BG3SE + MCM coexistence
- **T5.1** Load fgvk through BG3SE's native loader (resolve loader mechanism; avoid the `upscaler.dll` name clash) with BG3SE active.
- **T5.2** Ensure fgvk's Vulkan/NGX hooks CHAIN with BG3SE's (both installed, either order) — no clobber.
- **T5.3** Run the compat SE; confirm the MCM renders on fgvk's proxy swapchain and is interactive with FG on.
- **Gate:** MCM works + FG works simultaneously (the thing PureDark's upscaler cannot do).
