# fgvk — a free, standalone Vulkan DLSS Frame Generation injector

**Status:** design / spec (2026-08-27)
**Working name:** `fgvk` (rename freely)

## Goal

A single injector DLL that adds NVIDIA DLSS Frame Generation to Baldur's Gate 3
(Vulkan) by routing the game's swapchain through NVIDIA Streamline's DLSS-G proxy
swapchain, so the **driver** performs frame generation and present pacing. It is a
free, open replacement for PureDark's Patreon-gated `upscaler.dll`, and — unlike his —
it must **not break the BG3SE ImGui MCM**.

One sentence: *hook Vulkan, hand the game's swapchain and its DLSS-SR inputs to
Streamline DLSS-G, let the driver generate and pace, and present a swapchain the
existing BG3SE compat overlay can still draw on.*

## Why this architecture (and why the previous one failed)

The prior effort (BG3SE `dlss-fg` branch) took the **direct-NGX** path: it called the
DLSS-G generator itself (`NVSDK_NGX_VULKAN_EvaluateFeature`) and did frame **pacing and
presentation in userspace**. That path works but has a hard ceiling: a userspace present
hook serializes acquire + submit + present on the game's single present queue, so
generation/pacing on our side either starves the game (deadlock) or stalls it (1% lows).
Measured: our GPU sat ~50%; PureDark's ~90%. A userspace hook cannot match a
driver-internal pacer.

Root cause of the earlier "sl.dlss_g never arms" dead end: to kill a "zombie
VK_NOT_READY" lag, that branch **bypassed the Streamline proxy swapchain** (full native
swapchain). But the proxy swapchain **is** the component that runs DLSS-G on present.
Removing it from the present path is exactly why DLSS-G never armed. This project keeps
the proxy swapchain in the present path — the one thing we never did.

Because Streamline owns generation and pacing, **our code is small**: hooks + tagging +
config. No manual pacing, no worker threads, no queue-mutex management.

## Global constraints (verbatim, load-bearing)

- **Target:** BG3, Vulkan, NVIDIA RTX, Windows. 2560×1440 reference; must derive display
  refresh at runtime.
- **DLSS-G recipe** (captured live from PureDark's running build via NGX tap, 2026-08-26):
  `MultiFrameCount = N-1` (x2→1, x3→2, x4→3), `MultiFrameIndex` cycles 1..N-1;
  `DepthInverted=1`; `ColorBuffersHDR=0`; `Enable.OFA=1`; **`MvecScaleX=MvecScaleY=-1.0`**;
  real per-subframe jitter (NOT 0); MVecs/Depth at **render res** (~1707×960), Backbuffer
  at display res, fmt 44; matrices `CameraViewToClip / ClipToCameraView / ClipToPrevClip /
  PrevClipToClip`; **no `DLSSG.HUDLess`** in his set (he feeds Backbuffer only — but we may
  add HUD-less as an improvement, see Open Questions).
- **Reflex:** Low Latency **with Boost**, no manual fps cap (his `mReflexMode=2`,
  `mReflexFPSCap=0`).
- **Streamline stack:** SL 2.12.0, `nvngx_dlssg` 10.0.0 (both stacks identical generation —
  the arming difference was integration, not binaries). Ship the same DLLs PureDark's mod
  carries (`sl.interposer/common/dlss_g/pcl/reflex`, `nvngx_dlssg`).
- **MCM coexistence:** present the same proxy-swapchain interface PureDark's upscaler does,
  so the existing BG3SE compat SE overlay (`upscaler-run` branch) renders unchanged. Do NOT
  build our own ImGui.

## Architecture — four small units

### 1. Injection + Vulkan hook layer (`hooks.cpp`)
- DLL loaded by BG3SE's native plugin loader (same slot as `upscaler.dll`;
  `LoadLibraryW("upscaler.dll")` is how it loads today) — ship as that name or a name the
  loader picks up.
- MinHook (or Detours) on the minimum set: `vkCreateDevice`, `vkCreateSwapchainKHR`,
  `vkGetSwapchainImagesKHR`, `vkQueuePresentKHR`, `vkAcquireNextImageKHR`, and the NGX SR
  evaluate. Route swapchain create + present through `sl.interposer.dll`'s exports
  (`vkCreateSwapchainKHR`, `vkQueuePresentKHR`) instead of the native driver.
- Responsibility: put the SL proxy swapchain in the present path. Nothing else.

### 2. Streamline lifecycle (`streamline.cpp`)
- `slInit` with features { DLSS-G, Reflex, PCL } at process/plugin load.
- On `vkCreateDevice`: `slSetVulkanInfo` (instance/device/physical-device/queues).
- Proxy swapchain: created via SL's `vkCreateSwapchainKHR` (interposer export).
- Enable: `slDLSSGSetOptions({ mode=on, numFramesToGenerate=N })`,
  `slReflexSetOptions({ mode=LowLatencyWithBoost })`.
- Teardown symmetric.

