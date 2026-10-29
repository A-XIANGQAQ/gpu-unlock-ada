# Windows AD103 handoff: restore VGA 0x18 after FLR (experimental)

This is an opt-in, board-specific repair for the Windows Code 12 failure seen
after a complete AD103 transaction. It is not enabled in the default build and
has not been re-run on hardware in this app. The hardware evidence below comes
from a separate Windows audit harness that used the same stop/claim/FLR/VGA
sequence.

## Finding

On the ASUS B550-F / RTX 4070 Ti SUPER (AD103, `10de:2705`) test system, the
display-driver Stop clears more than the previous handoff code assumed:

| state | GPU attributes | GPU command | parent attributes | parent bridge control |
|---|---|---:|---|---:|
| before owner Stop | `0x718` (BME/IO/MEM/VGA `0x18`) | `0x7` | `0x718` | `0x18` |
| after owner Stop | `0x0` | `0x0` | `0x700` | `0x10` |
| after IO/MEM claim | `0x300` | `0x3` | `0x700` | `0x10` |
| after VGA restore | `0x318` | `0x3` | `0x718` | `0x18` |

The Stop therefore clears the GPU's VGA bits and the parent bridge's
VGA-enable bit 3. A control that stopped the three display consumers and the
PCI owner, took the exclusive claim, restored only IO/MEM, and handed off
Windows reproduced NVIDIA Code 12 / `0xc0000018` with no allocated GPU
configuration and both physical displays black (HND0021). The normal Windows
allocation contains legacy VGA I/O `0x3b0`-`0x3bb`, `0x3c0`-`0x3df` and
memory `0xa0000`-`0xbffff`; the failed boot has none.

Restoring only the observed `0x18` mask (and verifying the parent bridge
`0x718` / control `0x18` with BME off) fixed the control: Windows started
directly, NVIDIA PnP was OK/problem 0, both 1920x1080 outputs were active, and
the legacy VGA allocations returned (HND0022).

The full transaction with the same post-FLR restore (HND0023) completed FWSEC,
SEC2/payload, selectors, FLR and post-FLR GFW; the restore read back GPU
`0x318`/command `0x3` and parent `0x718`/control `0x18`. Windows started
directly with both displays. Three same-boot numerical runs had zero failures;
f16/f32 and bf16/f32 measured 2.067x/2.068x over the stock baseline, while
f16/f16, s8/s32, dp4a and ffma stayed near 1.0x. This verifies the unlock and
handoff for that boot, not long-term stability, all workloads, repeatability
across cold boots, or universal safety of the firmware VGA mutation.

## Code

- `vga_restore.c` / `vga_restore.h`: finds the single PCI-to-PCI bridge whose
  secondary bus is the GPU's bus, reads the GPU and parent state, enables only
  the observed `0x18` mask, optionally completes the parent update, and
  requires exact readback (`0x318`/`0x3`; `0x718`/`0x7`/`0x18`) with BME off.
  It never writes config directly, enables BME, reconnects the driver/GOP,
  maps DMA or resets.
- `app.c`: when built with `-DGU_VGA_RESTORE`, calls it after the core
  transaction returns and before `GuMsiFinish` releases the exclusive claim.
  A failed restore goes to the existing `failed` path and does not start
  Windows.
- `msi_release.c`: the same flag changes the claim/handoff attribute policy
  from full `Plan.attributes` equality to the observed safe states: the Stop
  may clear BME/VGA, the claim must leave BME off with IO/MEM restored, and the
  final state must have IO/MEM on and BME off (VGA is restored separately).
  The default build is unchanged.
- `Build-Experimental.ps1 -VgaRestore`: opt-in build. The default build does
  not call the module.

## Test

```
cl /nologo /std:c11 /W4 /WX /O1 /I. /Icompat /Ivendor/edk2/Include \
  /Ivendor/edk2/Include/X64 /DGU_VGA_RESTORE tests/test_vga_restore.c
```

The host test covers the unique-parent scan, GPU-only and parent-fallback
paths, precondition failures, enable/readback failures, BME-off checks and
pre- versus post-FLR admission. It reports 17 checks and makes no firmware
calls.

## Limitations and reproduction

- The hardware evidence is from the audit's separate one-shot Windows harness,
  not this app. This patch compiles and passes its host test, but the exact
  app build in this repository has not been run on hardware. Treat it as an
  opt-in reference change, not a validated fix.
- The audit's board pins (bus 9, subsystem `0x89581043`, local flush/DMA
  components) are not part of this patch; this branch keeps its AD103 pins.
- One successful boot is not long-term, all-workload or repeatability proof.
  Selectors are volatile; a cold boot resets them. No automatic boot
  persistence or VBIOS modification is included.
- The audit reproduction used a manual USB one-shot: boot the USB, press U,
  then the same-boot Windows validator. Logs and the validator result are kept
  in the audit workspace and are not part of this repository.
