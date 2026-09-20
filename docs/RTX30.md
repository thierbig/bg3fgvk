# Real DLSS Frame Generation on RTX 20 / 30 (testing)

Optional guide for the [bg3fgvk](../README.md) frame generation mod. **Not confirmed yet**: nobody
has run this combination on an RTX 20 or 30 card. This page is the test plan; report results in
[issue #1](https://github.com/thierbig/bg3fgvk/issues/1).

## Why a second mod is needed

NVIDIA's frame generation runtime (`nvngx_dlssg.dll`) ships its GPU kernels for RTX 40 and RTX 50
only, so on older cards Streamline refuses to load it. sdli1995's
[dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) rebuilt those kernels for RTX 30
(Ampere) and RTX 20 (Turing), but only swaps them in on DirectX 12. BG3's DLSS renderer is Vulkan,
where the runtime loads its kernels through a different driver path.

[SilyNoMeta's fork](https://github.com/SilyNoMeta/dlssg_for_sm86) adds that Vulkan path, and users
report it working in No Man's Sky, RTX Remix games and Doom: The Dark Ages. bg3fgvk still does its
usual part (Streamline, the game's depth, motion vectors and HUD-less image); the fork supplies an
NVIDIA frame generation runtime that runs on your card. The generated frames are NVIDIA's, not FSR.

The fork is a closed-source pre-release. Single-player only.

## Requirements

- An **RTX 30** card. The fork's RTX 20 route is untested on real hardware; reports welcome.
- A recent NVIDIA driver (the fork's kernels want roughly R580 or newer).
- bg3fgvk installed as in the [README quick start](../README.md#quick-start): Vulkan renderer,
  DLSS on in Video settings, Hardware-accelerated GPU scheduling on.
- No other frame generation mod (OptiScaler frame generation, dlssg-to-fsr3, ...). That includes
  the [optional OptiScaler test](#optional-second-test-shyvortexs-optiscaler-expected-to-fail)
  below: run one test at a time, never both unlockers together.

## Install

1. Download **dlssg-310.9.1-11-win64.zip** from the fork's
   [release page](https://github.com/SilyNoMeta/dlssg_for_sm86/releases/tag/v310.9.1-11).
2. Copy its `version.dll` and `dlssg_sm86.ini` into the game's `bin\` folder, next to `bg3.exe`.
   `bg3.exe` loads `version.dll` at startup, before bg3fgvk sets up Streamline. If `bin\` already
   has a `version.dll` from another mod, do not overwrite it: say so in the issue. The optional
   `DLSSGControls.addon64` (a ReShade panel) is not needed.
3. Rename `bin\NativeMods\Streamline\nvngx_dlssg.dll` to `nvngx_dlssg.dll.off`. **This step comes
   from the first RTX 30 reports** (see [What the first reports showed](#what-the-first-reports-showed)):
   NGX loads that bundled copy by path, which shadows the driver's copy, and the driver's copy is
   the one the fork replaces with its RTX 30 build. Put the name back to run bg3fgvk without the fork.
4. In the NVIDIA App, under Graphics → Baldur's Gate 3 → DLSS Override, set **Frame Generation** to
   "Use 3D application setting". With the override on, Streamline loads NVIDIA's own override plugin
   from `C:\ProgramData\NVIDIA\NGX\models\` instead, which the fork does not appear to patch.
5. For the first run, open `bin\NativeMods\fgvk.ini` and set `DLSSGFrames=1` (x2).
6. Launch, load a save, and move the camera around in the world for about 30 seconds. Quit.

To uninstall, delete `version.dll` and `dlssg_sm86.ini` from `bin\`, and rename
`nvngx_dlssg.dll.off` back.

## What to check

| Log | Line | Meaning |
|---|---|---|
| `bin\fgvk.log` | `gate: 60 consecutive DLSS-SR frames -> DLSS-G ON`, then `FG stats: ... (x2.00)` | **It works.** Press `End` for x3 and x4 and check `FG stats` again. |
| `bin\fgvk.log` | `OnDeviceCreated: feature function resolution failed` | Streamline dropped frame generation. Check `sl.log` below. |
| `bin\sl.log` | `Ignoring plugin 'sl.dlss_g' since it is not supported on this platform` | The fork did not reach bg3fgvk's Streamline, which lives in `bin\NativeMods\Streamline\` rather than next to `bg3.exe`. Probably fixable on bg3fgvk's side; the logs show how. |
| `dlssg3109.log` (the fork's log, look next to `bg3.exe`) | `proxy_attached` | The fork loaded. |
| `dlssg3109.log` | `installed_310_9_1` | It replaced the frame generation runtime with its RTX 30 build. |
| `dlssg3109.log` | `vulkan_backend_active` | Its Vulkan path is on (the part the original `dlssg_for_sm86` lacks). |
| `dlssg3109.log` | `vulkan_kernel_launches` | Frame generation kernels are actually running on the card. |
| `bin\fgvk.log` | `FG stats: ... (x1.00)` with no `DLSS-G ON` line | The game is not running DLSS. Re-select DLSS in Video settings (BG3 drops it after a failed start). |
| `bin\sl.log` | `m_gpuArch = 0x170`, then `vkCreateCuModuleNVX() for Kernel_... failed -3` and `NGX create feature failed 0xbad00002` | NVIDIA's stock RTX 40 / 50 kernels reached your card: the runtime in use is not the fork's. Check step 3 and attach `dlssg3109.log`. |
| `bin\sl.log` | `FeatureSupported == AdapterUnsupported`, then `Disabling DLSS-G since it is not supported on current hardware` | Streamline dropped frame generation before it ever ran. Check step 4, and that the fork loaded at all. |
| `dlssg3109.log` missing entirely | The fork never loaded. Check that `bin\version.dll` is the fork's file and that no other mod owns that name. |

## What the first reports showed

Two RTX 30 reports on 2026-09-19 ([issue #1](https://github.com/thierbig/bg3fgvk/issues/1)), before
steps 3 and 4 existed. Neither included `dlssg3109.log`, so whether the fork was loaded is unconfirmed.

- **RTX 3070 Ti, driver 616.92.** Everything armed: `slDLSSGSetOptions -> 0`, `DLSSG status=0
  framesMax=5`, and the gate turned DLSS-G on. Then every frame failed in
  `vkCreateCuModuleNVX() for Kernel_BlendCandidatesFused failed -3` after `m_gpuArch = 0x170`, so
  `FG stats` stayed at `x1.00`. Those are NVIDIA's stock kernels, which exist only for `sm_89` and
  `sm_120`. `sl.log` shows NGX loading the runtime from `bin\NativeMods\Streamline/nvngx_dlssg.dll`,
  bg3fgvk's own bundled copy — hence step 3. Worth noting: NVIDIA's current snippet reported
  `Snippet expects at least : 0x160`, so the architecture gate was not the blocker here, only the kernels.
- **RTX 3060, driver 616.56.** Streamline never got that far: NGX answered
  `FeatureSupported == AdapterUnsupported` for feature 11, `sl.dlss_g` was dropped at startup, and
  bg3fgvk logged `OnDeviceCreated: feature function resolution failed`. That machine had an NVIDIA
  App DLSS Override active, so Streamline used override plugins from `C:\ProgramData\NVIDIA\NGX\models\`
  — hence step 4.

## Optional second test: ShyVortex's OptiScaler (expected to fail)

[ShyVortex's OptiScaler fork](https://github.com/ShyVortex/OptiScaler-DLSSNR-PreSR-Multipass)
also has an RTX 20 / 30 unlock. **It will most likely not work in BG3**: the unlocker it ships
(`OptiScaler\dlssg_sm86\dlssg_sm86.dll`, sdli1995's older "Native 0.2.3" build) launches its kernels
only through DirectX 12 and contains no Vulkan code. It is listed so that a quick run can confirm
or rule it out. Do the SilyNoMeta test above first.

1. Remove the SilyNoMeta fork: delete `version.dll` and `dlssg_sm86.ini` from `bin\`.
2. Download **OptiScaler-NR-v0.9.6.zip** from the
   [release page](https://github.com/ShyVortex/OptiScaler-DLSSNR-PreSR-Multipass/releases/tag/v0.9.6)
   and extract everything into `bin\`, next to `bg3.exe`.
3. Rename `OptiScaler.dll` to **`dxgi.dll`** (`bg3.exe` loads dxgi; it does not load winmm).
4. In `bin\OptiScaler.ini` set:
   - `[DLSSG]` `AmpereMfgUnlock=true` (the RTX 20 / 30 unlock, off by default).
   - `[Upscalers]` `VulkanUpscaler=dlss`. Otherwise OptiScaler swaps the game's DLSS for FSR and
     bg3fgvk has nothing to read.
5. Keep `DLSSGFrames=1` in `fgvk.ini`, launch, and play about 30 seconds as before.

Check `bin\OptiScaler.log` for `AmpereMfgLoader: SM86/SM75 MFG loaded successfully`, then the same
`fgvk.log` and `sl.log` lines as in the table above. Expect `FG stats: ... (x1.00)` or a crash; an
`x2.00` would be a surprise worth reporting. To uninstall, delete what the zip added (`dxgi.dll`,
`OptiScaler.ini`, the `OptiScaler\` folder and the other extracted files).

## Reporting

Post in [issue #1](https://github.com/thierbig/bg3fgvk/issues/1): which test (SilyNoMeta or
OptiScaler), GPU model, driver version, and the logs (`bin\fgvk.log`, `bin\sl.log`, plus
`dlssg3109.log` or `bin\OptiScaler.log`), even when it works. Say whether `bin\version.dll` is the
fork's file, and if `dlssg3109.log` does not exist say that too: it means the fork never loaded, which
is as useful to know as a failure inside it. A word on how it looks helps too:
HUD stability, fast camera pans, and x3 / x4 if you tried them.

## Credits

- sdli1995, for `dlssg_for_sm86` and the Ampere kernels.
- SilyNoMeta, for the fork's Vulkan path.
- ShyVortex, for the OptiScaler fork with the RTX 20 / 30 unlock.
