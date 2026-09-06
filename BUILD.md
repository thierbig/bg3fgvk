# bg3fgvk - Build and Deploy

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
   `fgvk-stack.exe` next to it enables the automatic stack dump on a stall (developer only).
2. The Streamline runtime (exactly the seven files in `redist\`, Streamline 2.12.0) goes in
   `bin\NativeMods\Streamline\`; fgvk loads the interposer from there and points Streamline's
   plugin search at that folder. Fallback: the same seven files next to `bg3.exe`.

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
| `MvecScaleNormalized` | 1 | send Streamline the game's DLSS motion-vector scale divided by the mvec buffer size; Streamline multiplies it back by that size before the driver's DLSS-G sees it, so raw values (0) arrive ~1500x too large. `fgvk.log` prints `DLSS-G receives MvecScale=(...)`, expected -1,-1. 0 is for A/B only |
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

Overlays that detour the loader's exported `vkCreateDevice` (OptiScaler) resolve their present and
swapchain hooks from inside that call, which runs underneath Streamline's own vkCreateDevice, i.e.
inside fgvk's re-entry window where lookups normally go to the real loader. They would then hook the
driver's present under Streamline's pacer thread and deadlock DLSS-G when they draw (the OptiScaler
menu freeze). fgvk therefore also detours the exported `vkCreateDevice`, attached late in
`w_CreateInstance` so it is the outermost hook, and while it executes every export lookup gets the
game's view (`exportroute.h`, unit-tested). `fgvk.log` prints `export vkGetDeviceProcAddr(...) inside
loader vkCreateDevice (third-party hook) -> game view` when it happens.

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

## The Streamline runtime folder

DLSS Frame Generation itself is NVIDIA code. It ships as the **Streamline SDK runtime**, seven
files that bg3fgvk bundles unmodified (version 2.12.0) in `bin\NativeMods\Streamline\`:

```
sl.interposer.dll     Streamline core: takes over the Vulkan swapchain
sl.common.dll         shared plugin infrastructure
sl.dlss_g.dll         the DLSS Frame Generation plugin
sl.pcl.dll            PC latency markers
sl.reflex.dll         NVIDIA Reflex (required by frame generation)
nvngx_dlssg.dll       the frame generation neural network
NvLowLatencyVk.dll    Reflex helper for Vulkan
STREAMLINE-LICENSE.txt, README-STREAMLINE.txt
```

- **Leave the folder where it is**, next to `fgvk.dll`. `fgvk.dll` loads Streamline from there
  and tells it to look for its plugins there, so these files never conflict with copies another
  mod may have dropped into `bin\`.
- All seven must always come from the same Streamline release. Never mix files from two
  versions.
- **Updating Streamline:** download `streamline-sdk-vX.Y.Z.zip` from
  <https://github.com/NVIDIA-RTX/Streamline/releases>, open its `bin\x64\` folder (not
  `bin\x64\development\`, those are debug builds with an overlay) and replace all seven files
  in `bin\NativeMods\Streamline\` with the new ones.
- If the folder is missing, `fgvk.dll` falls back to looking for the same seven files next to
  `bg3.exe` and says so in `fgvk.log`.

## How it works (short version)

1. `fgvk.dll` is loaded by Native Mod Loader at startup and hooks `vkGetInstanceProcAddr`. The
   game builds its entire Vulkan dispatch through NVIDIA's `sl.interposer.dll`, so Streamline
   owns the device, the queues and the swapchain exactly as in a native integration.
2. It hooks the NGX call the game makes for DLSS Super Resolution and reads the depth buffer,
   motion vectors, jitter and the upscaled pre-UI image out of it. Those are tagged for DLSS-G
   every frame together with the camera constants.
3. Streamline's DLSS-G plugin generates the intermediate frames and paces their presentation
   in the driver. The mod's present hook only carries the Reflex/PCL frame markers and decides
   when frame generation should be on (3D world rendering) or suspended (menus, loading).
4. The exported `vkGetDeviceProcAddr` and swapchain functions of `vulkan-1.dll` are routed
   through the same view the game gets, so other mods that hook Vulkan (the Script Extender's
   ImGui overlay) see the presented frames and never run on Streamline's internal threads.

Details for developers are in `BUILD.md` and `docs/`.

## Release packaging

`package.ps1 -Version vX.Y.Z [-Build]` assembles `dist\bg3fgvk-vX.Y.Z.zip` laid out like the
game's `bin\` folder: `NativeMods\fgvk.dll`, `NativeMods\Streamline\` (the seven files from
`redist\`), README, INSTALL.txt and LICENSE. `fgvk-stack.exe` is deliberately not shipped.
Microsoft Detours and the Vulkan headers are expected where `CMakeLists.txt` points
(`External/` of a sibling checkout).
