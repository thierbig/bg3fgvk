# bg3fgvk

Free, open-source **DLSS Frame Generation (x2 / x3 / x4) for Baldur's Gate 3** on Vulkan.
**Compatible with DLSS 5** (NVIDIA Neural Rendering): optional, through OptiScaler, see
[DLSS 5 alongside frame generation](#optional-dlss-5-neural-rendering-alongside-frame-generation).

Download: [Nexus Mods](https://www.nexusmods.com/baldursgate3/mods/24804) or the [GitHub Releases](https://github.com/thierbig/bg3fgvk/releases) page.

**Compatible with the Baldur's Gate 3 Script Extender (BG3SE) and Mod Configuration Menu (MCM).** Frame generation and the extender's UI work at the same time.

bg3fgvk is a small DLL that plugs NVIDIA Streamline's DLSS-G into the game's Vulkan swapchain.
The game's own DLSS Super Resolution provides depth and motion vectors; the NVIDIA driver
generates and paces the extra frames. There is no upscaler replacement, no menu of its own, and
no account or key. It coexists with the **Baldur's Gate 3 Script Extender** and mods that draw
their UI through it (Mod Configuration Menu included).

Measured on an RTX 50-series card at 2560x1440 with DLSS Quality: a 30 to 35 fps scene shows at
roughly 120 to 140 fps with x4 (Streamline itself reports the multiplier), with the GPU
otherwise untouched.

![Mod Configuration Menu open while DLSS Frame Generation runs at x4 (indicator top-left, 422 fps top-right)](docs/images/mcm-dlssg-x4.png)

## Quick start

1. Install [**Native Mod Loader**](https://www.nexusmods.com/baldursgate3/mods/944) (manual
   install: extract its `bin` folder over the game's `bin` folder and let it replace
   `bink2w64.dll`). It loads every DLL in `bin\NativeMods\`.
2. Download `bg3fgvk-vX.Y.Z.zip` from the Releases page and extract its contents into the game's
   `bin\` folder (the one with `bg3.exe`). Merge the `NativeMods` folder when asked. That puts
   `fgvk.dll` and a `Streamline` folder into `bin\NativeMods\`.
3. In the game's Video settings make sure the renderer is **Vulkan** and **DLSS** is on (any
   quality, or DLAA). Windows must have **Hardware-accelerated GPU scheduling** on.

Load a save. Frame generation starts by itself a couple of seconds into the world. Numpad `*`
turns it off and on. That is all.

---

## Requirements

| | |
|---|---|
| GPU | NVIDIA RTX 40 series for x2. RTX 50 series for x3 and x4 (multi frame generation). |
| Game | Baldur's Gate 3 in **Vulkan** mode (the default `bg3.exe`, not DirectX 11). |
| In-game | **DLSS must be enabled** in Video settings (any quality level, or DLAA). Frame generation reads the game's DLSS inputs; with DLSS off it stays idle. |
| Loader | [Native Mod Loader](https://www.nexusmods.com/baldursgate3/mods/944) (the `bink2w64.dll` replacement). It loads every DLL in `bin\NativeMods\`. |

---

## Installation

Everything goes into the game's `bin` folder, normally
`C:\Program Files (x86)\Steam\steamapps\common\Baldurs Gate 3\bin` (or wherever `bg3.exe` is).

### 1. Native Mod Loader

Skip this if you already have it (you do if `bin\NativeMods\` exists with other `.dll` mods in it).

1. Download **Native Mod Loader** from Nexus Mods:
   <https://www.nexusmods.com/baldursgate3/mods/944> (Files tab, manual download; do not use a
   mod manager for it).
2. Open the zip. It contains a `bin` folder with `bink2w64.dll` (the loader) and
   `bink2w64_original.dll` (the game's original, kept as a backup).
3. Extract that `bin` folder over the game's `bin` folder and confirm replacing `bink2w64.dll`.
4. Make sure `bin\NativeMods\` exists; create the folder if it does not.

The loader itself does nothing visible. Every `.dll` placed in `bin\NativeMods\` is loaded when
the game starts, and `bin\NativeModLoader.log` lists what it loaded.

### 2. Extract the release zip into `bin\`

The release zip is laid out like the `bin` folder. Extract its **contents** straight into `bin\`
(say yes to merging the `NativeMods` folder). You end up with:

```
bin\NativeMods\fgvk.dll                   the mod
bin\NativeMods\Streamline\               NVIDIA Streamline runtime (7 files, see below)
bin\README.md, INSTALL.txt, LICENSE.txt    docs (can be deleted)
```

That is the whole installation. Nothing needs to be copied next to `bg3.exe`.

### 3. First launch

Start the game, load a save, and play for a few seconds. On first launch the mod writes its
config file `bin\NativeMods\fgvk.ini` with defaults and a log `bin\fgvk.log`.

Frame generation turns itself on about two seconds after the 3D world starts rendering and
suspends itself on loading screens, videos, and full-screen menus where the game does not run
DLSS. You do not need to do anything.

To confirm it is working, open `bin\fgvk.log` and look for lines like:

```
Streamline runtime: C:\...\bin\NativeMods\Streamline (loaded)
gate: 60 consecutive DLSS-SR frames -> DLSS-G ON
FG stats: 300 presents -> 1200 frames displayed (x4.00) status=0
```

`x4.00` is Streamline's own count of displayed frames per rendered frame. If you see `x1.00`
while playing, see Troubleshooting.

---

## The `Streamline` folder

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

---

## Configuration: `bin\NativeMods\fgvk.ini`

Read once at game start.

| Key | Default | What it does |
|---|---|---|
| `DLSSGFrames` | `3` | Generated frames per rendered frame. `1` = x2, `2` = x3, `3` = x4. Clamped to what the GPU supports (RTX 40 = 1). |
| `ReflexMode` | `2` | NVIDIA Reflex, which DLSS-G requires. `1` = Low Latency, `2` = Low Latency + Boost. `0` = off (frame generation will not activate). |
| `ReflexSleep` | `1` | Keep at `1`. |
| `TagHUDLess` | `1` | Feeds the game's pre-UI image to DLSS-G so the HUD does not smear over a moving background. Keep at `1`; `0` is only for comparison. |
| `TagUI` | `0` | Experimental transparent UI layer. Leave at `0`. |
| `OnAfterEvalFrames` | `60` | Frames of 3D rendering before frame generation switches on. |
| `OffAfterIdleFrames` | `30` | Frames without 3D rendering (menus, loading) before it suspends. |
| `KeyToggleFG` | `0x6A` | Hotkey: frame generation on/off. `0x6A` is numpad `*`. `0` disables the hotkey. |
| `KeyCycleFrames` | `0x23` | Hotkey: cycle x2, x3, x4. `0x23` is `End`. |

Hotkeys use Windows virtual-key codes, decimal or hex.

### The NVIDIA App can override the multiplier

If you have a per-game **DLSS Override, Frame Generation** setting for Baldur's Gate 3 in the
NVIDIA App (for example "4x"), the driver applies it on top of this mod and the `DLSSGFrames`
setting and the `End` hotkey will appear to do nothing. Set that override to
"Use 3D application setting" if you want the mod to control the multiplier. The mod works
either way.

---

## Hotkeys

| Key | Action |
|---|---|
| Numpad `*` | Frame generation on / off |
| `End` | Cycle x2, x3, x4 (see the NVIDIA App note above) |

---

## What to expect

- The displayed frame rate is roughly 4x, 3x or 2x the rendered one. Use an external counter
  such as the NVIDIA App overlay; the game's own FPS display cannot see generated frames.
- Generated frames add a little input latency compared to no frame generation. Reflex Boost
  keeps it in check. A rendered frame rate of 30 fps or more is where x4 looks good; below that,
  x2 or x3 is the better trade.
- Frame generation pauses when the game window loses focus, on loading screens, and in videos.
  It resumes on its own.
- Very fast camera pans can show light smearing. That is a property of the technique at this
  frame rate, not a bug.

---

## Optional: DLSS 5 Neural Rendering alongside frame generation

bg3fgvk works together with **DLSS 5**, NVIDIA's Neural Rendering pass, through the
[OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR) fork of OptiScaler by Dagherbou.
Both run at once: OptiScaler passes the game's DLSS Super Resolution call through to the driver and
runs the Neural Rendering model over the upscaled image in place, and bg3fgvk reads that same call for
frame generation. The generated frames carry the Neural Rendering result too.

![DLSS 5 Neural Rendering on a character close-up with DLSS Frame Generation x4 running](docs/images/dlss5-neural-rendering-closeup.png)

Verified on 2026-09-04 with an RTX 5070 Ti, driver 610.88, 2560x1440, DLSS-G x4. NVIDIA's own
on-screen indicators (a registry switch, off by default) show all three features live:

![NVIDIA DLSS-G indicator: v310.9.0, VK, 4x, Hudless: Yes](docs/images/dlss5-indicator-dlssg.png)
![NVIDIA DLSS indicator with the Neural Rendering weights line underneath](docs/images/dlss5-indicator-dlss-nr.png)

None of this is supported by NVIDIA. The fork drives an undocumented model and is under active
development; check its release notes.

### Requirements

- An **RTX 50 series** card. The Neural Rendering model does not run on older GPUs.
- `nvngx_dlssnr.dll`, NVIDIA's model DLL (about 165 MB, "NVIDIA DLSSNR" in its file properties).
  It comes from an NVIDIA driver package. Neither the fork nor bg3fgvk ships it.
- The fork's notes ask for driver 616.56 or newer. The stock 310.8.0 model also ran here on 610.88.
- bg3fgvk installed and working first (`x4.00` in `fgvk.log`).

### Install

1. Download `OptiScaler-DLSSNR-vX.Y.Z.zip` from the fork's
   [Releases](https://github.com/Dagherbou/OptiScaler_DLSSNR/releases) page and extract everything
   into the game's `bin\` folder, next to `bg3.exe`: the `OptiScaler\` folder, `Licenses\`,
   `nvngx.dll_dlssnr.dll`, `OptiScaler.dll` and `OptiScaler.ini`.
2. Rename `OptiScaler.dll` to **`dxgi.dll`** (or run `setup_windows.bat` and pick option 1).
   The script suggests `winmm.dll` for Vulkan games, but `bg3.exe` does not import winmm; it does
   import dxgi, so that one loads reliably.
3. Put `nvngx_dlssnr.dll` into `bin\` as well.
4. Edit `bin\OptiScaler.ini`:

| Section | Key | Set to | Why |
|---|---|---|---|
| `[Upscalers]` | `VulkanUpscaler` | `dlss` | **Required.** OptiScaler's default for Vulkan games is FSR 2.2, which silently replaces the game's DLSS. Frame generation would still run, on FSR. |
| `[DlssNr]` | `Enabled` | `true` | Turns Neural Rendering on without opening the menu. |
| `[Spoofing]` | `StreamlineSpoofing` | `false` | Otherwise Streamline can be shown a spoofed RTX 4090 and cap frame generation at x2. |
| `[Spoofing]` | `Dxgi` | `false` | No spoofing on an NVIDIA GPU. |
| `[Menu]` | `OverlayMenu` | `false` | Recommended, see the known issue below. Everything is set from the ini anyway. |

5. Launch the game and load a save. `bin\OptiScaler.log` (file logging is on in the fork's ini)
   shows `DLSS-NR Vulkan: the model initialised on this device`. `bin\fgvk.log` still shows
   `gate: ... DLSS-G ON` and `x4.00`. Its hook line now reads `NGX hooks: module=_nvngx.dll`, which
   is expected: bg3fgvk hooks the driver's NGX core that OptiScaler loads.

### Cost

The model runs at output resolution on every rendered frame. At 2560x1440 on an RTX 5070 Ti it
roughly halved the rendered frame rate; with x4 frame generation the displayed rate stays high.
`WorkingScale` under `[DlssNr]` (for example `0.75`) runs the model smaller to win some of that
back. The fork's ini and readme document the other knobs.

### Known issue: the OptiScaler menu freezes the game while frame generation is on

OptiScaler's overlay hooks the driver's present call underneath Streamline. With the menu open it
injects GPU work and fence waits on Streamline's frame-pacing thread, and DLSS-G deadlocks: no
crash dump, the game just stops. Either keep `OverlayMenu=false` and configure through the ini, or
suspend frame generation with Numpad `*` before pressing `Insert` and turn it back on after closing
the menu. The boot-screen menu is safe because frame generation is not running there. A fix on the
bg3fgvk side is being looked at.

### Removing it

Delete `dxgi.dll`, `OptiScaler.ini`, `OptiScaler.log`, `nvngx.dll_dlssnr.dll`, `nvngx_dlssnr.dll`
and the `OptiScaler\` and `Licenses\` folders from `bin\`. bg3fgvk itself is untouched.

---

## Troubleshooting

All diagnostics are in `bin\fgvk.log` (this mod) and `bin\sl.log` (Streamline's own verbose log).

**`FG stats` shows `x1.00` while playing.**
- Check the `gate:` lines. If DLSS-G never turned ON, the game is not running DLSS: enable DLSS
  in Video settings. Note that BG3 silently drops the DLSS setting if DLSS ever failed to
  initialize on a previous launch (for example after a broken install); just select DLSS again
  under Options, Video, Upscaling.
- Check the `DLSSG status=` line: `status=0` is good. `status=2` means Reflex is off (set
  `ReflexMode` to 1 or 2). `status=1` means the resolution is too low.
- The window must have focus.

**The game does not start, or `fgvk.log` stops after `slInit`.**
- The seven Streamline files are missing from `bin\` or mixed from different versions.
- Hardware-accelerated GPU scheduling is off.

**Black screen or freeze.**
- Attach `bin\fgvk.log` and `bin\sl.log` to a bug report. (Developers: build `fgvk-stack.exe`
  from this repo and put it next to `fgvk.dll`; the mod then dumps every thread's stack to
  `bin\fgvk-stacks.log` when the present loop stalls.)
- Another mod that hooks Vulkan (an overlay, a different frame generator) is the usual cause.
  Streamline's log will show `WaitSemaphores ... timed out` right before the stall.
- If OptiScaler (the DLSS 5 setup above) is installed and you opened its menu with frame
  generation on, that is the known freeze; see the DLSS 5 section.

**Script Extender mods stop drawing their UI.**
- Not expected: bg3fgvk routes the Vulkan functions the Script Extender hooks so its overlay
  lands on the frames the game actually presents. If it happens, report it with the logs.

---

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

---

## Building from source

Visual Studio 2022 with C++ and CMake. The Vulkan headers and Microsoft Detours are expected
where `CMakeLists.txt` points (`External/` of a sibling checkout, see `BUILD.md`).

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output: `build\Release\fgvk.dll` (the mod) and `build\Release\fgvk-stack.exe` (developer
diagnostics, not part of releases). Copy `fgvk.dll` into `bin\NativeMods\` and the seven files
from `redist\` into `bin\NativeMods\Streamline\`. `package.ps1 -Version vX.Y.Z [-Build]`
assembles `dist\bg3fgvk-vX.Y.Z.zip` in that layout.

---

## Credits and licenses

- bg3fgvk is released under the MIT License (see `LICENSE`).
- The NVIDIA Streamline SDK 2.12.0 runtime (`sl.*.dll`, `nvngx_dlssg.dll`, `NvLowLatencyVk.dll`)
  is bundled unmodified from NVIDIA's release package under its license, included as
  `STREAMLINE-LICENSE.txt`. Copyright NVIDIA Corporation.
- Microsoft Detours (MIT) for the hooks.
- Norbyte's Baldur's Gate 3 Script Extender, which this mod is built to coexist with.
- PureDark's frame generation mod for Baldur's Gate 3, whose DLSS-G input recipe served as the
  reference for what the game's buffers look like.

This is a fan-made mod. It is not affiliated with Larian Studios or NVIDIA.
