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
Task 1: complete (commits d2c9505a..63e4e30, review clean — scaffold+logger+stubs)
Task 2: complete (commits 63e4e308..3202a14, review clean — vkCreateInstance+Device hooks)
Task 2: Ruling: gGraphicsQueue declared but unstored — not needed for M1 (slSetVulkanInfo uses family+index, not the VkQueue handle). Capture via vkGetDeviceQueue in M2 if tagging needs it. — Cost if wrong: one added vkGetDeviceQueue call in M2.
Task 4 (pre-dispatch): Ruling: refined the M1 arming gate from header facts. DLSSGStatus enum (eOk=0; fail flags 2=Reflex-not-detected, 8=common-constants-invalid, 16=backbuffer-index) means M1 SUCCESS = ANY "DLSSG status=" line logged (proves DLSS-G is in the present loop, which direct-NGX never achieved); a fail-flag status is the M2 to-do list, not M1 failure. Also added slReflexSetOptions(eLowLatency) to OnDeviceCreated (DLSS-G requires Reflex; addresses status=2). VulkanInfo confirmed in sl_helpers_vk.h. — Cost if wrong: if arming truly needs eOk, we still learn the exact missing prereq from the status flag, so no wasted work.
Task 3: complete (commits 3202a14b..d4979f9, review clean — swapchain+present routed to SL proxy; slboot.h include verified present)
Task 4: implemented (commit 7855dda9) DONE_WITH_CONCERNS — SL core funcs (slInit/slSetVulkanInfo/slGetFeatureFunction) are extern SL_API with no import lib -> LNK2019.
Task 4: fix round 1/5 dispatched (resume implementer) — convert to dynamic GetProcAddress resolution from sl.interposer.dll + slGetFeatureFunction for feature funcs (BG3SE-proven pattern). FIX_BASE 7855dda9.
Task 4: fix round 1/5 (1 addressed, 0 open — SL dynamic resolution; commits 7855dda9..adb4319; re-review clean)
Task 4: minor (deferred): g_sl load-with-fallback duplicated 3x — cosmetic, factor into a helper at cleanup.
Task 4: complete (commits d4979f92..adb4319, review clean after 1 fix round — slInit+Reflex+DLSS-G enable+status poll, all SL calls dynamic)
Task 5: complete (commit adb43190..fae33fc, controller-reviewed — docs/M1-result.md + BUILD.md correct)

## Final whole-branch review (M1): SHIP with 3 Important diagnosability findings -> one fix wave
- F1 vkhooks InstallVkHooks/RemoveVkHooks: Detours return codes discarded; "hooked" logged unconditionally.
- F2 vkhooks InstallVkHooks: no null-check on vulkan-1.dll handle before GetProcAddress.
- F3 slboot OnDeviceCreated: slSetVulkanInfo result not gated (proceeds even on failure).
Fix-wave FIX_BASE fae33fc.
Final fix wave: re-review clean (F1/F2/F3 all ADDRESSED, no new breakage; commits fae33fc0..8d571e3)
M1 COMPLETE — 5 tasks, all reviewed; 1 build-blocker (SL linkage) + 3 diagnosability findings caught and fixed.
