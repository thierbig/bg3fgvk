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

Copy the built DLL and Streamline runtime to the game directory:

1. Copy `fgvk.dll` to `C:\Games\Baldurs Gate 3\bin\`
2. Copy all Streamline/NGX DLLs from `C:\Games\Baldurs Gate 3\bin\mods\UpscalerBasePlugin\Streamline\` to `C:\Games\Baldurs Gate 3\bin\`:
   - `sl.interposer.dll`
   - `sl.common.dll`
   - `sl.dlss_g.dll`
   - `sl.pcl.dll`
   - `sl.reflex.dll`
   - `nvngx_dlssg.dll`
   
   *(Copy the entire Streamline folder's DLLs to be safe.)*

## Load fgvk.dll into the game

**Options** (choose one):

- **Manual injector**: Use a manual DLL injector to load `fgvk.dll` after the game launches.
- **BG3SE hook** (advanced): Temporarily add a `LoadLibraryW(L"fgvk.dll")` call in the BG3SE compat build's hook init (it already handles `upscaler.dll` this way).

## Important for the spike

- **Run WITHOUT PureDark's `upscaler.dll` active.** Only ONE thing should route the swapchain through Streamline to isolate arming behavior.
- **Initially run WITHOUT BG3SE.** Isolate arming first, then test BG3SE coexistence in M5.

## Verify arming

1. Launch BG3 with the loaded `fgvk.dll`.
2. Play for ~20 seconds to trigger rendering and Streamline initialization.
3. Locate the log file at `C:\Games\Baldurs Gate 3\bin\fgvk.log`.
4. Read the `slInit`, `slSetVulkanInfo`, `slDLSSGSetOptions`, and **`DLSSG status=`** lines.
5. Record findings in `docs/M1-result.md`.
