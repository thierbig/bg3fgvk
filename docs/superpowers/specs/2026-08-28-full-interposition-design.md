# fgvk — Full Transparent Interposition (M2 rework)

**Status:** approved design (2026-08-28)
**Supersedes:** the hybrid hook layer in `src/vkhooks.cpp` only. All other units
(slboot, inputs pipeline, recipe, logging) carry forward unchanged.

## Problem (evidence-complete)

fgvk reached: DLSS-G armed (`status=0`), inputs snooped at PureDark's exact formats,
all four required tags live, FG feature created (4.9 GB VRAM), pacer thread running.
Generation then stalls: `getHostQueueInfo: Invalid VK queue - not created by the
application` → pacer semaphore/fence timeouts → present thread starves.

Root cause: **mixed ownership.** Streamline only trusts Vulkan objects it observed
being created. Every mixing strategy fails a different way (all reproduced tonight):

| Strategy | Result |
|---|---|
| Native device + slSetVulkanInfo (family/index) | Boots+arms, but pacer rejects the present-queue HANDLE (SL never observed it) |
| Native device + interposer vkGetDeviceQueue | Hard crash (exec-at-null; interposer can't serve a foreign device) |
| Interposer-owned device creation (4 variants) | Silent crash in initializePlugins (exec-at-null; same wall BG3SE documented) |

PureDark's mod achieves interposer-owned creation ON THIS MACHINE — the wall is a
difference in integration, not a fundamental limit.

## Design

**Phase 0 — Mine the working reference (throwaway diagnostics).**
Run PureDark's mod once with `sl.interposer.json` (verbose logging override) next to
his interposer. His sl.log answers, definitively: who creates the device (interposer
in-create init succeeding, and under which flags/appId); how the present queue gets
registered (his `getHostQueueInfo` path); whether the game's dispatch is built on the
interposer (GIPA redirection) or via export patching; NvLowLatencyVk / OTA / plugin
paths. Environment must be HIS exact original (his 2.10.3 stack, no fgvk DLLs in bin,
compat SE's own Streamline disabled).

**Phase 1 — Redirection hook layer (the rework).**
Replace in-place Detours on loader exports / driver pointers with
**vkGetInstanceProcAddr / vkGetDeviceProcAddr redirection**: hook the game's GIPA/GDPA
resolution so every Vulkan function it fetches resolves to `sl.interposer.dll`'s
export when the interposer provides one (else the native pointer). The game builds its
entire dispatch on the interposer → SL uniformly owns instance → device → queues →
swapchain → present. Consequences:
- No re-entry guards, no stack-walk filtering, no native/interposer split — the whole
  class of tonight's bugs is structurally impossible (the interposer calls the real
  loader chain itself, unpatched).
- Load-order risk: redirection must land before the game's first Vulkan resolution.
  Phase 0 reveals how PureDark wins this race; mirror it (worst case: detour GIPA
  itself in vulkan-1.dll — one export, resolved before instance creation).
- The NGX snoop (`inputs.cpp`) is untouched; slboot drops slSetVulkanInfo and the
  device surgery if Phase 0 shows the interposer needs neither.

**Phase 2 — Reconcile and bring-up.**
Diff our boot against the Phase-0 reference line-by-line (appId, flags, plugin paths,
device path, queue registration). Then walk the proven ladder: arm → inputs → tags →
pacer accepts queue → frames (watermark + ~4× fps).

## Constraints carried forward
- Recipe (captured): x4, DepthInverted=1, Reflex LowLatency+Boost no cap,
  MvecScale −1, real jitter, mvec/depth at render res, matched SL stack, OTA off
  (unless Phase 0 shows his differs — Phase 0 wins every conflict).
- One variable per build; SL verbose log + watchdog stay on in every run.
- MCM/BG3SE coexistence, config/hotkeys: deferred to their own spec after frames.

## Success criteria
Phase 0: his sl.log captured, the queue-registration and device-creation questions
answered with line citations. Phase 1/2: fgvk run shows no `Invalid VK app queue`,
pacer paces, DLSS-G watermark visible, displayed fps ≈ 4× real.
