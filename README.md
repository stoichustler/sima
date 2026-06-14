# CLAN HYPERVISOR

```text
 ______   __       ______   __   __     ______   ______
/\  ___\ /\ \     /\  __ \ /\ "-.\ \   /\  __ \ /\  ___\
\ \ \____\ \ \____\ \  __ \\ \ \-.  \  \ \ \/\ \\ \___  \
 \ \_____\\ \_____\\ \_\ \_\\ \_\\"\_\  \ \_____\\/\_____\
  \/_____/ \/_____/ \/_/\/_/ \/_/ \/_/   \/_____/ \/_____/
```

CLAN is a compact ARM64 hypervisor bring-up project for QEMU `virt`. It runs at
EL2 and focuses on a small, readable virtualization base for RTOS guests.

```text
┌──────────────────────────────┐
│ CLAN Hypervisor · ARM64 EL2  │
├──────────────┬───────────────┤
│ VM0 Zephyr   │ service VM    │
│ VM1 LK       │ prelaunch VM  │
│ VM2 clot.os  │ prelaunch VM  │
└──────────────┴───────────────┘
```

## Completed

| Area | Feature |
| --- | --- |
| Platform | ARM64 QEMU `virt`, GICv3, 8 pCPUs |
| Boot | Static raw-image guests with `kick.py` toolchain selection |
| Guests | Zephyr, LK, and clot.os boot as 3 concurrent OS instances |
| vCPU | EL1 guest entry/exit, shared-pCPU scheduling, PSCI CPU_ON |
| Memory | Static guest RAM windows, stage-2 identity mapping, zero-page guard |
| GICv3 | vGICv3 distributor, per-vCPU redistributor, list registers |
| Interrupts | SGI/PPI/SPI routing, timer injection, vGIC maintenance handling |
| Console | vPL011, async VM console buffer, `vsh` guest shell switching |
| Debug | `vcpus`, `threads`, `schedstat`, `vmap`, `irqs`, `dumpstat`, `vsh` |
| Tests | `scripts/regress.py` validates CLAN shell and all guest consoles |
| Docs | SDK notes, ARM64 assembly comments, concise architecture references |

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
| Loader | Replace `.incbin` raw-image flow with module/image loader options |
| vGICv3 | Improve priority, pending/active state, MSI/LPI, ITS readiness |
| Redistributor | Harden multi-vCPU GICR state save/restore and migration paths |
| Memory | Add richer stage-2 attributes, dynamic regions, and fault reporting |
| Devices | Generalize MMIO emulation beyond vPL011 and virtual GIC devices |
| Scheduling | Refine shared-pCPU policy, accounting, and latency visibility |
| Regression | Add longer SMP, interrupt, console overflow, and reboot tests |
| Portability | Derive more platform data from FDT instead of static QEMU tables |
| Documentation | Keep docs aligned with implemented behavior and test coverage |

---

Hustle Embedded OS.
