# x4 Streamline 2.14 transition-pin pacing workaround

This document describes the opt-in `MFG4xTransitionPin` workaround. It is deliberately narrow:
it changes one verified two-byte instruction in known Streamline 2.14 `sl.dlss_g` builds while
x4 is active, and restores the stock bytes for x2, x3 and frame-generation off.

## What was observed

On the tested RTX 5080 Vulkan setup, x4 can enter a burst-shaped presentation route after
Streamline's present-metering feedback reaches its internal threshold. With Neural Rendering on,
that route produced a repeating long display hole: approximately `1.2 / 0.17 / 2.0 / 9.5 ms`
across the four output phases. Keeping the pre-transition route produced approximately even
Present-call pacing around `3.4 / 3.4 / 3.4 / 3.4 ms` and removed the severe long tail.

The workaround is a pacing fix, not a throughput optimization. The direct route can be somewhat
faster in source-frame throughput, especially in lighter workloads. The purpose of the pin is to
avoid the x4 batch-boundary starvation pattern.

## Verified Streamline 2.14 state transition

The active OTA build used for the original investigation was:

- `sl.dlss_g` 2.14.0-rc2 / build `614ea534a`
- SHA-256 `73b8a78a275b5a3db58038a4ff701a1fb4049db5c379eab9473a09fa856ba84e`
- PE timestamp `0x6A8DB8C8`
- `SizeOfImage = 0x9C000`

The official 2.14.1-rc0 build has the same relevant `presentCommon` implementation:

- SHA-256 `f4a6b2b14dcc0b1485989e430d3b4e3a44ac1800b92ba1ad74f476e64fb2b09c`
- PE timestamp `0x6A9AFD6B`
- `SizeOfImage = 0x9C000`

For these two builds, the complete `presentCommon` range at RVA `0x46840..0x4840A` is byte-for-byte
identical (7,114 bytes, SHA-256 `db6fd2da2605dde8381da06361144eee7291a8ec715b73a6d0e974a4e34cbf26`).

At RVA `0x46E30` both contain:

```asm
; BL initially derives from the flip-metering setting at ctx+0x4520
; EDI is the feedback counter at ctx+0x4088

0x46E27  test bl, bl
0x46E29  jne  common_target
0x46E2B  cmp  edi, 30
0x46E2E  jb   common_target
0x46E30  mov  bl, 1
```

The workaround changes only:

```text
B3 01    mov bl, 1
```

to:

```text
90 90    nop / nop
```

The branch reaches this instruction only after confirming `BL == 0`, so neutralizing the write
leaves the derived state at zero.

The full validated local stock shape beginning at RVA `0x46E27` is:

```text
84 DB 75 B6 83 FF 1E 72 B1 B3 01 EB AF 40 84 F6 74 0C
41 83 BE 88 40 00 00 01 41 0F 43 C4
```

## Why the state matters

The derived value is compared with a stored state byte at approximately `ctx+0x4081`. A change
calls a broad `flushAll` path and publishes the new state. The same state is then consulted by the
output path:

- around RVA `0x3CC61`, state 1 selects direct `presentFrame` calls near `0x3CD61` / `0x3CDF1`;
- state 0 constructs the asynchronous queued/paced output payload;
- state-dependent command-context selection also occurs around `0x3DDCA`, `0x48420`, `0x48E38`
  and `0x3EB37`.

These are useful locations for independently checking the mechanism in IDA/Ghidra.

## This is not ForceFlipMeteringOff

The important distinction is that this patch does **not** write the separate flip-metering control
field at `ctx+0x4520`.

`ForceFlipMeteringOff=1` takes the software/RSYNC-style route by forcing that field. The transition
pin instead leaves the normal flip-metering-capable field clear and prevents only the later
feedback-triggered derived-state transition. That is why this option is named a transition pin,
not a flip-metering disable.

## Runtime safety / build gating

The implementation intentionally fails closed:

1. `slDLSSGSetOptions`' owning module is checked first.
2. If that pointer belongs to a wrapper, loaded modules are enumerated.
3. Only exact known PE timestamp + `SizeOfImage` candidates are hashed.
4. The full file SHA-256 must match a closed known-build table.
5. The local instruction prefix/suffix and current target bytes must match either stock `B3 01`
   or already-pinned `90 90`.
6. The target is 16-bit aligned and changed with a single `InterlockedExchange16` while its page
   is temporarily writable, followed by `FlushInstructionCache` and protection restoration.
7. Unknown, ambiguous or locally modified builds are never patched.

No NVIDIA DLL is changed on disk.

## Why the bundled Streamline 2.12 build is not patched here

The repository's bundled 2.12.0 build (SHA-256
`1fec3f8fdfc59d78c4445c276c1a0fb798bf251985f348597dc2b44d0c995e52`, PE timestamp
`0x6A39AB23`, `SizeOfImage=0x97000`) has the analogous transition at RVA `0x45EF6`, but its
verified instruction is three bytes (`0F 43 F9`, `cmovae edi, ecx`). The original experiment
neutralized those three bytes successfully, but it does not provide the same naturally aligned
16-bit atomic update available in 2.14. This PR therefore leaves 2.12 unchanged rather than widen
the runtime-code-write surface. A separate 2.12 implementation can be considered if the maintainer
wants to support that path explicitly.

## Configuration

```ini
[fgvk]
MFG4xTransitionPin=1
```

When enabled:

- x4 (`numFramesToGenerate == 3`) -> `B3 01` becomes `90 90` on a verified build;
- x2 / x3 / FG off -> `B3 01` is restored;
- unsupported build -> log once and leave Streamline untouched.

The setting defaults to `0` for an initial upstream review/testing period.
