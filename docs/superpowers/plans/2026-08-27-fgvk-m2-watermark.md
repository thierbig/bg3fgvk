# fgvk M2+ — Finish Plan (watermark → quality → config → MCM)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (inline). Each task ends in a build; launch-gates are batched to minimize user launches.

**Goal:** From "SL fully configured but present path not routed" (current state) to: DLSS-G watermark + N×fps, then real camera quality, config, and MCM coexistence.

**Spec:** `docs/2026-08-27-fgvk-design.md`. **Progress ledger:** M1 chain is GREEN in-game (slInit/slSetVulkanInfo/Reflex/DLSSGSetOptions all -> 0, stable). Root cause of "no watermark": BG3 resolves device-level Vulkan functions via `vkGetDeviceProcAddr` (driver pointers), bypassing our Detours on vulkan-1.dll exports → swapchain/present hooks never fire → proxy swapchain never in present path, no status poll, no inputs.

## Global Constraints (carried forward)
- Hybrid creation (PROVEN tonight): interposer-created INSTANCES + device surgery + NATIVE vkCreateDevice + slSetVulkanInfo. Do not revisit.
- Recipe: MvecScale -1, real per-subframe jitter (from the game's SR evaluate), DepthInverted=1, mvec/depth at render res, no HUDLess (match PureDark first).
- One launch-gate per risk boundary (user's debug protocol: one variable per build).
- BG3SE compat: fgvk loads via NativeMods loader (independent of SE); MCM validation is M5.

### Task 1: Route the REAL present path (device-proc-addr hooks)  [LAUNCH GATE A]
In `h_CreateDevice` post-create: resolve the driver's real pointers via `vkGetDeviceProcAddr(device, ...)` for `vkCreateSwapchainKHR`, `vkQueuePresentKHR`, `vkAcquireNextImageKHR`, `vkGetSwapchainImagesKHR`, `vkDestroySwapchainKHR`; Detour THOSE (new transaction; keep existing hooks). Loader-export detours stay (harmless). Re-point h_CreateSwapchainKHR/h_QueuePresentKHR originals at the driver pointers. Gate A (user launch): `vkCreateSwapchainKHR ... proxy=` line + `DLSSG status=` line appears. Status value read per docs/M1-result.md.

### Task 2: Frame loop (token + PCL + Reflex sleep)
Resolve `slGetNewFrameToken`, `slSetConstants`, `slSetTagForFrame`, `slPCLSetMarker` (kFeaturePCL), `slReflexSleep` (kFeatureReflex) via slGetFeatureFunction. Per present: new frame token; slReflexSleep; PCL markers eSimulationStart/End + eRenderSubmitStart/End + ePresentStart/End around the present call. Store current token for tagging.

### Task 3: NGX SR snoop (inputs)
Detour `NVSDK_NGX_VULKAN_EvaluateFeature` (export of _nvngx.dll/nvngx.dll — same tap point proven on upscaler-run). For SR (feature id 1): via NGX getter read Depth, MotionVectors, Color, width/height, `Jitter.Offset.X/Y`, MV scale. Stash VkImageView/VkImage + dims per frame (no copies — tag the live resources).

### Task 4: Tag + constants (identity reproj first)  [LAUNCH GATE B]
Per frame with valid snoop: `slSetTagForFrame(token, vp0, tags[])` — kBufferTypeDepth + kBufferTypeMotionVectors (render res, eValidUntilPresent). `slSetConstants`: jitter from snoop, mvecScale {-1,-1}, depthInverted, cameraViewToClip from SR-era proj if available else plausible fixed FOV(0.84)/aspect(1.7778)/near(0.05)/far(10000); clipToPrevClip = IDENTITY (proven bootstrap in FillFGCamera), reset=false, motionVectors3D=false, cameraMotionIncluded=true. Gate B (user launch): DLSS-G WATERMARK visible, displayed fps ≈ 2×real (x2). Ghosting acceptable at this gate.

### M3 group — real camera (quality)
Track consecutive frames: derive viewProj from game data. Preferred: shared-memory bridge from compat SE (FillFGCamera already computes everything; SE writes 4 matrices + camera vecs per frame to named shared mem; fgvk reads). Fallback: keep identity + jitter (assess quality vs PureDark first — his data may show identity is close at high base fps). Then ClipToPrevClip = prevVP * inverse(curVP) exactly as FillFGCamera does.

### M4 group — config + multiplier + Reflex boost
INI `fgvk.ini` (mDLSSGFrames 2/3/4 → numFramesToGenerate 1/2/3; mReflexMode → eLowLatencyWithBoost), hotkey cycle, live slDLSSGSetOptions update, DLSSGGetState numFramesToGenerateMax clamp.

### M5 group — BG3SE + MCM + packaging
Official BG3SE restored; compat-SE overlay renders on proxy swapchain with FG on; README + release zip (fgvk.dll + SL/NGX DLLs + ini).