### 3. Input snoop + tag (`dlssg_inputs.cpp`)
- Hook `NVSDK_NGX_VULKAN_EvaluateFeature`; for the DLSS-**SR** feature (id 1), read
  depth / motion-vectors / color out of `InParameters` via the NGX getters (proven working
  by the tap).
- Tag them for DLSS-G via `slSetTag` (kinds: depth, mvecs, hud-less/color, backbuffer) and
  file camera + jitter + mvec-scale via `slSetConstants`, using the captured recipe.
- Per frame: `slReflexSleep` + PCL markers (`slPCLSetMarker` sim/render/present) carrying
  the frame token — this is the pacing/timing signal DLSS-G keys on.
- The proxy swapchain's `present` then runs DLSS-G internally (generate N, pace, display).

### 4. Config + control (`config.cpp`)
- INI (PureDark's field names for familiarity): `mDLSSGMode`, `mDLSSGFrames` (x2/x3/x4),
  `mReflexMode`, `mDLSSPreset`. Hotkey toggle (cycle frames / on-off).
- No ImGui. MCM is BG3SE's.

## Data flow

```
game frame render
  └─ DLSS-SR EvaluateFeature (id 1)   ── snoop depth/mvec/color ──┐
game submits + vkQueuePresentKHR                                   │
  └─ routed to SL interposer present                               │
       └─ slSetTag(depth,mvec,hudless,backbuffer) + slSetConstants(cam,jitter,mvecscale=-1)
       └─ PCL markers / Reflex sleep
       └─ SL DLSS-G proxy swapchain: generate N frames, pace, present
             └─ BG3SE compat SE overlay draws MCM on the presented images
```

## Milestones (each independently testable; M1 is a throwaway spike)

**M1 — Arming spike (throwaway; the whole bet).** Minimal DLL: MinHook `vkCreateSwapchainKHR`
+ `vkCreateDevice`, `slInit`, `slSetVulkanInfo`, create proxy swapchain via SL,
`slDLSSGSetOptions(on, 1)`. Success = the SL log reports DLSS-G entering the *active/armed*
state (the transition our old branch never got). No image quality expected. **If it does not
arm, stop and debug arming before building M2+** — cheap, before investment.

**M2 — Inputs + first generated frames.** Add the SR-evaluate snoop, `slSetTag` +
`slSetConstants` with the captured recipe, PCL markers. Success = the DLSS-G watermark shows
and displayed fps ≈ (N)×real; image roughly correct.

**M3 — Recipe correctness.** MvecScale −1, per-subframe jitter, correct mvec/depth res,
matrices. Success = motion clean (no ghosting) vs PureDark.

**M4 — Config + multiplier + Reflex Boost.** INI, 2x/3x/4x, hotkey, Reflex LowLatency+Boost,
no cap. Success = matches his fps/1%-low behavior.

**M5 — MCM coexistence verified.** Run with the compat SE; confirm the MCM renders on our
proxy swapchain unchanged. Success = MCM works + FG works simultaneously (the thing his
upscaler cannot do).

## Testing / verification

- **Arming:** grep the SL log (`sl.interposer` writes one) for the DLSS-G active-state
  transition. This is the objective M1 gate.
- **fps / 1% lows:** in-game overlay counter (real fps) × N ≈ displayed; compare 1% lows to
  PureDark in the same scene. Optional: keep the present-stream profiler idea (from the BG3SE
  tap work) if a userspace present hook remains for measurement.
- **GPU utilization:** the tell — target ~90% like his (vs 50% direct-NGX).
- **Quality:** side-by-side vs PureDark on fire/smoke/hover and dark scenes.
- **MCM:** open the MCM in-game with FG active; it must render and be interactive.

## Build

- C++17/20, CMake, MSVC x64. Deps: Vulkan SDK headers, Streamline SDK (headers + the
  redistributable DLLs we already have from the BG3SE mod), MinHook.
- Output: one DLL placed in the game `bin` (or `bin/mods`) where the native loader finds it,
  plus the SL/NGX DLLs alongside.

## Open questions (resolve during M2/M3, not blockers for M1)

1. **HUD-less:** PureDark feeds Backbuffer only (UI included → UI interpolates). Our earlier
   direct-NGX used a pre-UI HUD-less copy to fix hover ghosting. Decide in M3 whether to add
   HUD-less tagging as an improvement over his.
2. **Input source:** snoop the SR evaluate (chosen) vs. hook the game's own resource tags.
   Snoop is proven; revisit only if a resource is missing/wrong-res.
3. **Loader name/slot:** confirm exactly how BG3SE's native loader picks the DLL (name vs
   manifest) so ours loads without renaming to `upscaler.dll` (avoid clashing with his).

## Non-goals (YAGNI)

- No upscaling (game's DLSS-SR does it; we only snoop its inputs).
- No ImGui / own menu (MCM is BG3SE's).
- No manual present pacing, no worker threads, no queue-mutex management (Streamline does it).
- No DX12 path (BG3 is Vulkan).
