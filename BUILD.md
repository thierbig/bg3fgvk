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

## Verify arming

1. Launch BG3 with the loaded `fgvk.dll`.
2. Play for ~20 seconds to trigger rendering and Streamline initialization.
3. Locate the log file at `C:\Games\Baldurs Gate 3\bin\fgvk.log`.
4. Read the `slInit`, `slSetVulkanInfo`, `slDLSSGSetOptions`, and **`DLSSG status=`** lines.
5. Record findings in `docs/M1-result.md`.
