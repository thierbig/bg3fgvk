# DLSS Frame Generation on RTX 20 / 30

Guide for the [bg3fgvk](../README.md) frame generation mod. **Confirmed on an RTX 3070 Ti**
(driver 616.92, 2560x1440, DLSS Quality): x2 frame generation, verified from the logs in
[issue #1](https://github.com/thierbig/bg3fgvk/issues/1). RTX 20 is untested; reports welcome.

The generated frames are NVIDIA's own DLSS-G, not FSR.

## Why a second mod is needed

NVIDIA's frame generation runtime ships its GPU kernels only for RTX 40 (`sm_89`) and RTX 50
(`sm_120`). On an RTX 30 the runtime loads, then fails per frame in
`vkCreateCuModuleNVX() ... failed -3` because those kernels cannot be created on Ampere.
[RTX30MFG-Unlock](https://github.com/mcsoderh/RTX30MFG-Unlock) (MIT) supplies the missing piece, and
unlike most mods of its kind it covers Vulkan as well as DirectX 12. bg3fgvk does its usual part:
Streamline, the game's depth, motion vectors and the HUD-less image.

This is unsupported research software on both sides. Single-player only.

## Requirements

- An **RTX 30** card, a recent NVIDIA driver, and Windows **Hardware-accelerated GPU scheduling** on.
- bg3fgvk installed as in the [README quick start](../README.md#quick-start), with the Vulkan
  renderer and **DLSS on** in Video settings.
- **Do not let another mod replace `bin\NativeMods\Streamline\nvngx_dlssg.dll`.** Other RTX 30 frame
  generation mods work by substituting that file; with bg3fgvk it must stay as shipped, because it is
  what tells Streamline your card is supported.
- In the NVIDIA App, Graphics → Baldur's Gate 3 → DLSS Override → **Frame Generation** set to
  "Use 3D application setting". An active override loads NVIDIA's own plugins from the NGX model
  store instead, and one tester's card was refused frame generation that way.

## Install

Four components, all in the game's `bin\` folder (the one with `bg3.exe`):

1. **Native Mod Loader** and **bg3fgvk**, as in the README quick start.
2. In `bin\NativeMods\fgvk.ini`, set `DLSSGFrames=1` (x2). Higher multipliers are not confirmed
   here, and RTX30MFG-Unlock's own notes report freezes above 2x.
3. **[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)** (x64), renamed to
   **`version.dll`**, in `bin\`. `bg3.exe` imports `VERSION.dll` directly, so it loads early enough.
   Do not name it `bink2w64.dll`; Native Mod Loader owns that name.
4. **[RTX30MFG-Unlock v1.2.2](https://github.com/mcsoderh/RTX30MFG-Unlock/releases/tag/v1.2.2)**: from
   `Universal-RTX-30-MFG-Unlock-v1.2.2.zip`, copy `RTX40MFG.asi` and `RTX40MFGCore.dll` into `bin\`,
   and merge the supplied `global.ini` (it carries `LoadExtraPlugins=RTX40MFG.asi`) next to the
   loader. `RTX40MFG-UI.addon64` is its ReShade menu and is not needed; the tested setup has no ReShade.

The file names keep the "RTX40" prefix from the project this fork is based on. That is expected.

## Verify

Play about 30 seconds of the 3D world, then read `bin\fgvk.log`:

```
slGetFeatureFunction(slDLSSGSetOptions) -> 0
DLSSG status=0 framesMax=5
gate: 60 consecutive DLSS-SR frames -> DLSS-G ON
FG stats: 300 presents -> 600 frames displayed (x2.00) status=0
```

`x2.00` is Streamline's own count of displayed frames per rendered frame. `x1.00` during menus,
videos and loading screens is normal: bg3fgvk suspends generation where the game runs no DLSS.

## Troubleshooting

| Log | Line | Meaning |
|---|---|---|
| `bin\sl.log` | `m_gpuArch = 0x170`, then `vkCreateCuModuleNVX() for Kernel_... failed -3` and `NGX create feature failed 0xbad00002` | NVIDIA's RTX 40 / 50 kernels reached your card, so the unlocker is not active. Check that the ASI loader really loaded it (step 3 and 4, and `global.ini`). |
| `bin\sl.log` | `Failed to obtain DLSS-G min spec requirements from NGX, using SL defaults`, then `Disabling DLSS-G since it is not supported on current hardware` | `nvngx_dlssg.dll` is gone from `bin\NativeMods\Streamline\`, or another mod replaced it. Restore bg3fgvk's copy: it answers that query with `0x160`, and without an answer Streamline falls back to its own "RTX 40 or newer" default. |
| `bin\sl.log` | `FeatureSupported == AdapterUnsupported`, then `Ignoring plugin 'sl.dlss_g'` | Streamline refused frame generation at startup. Turn the NVIDIA App DLSS Override off (Requirements). |
| `bin\fgvk.log` | `slGetFeatureFunction(slDLSSGSetOptions) -> 31` and `OnDeviceCreated: feature function resolution failed` | Same as above: frame generation was never handed to bg3fgvk. |
| `bin\fgvk.log` | `FG stats: ... (x1.00)` with no `DLSS-G ON` line | The game is not running DLSS. Re-select DLSS in Video settings; BG3 drops it after a failed start. |
| — | Frozen image or black screen at x3 / x4 | Known above 2x. Keep `DLSSGFrames=1`. |

## What does not work

Both were tested on the same RTX 3070 Ti, with logs in issue #1:

- **[dashdogy/RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock)**, the project
  RTX30MFG-Unlock is forked from: reported as half the screen missing. Its published source gates the
  Ampere backend behind DirectX 12 (its Vulkan hook for Ampere returns `false`), so use the fork above.
- **[SilyNoMeta's dlssg_for_sm86 fork](https://github.com/SilyNoMeta/dlssg_for_sm86)**: it installs
  correctly (`installed_310_9_1`, `redirect_ngx_cache`, `vulkan_init_ext2_resolvers` in its
  `dlssg3109.log`) and still cannot get RTX 30 kernels into the Vulkan path — 6297 consecutive
  `vkCreateCuModuleNVX() for Kernel_BlendCandidatesFused failed -3` in one session, with `x1.00`
  throughout. The original [sdli1995/dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) is
  DirectX 12 only by its own README, and ShyVortex's OptiScaler fork ships that same DirectX 12 build.

## Credits

- **deusimperatork**, who found the working combination and produced every log this page is built on.
- mcsoderh, for the RTX 30 fork that covers Vulkan, and dashdogy for the original unlocker.
