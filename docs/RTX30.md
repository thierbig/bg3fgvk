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
- No other frame generation mod (OptiScaler frame generation, dlssg-to-fsr3, ...).

## Install

1. Download **dlssg-310.9.1-11-win64.zip** from the fork's
   [release page](https://github.com/SilyNoMeta/dlssg_for_sm86/releases/tag/v310.9.1-11).
2. Copy its `version.dll` and `dlssg_sm86.ini` into the game's `bin\` folder, next to `bg3.exe`.
   `bg3.exe` loads `version.dll` at startup, before bg3fgvk sets up Streamline. If `bin\` already
   has a `version.dll` from another mod, do not overwrite it: say so in the issue. The optional
   `DLSSGControls.addon64` (a ReShade panel) is not needed.
3. For the first run, open `bin\NativeMods\fgvk.ini` and set `DLSSGFrames=1` (x2).
4. Launch, load a save, and move the camera around in the world for about 30 seconds. Quit.

To uninstall, delete `version.dll` and `dlssg_sm86.ini` from `bin\`.

## What to check

| Log | Line | Meaning |
|---|---|---|
| `bin\fgvk.log` | `gate: 60 consecutive DLSS-SR frames -> DLSS-G ON`, then `FG stats: ... (x2.00)` | **It works.** Press `End` for x3 and x4 and check `FG stats` again. |
| `bin\fgvk.log` | `OnDeviceCreated: feature function resolution failed` | Streamline dropped frame generation. Check `sl.log` below. |
| `bin\sl.log` | `Ignoring plugin 'sl.dlss_g' since it is not supported on this platform` | The fork did not reach bg3fgvk's Streamline, which lives in `bin\NativeMods\Streamline\` rather than next to `bg3.exe`. Probably fixable on bg3fgvk's side; the logs show how. |
| `dlssg3109.log` (the fork's log, look next to `bg3.exe`) | `proxy_attached` / `installed_310_9_1` | The fork loaded / took over the frame generation runtime. |
| `bin\fgvk.log` | `FG stats: ... (x1.00)` with no `DLSS-G ON` line | The game is not running DLSS. Re-select DLSS in Video settings (BG3 drops it after a failed start). |

## Reporting

Post in [issue #1](https://github.com/thierbig/bg3fgvk/issues/1): GPU model, driver version, and
the three logs (`bin\fgvk.log`, `bin\sl.log`, `dlssg3109.log`), even when it works. A word on how it
looks helps too: HUD stability, fast camera pans, and x3 / x4 if you tried them.

## Credits

- sdli1995, for `dlssg_for_sm86` and the Ampere kernels.
- SilyNoMeta, for the fork's Vulkan path.
