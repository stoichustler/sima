# CLAN ARM64 QEMU Reference

## Repository Map

- `Makefile`: static ARM64 QEMU config path when `ARCH=arm64 PLATFORM=qemu`.
- `scripts/kick.sh`: QEMU launcher for `build/acrn.out`.
- `arch/arm64/platform/qemu`: QEMU board/scenario, static VM config, embedded RTOS images, platform helpers.
- `arch/arm64/guest`: stage-2 VM setup, vCPU entry/exit, vGICv3, vPL011, hypercall handling.
- `core`: common VM, vCPU, scheduler, timer, hypercall, and MMIO request logic.
- `sdk/boot`: boot module discovery and kernel loaders.
- `sdk/debug`: CLAN shell, console, debug commands, vsh, vdump, irqs.
- `sdk/sdk.md`: current implementation status and verified behavior.

## Build And Run

Build:

```bash
PATH=/home/beau/clan-arm64-none-elf/bin:$PATH \
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
```

When editing `sdk/debug/*`, stale archive dependencies may hide source changes.
Force a debug rebuild before validation:

```bash
rm -f build/modules/libdebug.a build/acrn.out build/acrn.bin
PATH=/home/beau/clan-arm64-none-elf/bin:$PATH \
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
```

Run:

```bash
./scripts/kick.sh
```

Dry run:

```bash
./scripts/kick.sh --dry-run
```

Default QEMU shape:

```bash
qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3 \
  -cpu cortex-a57 \
  -smp 8 \
  -m 1024M \
  -nographic \
  -serial mon:stdio \
  -kernel build/acrn.out
```

## Current VM Layout

- pCPU0-pCPU5 model ordinary cores.
- pCPU6-pCPU7 model performance cores.
- VM0 is the Zephyr service VM:
  - Image: `sdk/images/zephyr.bin`
  - Load/entry: `0x42000000`
  - RAM identity window: `0x42000000-0x48000000`
  - pCPUs: 0, 2, 3, 4
- VM1 is the LK pre-launched VM:
  - Image: `sdk/images/lk.bin`
  - Load/entry: `0x40100000`
  - RAM identity window: `0x40000000-0x42000000`
  - pCPUs: 3, 5, 6, 7
- pCPU3 is intentionally shared by VM0 and VM1 AP vCPUs.

The first `create_vcpu()` call creates vCPU0/BSP. The ARM64 creation order keeps
the service VM BSP away from pCPU0 and keeps the pre-launched VM BSP away from
the pCPU shared with the service VM.

## Design Invariants

- VM configuration is static for QEMU RTOS bring-up.
- Stage-2 guest RAM is identity-mapped: IPA/GPA equals HPA/PA.
- vGIC and vPL011 guest IPA windows are not mapped as RAM; data aborts trap to
  MMIO emulation.
- Zephyr and LK are RTOS raw images and use `GUEST_FLAG_NO_FW`. This skips
  external ACPI/FDT boot modules; ARM64 can still pass a synthetic static vFDT.
- Future Linux support should use loader/module delivery and external FDT/ACPI
  handoff rather than platform `.incbin`.
- The host owns CNTP for scheduler ticks. Guest physical/virtual timer sysreg
  accesses are backed by vCPU CNTV state, with `timer_virq` preserving the
  guest-visible PPI ABI.
- Shared-pCPU scheduling requires vCPU-owned EL1 sysregs, CNTV state, and vGIC
  state to be saved/restored on context switch.

## Shell Validation

Useful CLAN shell commands:

- `vcpus`: VM/vCPU pCPU binding and state.
- `sched`: per-pCPU scheduler name, scheduler ticks, context switches, current
  thread, and thread state.
- `vmap`: host stage-1 and VM stage-2 maps.
- `vdump [vm id] [vcpu id]`: ARM64 VM/vCPU register and stack dump.
- `irqs`: interrupt names, active state, and per-pCPU counts.
- `vsh 0`: switch to Zephyr console, expect `zero ~>`.
- `vsh 1`: switch to LK console, expect `beau ~>`.
- Ctrl-D: return from VM console to `console:\>`.

`sched` field meaning:

- `ticks`: scheduler timer callback count on the pCPU. It represents periodic
  opportunities to charge runtime and request rescheduling.
- `ctx_switches`: number of times `schedule()` actually selected a different
  thread. A tick may not switch; wake/sleep/yield can also switch.

## Runtime Checklist

For VM/scheduler/console changes, validate:

1. QEMU finishes boot logs; pressing Enter prints `console:\>`.
2. VM0 and VM1 autostart.
3. `vcpus` shows VM0 on pCPU0,2,3,4 and VM1 on pCPU3,5,6,7.
4. `sched` shows `sched_iorr`; pCPU3 context switches grow when both VMs run.
5. `vsh 0` reaches `zero ~>` and the switch banner appears before replayed VM0
   logs.
6. `vsh 1` reaches `beau ~>`.
7. Ctrl-D returns to CLAN shell.
8. No `[cut here]`, `unexpected arm64 trap`, or host unexpected IRQ appears.

## Commenting Rules

Use English comments for:

- module ownership and architectural boundaries;
- stage-2 identity assumptions and validation;
- EL1/EL2 state ownership during vCPU scheduling;
- interrupt routing and vGIC LR sync/load/save;
- timer virtualization and host/guest timer ownership;
- console ownership and ring-buffer ordering.

Avoid comments that repeat simple assignments or function names.
