# AD103 (10de:2705) experimental Windows target

**Not a supported target.** One machine, one boot, one card. Read
[compatibility](/docs/COMPATIBILITY.en.md) before assuming anything transfers.

This directory was copied from `windows/ad104-msi` and edited against what an
AD103 card on an ASUS B550-I actually reports. It is kept here as a branch, not
as a pull request: several of the edits are *relaxations* of checks that
`ad104-msi` deliberately enforces, and opening a PR would amount to asking the
maintainer to revisit a policy the README already states ("Do not remove checks
or substitute a PROM hash to force another machine through"). Anyone who wants
to build on this should read the table below and decide per check.

## Verified configuration

| | |
|---|---|
| GPU | AD103, `10de:2705`, 16376 MiB, VBIOS `95.03.45.40.c5` |
| Subsystem | `1771:10de` |
| Board | ASUS ROG STRIX B550-I GAMING, BIOS 4304 |
| CPU | Ryzen 7 5700X3D — **no iGPU** |
| PCI | `0000:08:00.0`, BAR0 `0xFB000000` (16 MiB), BAR1 16 GiB (Re-BAR on) |
| Driver | 621.01 |
| OS | Windows 11, Secure Boot off |

Transaction log from the one successful run:

```
selector write: SS0=0x88888888  SS1=0x8
selector result: SS0=0x88888888 matched=1   SS1=0x8 matched=1
AD102 selectors-applied: readout1=0  full=1
post-FLR: SS0=88888888 SS1=00000008 WPR2=1ffffe00:00000000
Transaction result=0 safe=1 retained=0 started=1 quiesced=1 flrs=1 DMA_live=0
```

Measured (register-resident issue rate, 66 SM, 20000 iter x 3):

| mode | stock | unlocked | ratio |
|---|---:|---:|---:|
| f16/f32 | 88.0 | 176.6 | 2.01x |
| bf16/f32 | 87.9 | 176.2 | 2.00x |
| e4m3/f32 | 170.63 | 325.98 | 1.91x |
| f16/f16 | 175.6 | 176.0 | control |
| s8/s32 | 352.5 | 350.7 | control |

`validate_gemm`: 3 shapes x 2 modes, failures=0, max_abs=0.

## Every file that differs from ad104-msi

| file | lines | what changed |
|---|---|---|
| `port.h` | 116 → 116 | board pins: `GU_BOARD_ID 0x270510de`, `GU_BOARD_SUBSYSTEM 0x10de1771`, `GU_BAR0_BASE 0xfb000000`, VRAM `0x3ff8` |
| `port.c` | 342 → 342 | bus `1` → `8` |
| `checks.c` | 199 → 201 | `GuDisplayPath` route 2 root port `dev=1` → `dev=3` |
| `profile.c` | 82 → 156 | cold-profile diagnostics; bus/board gate constants |
| `boot_io.c` | 184 → 239 | read the boot manager by its actual size instead of a pinned length; report hash instead of refusing |
| `app.c` | 88 → 101 | **handoff to Windows whenever the GPU is quiescent** (see R1) |
| `msi_release.c` | 524 → 644 | display-child grouping, cascade removal, disconnect target, `0x193000a1`, residual counter (see R1–R6) |

`format.c` is unchanged. The `%p` problem was fixed at the call sites — this
layer only supports `%s %u %x %X`, and a `%p` makes `GuFormatV` return -1, which
sets `Gu.fault` and silently drops every later log line.

## The relaxations, stated plainly

Each of these makes a check that `ad104-msi` enforces into something that only
records. They were needed to get through this board, not because the checks were
wrong.

**R1 — failures no longer halt the boot.** `app.c` used to `GuBootHalt()` on any
refused gate whenever `Plan.mutated` was set, which is from stage 1 onward. That
cost a power cycle per attempt. Now only `Gu.started || Gu.retained ||
!GuSafeToReturn()` halts; everything else logs and starts Windows.

**R2 — `GuMsiReleased()` no longer requires graph equality.** It is
`Gu.display_check`, i.e. the entry ticket to the firmware transaction, and it
used to demand `Expected(3,TRUE)`, `ConfigStable` and exact PCI attributes.
Now it requires ownership (`Plan.claimed && Gu.owned`) and read-only probes that
cannot fail benignly; graph drift and attribute drift are logged as tolerated.

**R3 — `ClaimRestore()` tolerates attribute and decode-mask mismatch.** Same
reason: these are assertions about firmware behaviour, and this firmware differs.

**R4 — `Consumers()` allows `text`/`virtual` to be 0** and lowers the child
count to `>=1`. See "why" below.

**R5 — `Driver()` keeps the driver identity pins as diagnostics.** The three
drivers here are 30912 / 22080 / 162496 bytes vs the pinned 24576 / 14816 /
162016. Structural checks (`ImageBase`, `EfiBootServicesCode`) still refuse.

**R6 — `PointerBaseline()` / `UsbBaseline()` do not assert the USB branch.**
`UsbBaseline` contained `if(Plan.usb_host) return EFI_UNSUPPORTED;` — it cannot
pass with two xHCI controllers, and this board has two.

## Why the display-child handling had to change

This board exposes two display children under one GPU root port:

```
ACPI根 -> PCI(fn=1,dev=3) -> PCI(fn=0,dev=0) -> ACPI ADR 0x4003 -> END
ACPI根 -> PCI(fn=1,dev=3) -> PCI(fn=0,dev=0) -> ACPI ADR 0x7003 -> END
```

`LocateHandleBuffer(GopGuid)` returns one per boot while the TextOut/GOP owner
edges can sit on the other. A single pinned `GopPath` therefore fails half the
time. Replaced with a prefix-plus-structure matcher (`GpuChild`) used by every
ownership check, `Removed()`, and the `DisconnectController` target.

That is also why R4 exists: on this firmware the ConSplitter opens TextOut on the
GPU child *and* a BY_CHILD virtual console, but which child carries which edge
varies. `text=1 virtual=1` on one boot, both 0 on another.

Note this is **not** the aggregate-console topology from
[impacifist/Windows-GPU-UNLOCK](https://github.com/impacifist/Windows-GPU-UNLOCK)
— that one requires a pathless, zero-framebuffer GOP after release, and this
GOP has `fb=0x7800000000`, `fbsz=0x1900000`.

## Opt-in VGA restore (experimental)

`-VgaRestore` builds an experimental variant that restores the VGA/ISA
attribute mask `0x18` and verifies the parent bridge VGA-enable bit after FLR,
before the exclusive claim is released. It is off by default. The evidence
from the separate Windows audit harness, the exact pre/post register values,
the claim-policy change and the limitations are in
[`docs/windows-ad103-vga-restore.md`](../../docs/windows-ad103-vga-restore.md).
The default build is unchanged; this variant has not been hardware-validated
in this app.

## Known gaps

- **Three-stage release.** `ad104-msi` also has three stages, but
  `impacifist/Windows-GPU-UNLOCK` states its non-aggregate path never stops the
  NVIDIA owner. Here stage 3 does drop the PciIo owner and `ClaimRestore()`
  re-takes it. Verified once; not verified across boots.
- **`Expected()` never proves removed edges are gone.** It skips anything
  `Removed()` marks. A residual counter was added to `GuMsiReleased` but it only
  logs; tightening it to an assertion on an unvalidated release path would make
  that path more brittle.
- **Console protocol copies are not compared byte-wise.** `Alive()` compares
  pointers only; `headless-policy.h` in the impacifist tree compares full struct
  copies.
- **Observations outside TPL.** Only `Snapshot()` is inside `TPL_NOTIFY` here.
- **Volatile.** A power cut clears the override: `SS1` returns to `0x1` and
  `readout1` back to `full=0`. `SS0` itself comes back as a *different* value on
  every cold boot — `0x6310036`, `0x7300036`, `0x5305036`, `0x7300036` across
  RUN0001/0002/0004/0005 — so never compare it by equality. The one read-only
  probe sample of `0x27304036` is a single observation, not the stock constant.
  Nothing in this port changes that.

## Building

Same as `ad104-msi`: `Build-Experimental.ps1`. `local/boot_profile.h` is not
committed — it carries the ESP partition identifier and is generated per machine.