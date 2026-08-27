# M1 Arming Result

## How to read it

M1 SUCCESS = any `DLSSG status=` line appears in `fgvk.log`.

- **status=0** (eOk): DLSS-G fully armed. Prerequisites satisfied. Ready for M2.
- **status=2** (Reflex not detected), **status=8** (common constants invalid), **status=16** (backbuffer index): DLSS-G is in the loop. These are prerequisite gaps — the M2 to-do list. Arming succeeded; resolve these in M2 (inputs, tagging, viewport).
- **NO status line ever**: Arming failed. Debug before M2. Check:
  - Proxy swapchain actually used by the game
  - Reflex active
  - Valid viewport at slInit time
  - slInit result codes
  - Interposer loaded from the game's `mods/UpscalerBasePlugin/Streamline` or `bin` directory

---

## Paste your fgvk.log lines here

```
[Paste the complete fgvk.log output or relevant slInit / slSetVulkanInfo / slDLSSGSetOptions / "DLSSG status=" lines here]
```

---

## Verdict

**Verdict: [ARMED / ARMED-WITH-PREREQS / NOT ARMED]**

(Choose one and record the status= value if present.)

---

## Next

- **If ARMED or ARMED-WITH-PREREQS**: Expand M2 (inputs + tagging) into full task cards. Begin recording which constants/buffers map to the game's state and vertex layout.
- **If NOT ARMED**: Open arming debug sub-plan. Verify:
  - Proxy swapchain is actually used (check D3D11 hooks firing)
  - Reflex is enabled in NV Control Panel or game settings
  - Valid viewport when slInit is called
  - slInit and slDLSSGSetOptions return codes (0 = success)
  - Interposer DLL loaded from correct path (`mods/UpscalerBasePlugin/Streamline` or `bin`)
