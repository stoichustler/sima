# SIMA HYPERVISOR

```text
 ____  ___ __  __    _
/ ___||_ _|  \/  |  / \
\___ \ | || |\/| | / _ \
 ___) || || |  | |/ ___ \
|____/|___|_|  |_/_/   \_\
```

SIMA is a compact ARM64 hypervisor bring-up project for QEMU `virt` and rk356x
hardware. It runs at EL2 and focuses on a small, readable virtualization base
for mixed RTOS and Linux guests.

```text
┌──────────────────────────────┐
│ SIMA Hypervisor · ARM64 EL2  │
├──────────────┬───────────────┤
│ VM0 Zephyr   │ service VM    │
│ VM1 LK       │ prelaunch VM  │
│ VM2 Linux    │ prelaunch VM  │
└──────────────┴───────────────┘
```

## Completed

| Area | Feature |
| --- | --- |
| Platform | ARM64 QEMU `virt`; rk356x hardware-platform skeleton |
| Boot | Static raw-image guests with `kick.py` toolchain and Linux image loader selection |
| Guests | Zephyr, LK, and Linux boot as 3 concurrent OS instances |
| vCPU | EL1 guest entry/exit, shared-pCPU scheduling, PSCI CPU_ON |
| Memory | Static guest RAM windows, stage-2 identity mapping, zero-page guard |
| GICv3 | vGICv3 distributor, per-vCPU redistributor, list registers |
| Interrupts | SGI/PPI/SPI routing, timer injection, vGIC maintenance handling |
| Console | vPL011, async VM console buffer, `vsh` guest shell switching |
| Debug | `vcpus`, `threads`, `schedstat`, `vmap`, `irqs`, `dumpstat`, `vsh` |
| Tests | `scripts/regress.py` validates SIMA shell and all guest consoles |
| Docs | SDK notes, ARM64 assembly comments, concise architecture references |

LK and Zephyr stay as repository-local `.incbin` guest images under
`sdk/images`. VM2 Linux uses `sdk/images/linux/Image` and `sdk/images/linux/Initrd`, which
QEMU stages with `-device loader`; SIMA then copies them into the VM2 RAM window.
The Linux device tree remains repository-local as `sdk/images/linux/sima-linux.dtb`.

```text
┌────────────┬────────────┬────────────┬────────────┐
│ CPU/vCPU   │ Stage-2    │ vGICv3     │ vPL011     │
├────────────┼────────────┼────────────┼────────────┤
│ EL1 guest  │ RAM guard  │ GICD/GICR  │ vsh switch │
│ PSCI boot  │ 1:1 map    │ LRs/timer  │ ring buf   │
└────────────┴────────────┴────────────┴────────────┘
```

## To Optimize

| Area | Work |
| --- | --- |
| VM config | Make VM2/VM3 additions more data-driven and less board-specific |
| Loader | Generalize the Linux module/image loader path beyond QEMU staging |
| vGICv3 | Improve priority, pending/active state, MSI/LPI, ITS readiness |
| Redistributor | Harden multi-vCPU GICR state save/restore and migration paths |
| Memory | Add richer stage-2 attributes, dynamic regions, and fault reporting |
| Devices | Generalize MMIO emulation beyond vPL011 and virtual GIC devices |
| Scheduling | Refine shared-pCPU policy, accounting, and latency visibility |
| Regression | Add longer SMP Linux, interrupt, console overflow, and reboot tests |
| Portability | Derive more platform data from FDT instead of static QEMU tables |
| Documentation | Keep docs aligned with implemented behavior and test coverage |

## Source Base And License

SIMA is based on the Project ACRN hypervisor source tree and keeps ACRN-derived
code under the BSD 3-Clause License.

The ARM64 QEMU porting work is non-Intel work and is marked as
`Copyright (C) 2026 Hustler Lo.` under the same BSD 3-Clause License. Third
party components retain their original notices; see `NOTICE.md` and `LICENSES/`.

License-risk summary: no GPL-only source was found. The notable item is libfdt,
which is dual-licensed `GPL-2.0-or-later OR BSD-2-Clause`; use the BSD-2-Clause
option when distributing SIMA to avoid GPL copyleft obligations.

---

Hustle Embedded OS.
