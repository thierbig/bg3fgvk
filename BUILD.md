# Build and Deploy Instructions

## Prerequisites

- Visual Studio 2022 with C++ and CMake, **OR** standalone CMake + MSVC Build Tools x64.
- Vulkan SDK **not required to build** — headers are vendored at `C:/Dev/bg3se-upscaler/External/VulkanSDK/Include`.
- Streamline/NGX runtime DLLs required **to run** (deployed in step 3).

## Build

```bash
cmake -S C:/Dev/fgvk -B C:/Dev/fgvk/build -A x64
cmake --build C:/Dev/fgvk/build --config Release
```

**Output:** `C:/Dev/fgvk/build/Release/fgvk.dll`

## Deploy

The DLL is loaded by **Native Mod Loader** (the `bink2w64.dll` replacement in `bin\`), which
loads every DLL in `bin\NativeMods\` at startup.

1. Copy `build\Release\fgvk.dll` to `C:\Games\Baldurs Gate 3\bin\NativeMods\fgvk.dll`
   (the game must not be running; keep the previous DLL as `fgvk.dll.bakX` for quick rollback).
2. The Streamline/NGX runtime must be in `bin\` (already deployed; the set fgvk expects is
   exactly Streamline 2.12.0): `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.pcl.dll`,
   `sl.reflex.dll`, `nvngx_dlssg.dll`, `NvLowLatencyVk.dll` (sl.reflex's Vulkan helper - BG3
   itself has no Reflex integration).
3. Run WITHOUT PureDark's `upscaler.dll` (parked as `upscaler.dll.parked`).

## Logs

- `bin\fgvk.log` - ours. Every line: wall clock + seconds since DllMain. Key lines: `gate:`
  (DLSS-G on/suspended and why), `FG stats:` (real multiplier over 300 presents; ~x4 = MFG
  working, x1 = nothing generated), `DLSSG status=`, `TIMING:`, `wd:` (watchdog; `STALLED`
  with `pos=` names where the present thread is stuck).
- `bin\sl.log` - Streamline verbose. Fatal signature so far: `waitCPUFence ... WaitSemaphores
  ... timed out` (500 ms cap) followed by a black screen and freeze.

## Configuration

`bin\NativeMods\fgvk.ini` is written with defaults on first run and read at game start:

| Key | Default | Meaning |
|---|---|---|
| `DLSSGFrames` | 3 | generated frames per real frame (1 = x2, 2 = x3, 3 = x4) |
| `ReflexMode` | 2 | 0 off, 1 low latency, 2 low latency + boost |
| `ReflexSleep` | 1 | call slReflexSleep once per frame |
| `TagHUDLess` | 1 | feed the DLSS-SR output as HUD-less color (PureDark's recipe; 0 = backbuffer only) |
| `TagUI` | 0 | feed a transparent UI color+alpha layer |
| `OnAfterEvalFrames` | 60 | DLSS-SR frames before DLSS-G turns on |
| `OffAfterIdleFrames` | 30 | presents without DLSS-SR before DLSS-G suspends |
| `KeyToggleFG` | 0x6A (numpad *) | hotkey: DLSS-G on/off |
| `KeyCycleFrames` | 0x23 (End) | hotkey: cycle x2 / x3 / x4 |

## Script Extender coexistence

BG3SE (stock and the custom `upscaler-run` build) links `vulkan-1.lib` and resolves the
functions it hooks through the EXPORTED `vkGetDeviceProcAddr`, then Detours what it gets. Without
help that is the driver's entry points, which puts extender code (and its `fgQueueMutex_`) on
Streamline's pacer thread (deadlock: `fgvk-stacks.log` shows `sl.dlss_g -> sl.common ->
BG3ScriptExtender -> vulkan-1`), and its overlay fetches the driver's real swapchain images while
the game renders into Streamline's fake buffers (overlay never shows).

fgvk therefore also detours the vulkan-1 exports `vkGetDeviceProcAddr`, `vkGetSwapchainImagesKHR`,
`vkAcquireNextImageKHR` and `vkQueuePresentKHR`: any import-based caller gets the same view the
game gets (fgvk wrappers + interposer), so extender detours land on fgvk's wrappers on the game
thread and its image queries return the presented (fake) buffers. Calls from inside the interposer
still reach the loader. If the extender must be excluded for a test, rename `bin\DWrite.dll` and
`bin\ScriptExtender.dll` to `*.fgvk-off`.

## NVIDIA App overrides

The NVIDIA App applies BG3's per-game DLSS overrides to fgvk's Streamline instance (the on-screen
indicator says `nvapp_override`, sl.log logs `Override Reported: ...`). A Frame Generation override
("4x") pins `numFramesToGenerate` to 3 regardless of `DLSSGFrames` or the End hotkey. To let fgvk
control the multiplier: NVIDIA App -> Graphics -> Baldur's Gate 3 -> DLSS Override, Frame
Generation -> "Use 3D application setting". The Model Preset override also disables UI
recomposition inside DLSS-G.

## Verify arming

1. Launch BG3 with the loaded `fgvk.dll`.
2. Play for ~20 seconds to trigger rendering and Streamline initialization.
3. Locate the log file at `C:\Games\Baldurs Gate 3\bin\fgvk.log`.
4. Read the `slInit`, `slSetVulkanInfo`, `slDLSSGSetOptions`, and **`DLSSG status=`** lines.
5. Record findings in `docs/M1-result.md`.
