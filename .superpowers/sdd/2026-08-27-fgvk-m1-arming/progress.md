# SDD ledger — plan: /mnt/c/Dev/fgvk/docs/superpowers/plans/2026-08-27-fgvk-m1-arming.md

## Pre-flight conflict scan (M1 tasks 1-5)

Interface/shared-file rows:
- T1→T2..T4: T1 creates src/vkhooks.cpp/.h + src/slboot.cpp/.h as stubs; T2/T3/T4 fill them. Produces stub InstallVkHooks/RemoveVkHooks (T1) consumed+extended by T2. Consistent — T1 explicitly makes stubs so later tasks fill bodies. OK.
- T2 produces gDevice/gInstance/gPhysicalDevice/gGraphicsFamily/NativeCreateSwapchain(); T3 consumes NativeCreateSwapchain()+gDevice; T4 consumes gInstance/gPhysicalDevice/gDevice. NOTE: T2 does NOT capture gInstance (only device/phys/family); T4 references gInstance and its own text says "add a vkCreateInstance hook in this task". Ruling below.
- T2 & T3 both edit src/vkhooks.cpp (CreateDevice hook T2; CreateSwapchain hook T3) + attach in same InstallVkHooks. Sequential, same file, no conflict (T3 adds alongside). OK.
- T4 edits src/slboot.cpp (T3 also created SlProxyCreateSwapchain there). Sequential same file. OK.

Self-consistency rows:
- T1: files it creates (CMakeLists, log, dllmain, stubs) match what it builds. OK.
- T2: code references OnDeviceCreated() (defined in T4/slboot) — T2 calls it; must exist as a stub or be declared. Ruling below.
- T3: routes to SlProxyCreateSwapchain (T3 defines it) + falls back to o_CreateSwapchainKHR (T2 stores it). OK.
- T4: real SL API verified against headers (slInit/slSetVulkanInfo/DLSSGOptions{eOn,numFramesToGenerate}/slDLSSGGetState). OK.

## Rulings
Ruling: gInstance capture — T2 brief captures device/phys/family but T4 needs gInstance; T4's own text says add a vkCreateInstance hook. Decision: fold gInstance capture (hook vkCreateInstance, store gInstance) into T2 so all device-scope handles are captured in one place; T4 just consumes it. — Why: keeps handle-capture cohesive in vkhooks, avoids T4 touching hook install for an unrelated reason. — Cost if wrong: minor rework moving one hook between tasks.
Ruling: OnDeviceCreated declaration — T2 calls fgvk::OnDeviceCreated() but it's defined in slboot (T4). Decision: T1 stub slboot.h declares `void OnDeviceCreated();` and `void PollDLSSGState();` with empty bodies so T2 links; T4 fills the bodies. — Why: preserves per-task buildability (T2 must build before T4). — Cost if wrong: T2 fails to link → caught immediately at T2 build.
Ruling: build+launch verification — native Windows/Vulkan; subagents cannot build (no MSVC) or run BG3. Decision: implementers write files + self-check syntax by reading; the "build"/"in-game" steps are recorded as USER-RUN in the report, not executed by the subagent. Task reviews check code correctness against the plan, not runtime. — Why: the toolchain and game live on the user's Windows side. — Cost if wrong: a compile error slips to the user's build step instead of being caught by a subagent (acceptable; user builds each task anyway).

## Tasks
