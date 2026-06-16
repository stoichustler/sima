# SIMA ARM64 Reference

## Repository Map

- `Makefile`: static ARM64 platform config path when `ARCH=arm64
  PLATFORM=qemu` or `ARCH=arm64 PLATFORM=rk356x`.
- `scripts/kick.py`: QEMU launcher for `out/qemu_out/sima.debug.out`.
- `scripts/regress.py`: boot regression harness for QEMU shell and VM console checks.
- `arch/arm64/platform/qemu`: QEMU board/scenario, static VM config, embedded RTOS images, platform helpers.
- `arch/arm64/platform/rk356x`: rk356x hardware-platform constants, static VM
  config, embedded RTOS images, and platform helpers.
- `arch/arm64/guest`: stage-2 VM setup, vCPU entry/exit, vGICv3, vPL011, hypercall handling.
- `core`: common VM, vCPU, scheduler, timer, hypercall, and MMIO request logic.
- `sdk/boot`: boot module discovery and kernel loaders.
- `sdk/debug`: SIMA shell, console, debug commands, vsh, dumpstat, irqs.
- `sdk/sdk.md`: current implementation status and verified behavior.

## Build And Run

Build:

Set `SIMA_TOOLCHAINS` to the bare-metal toolchain bin directory, then run:

```bash
./scripts/kick.py --build --dry-run
./scripts/kick.py --build
```

Build rk356x hardware image:

```bash
PATH=${SIMA_TOOLCHAINS}:$PATH \
make ARCH=arm64 PLATFORM=rk356x CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
```

Default platform build outputs are `out/qemu_out` for QEMU and
`out/rk356x_out` for rk356x.

rk356x hardware validation is manual for now: flash the generated image using
the board workflow and inspect serial logs for EL2 boot, MMU setup, GIC init,
the SIMA shell prompt, and VM launch logs.

When editing `sdk/debug/*`, stale archive dependencies may hide source changes.
Force a debug rebuild before validation:

```bash
rm -f out/qemu_out/modules/libdebug.a out/qemu_out/sima.out out/qemu_out/sima.debug.out out/qemu_out/sima.debug.bin
PATH=${SIMA_TOOLCHAINS}:$PATH \
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
```

When changing shared layout headers such as `vm.h`, `vcpu.h`, or ARM64 guest
vGIC/vCPU headers, prefer a clean build before runtime validation. Mixed object
layouts can make VM/vCPU state appear corrupted even when the source logic is
correct.

Run:

```bash
./scripts/kick.py
```

Dry run:

```bash
./scripts/kick.py --dry-run
```

Regression:

```bash
./scripts/regress.py
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
  -kernel out/qemu_out/sima.debug.out \
  -device loader,file=sdk/images/linux/Image,addr=0x70000000,force-raw=on \
  -device loader,file=sdk/images/linux/Initrd,addr=0x74000000,force-raw=on
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
- VM2 is the Linux pre-launched VM:
  - Kernel image: `sdk/images/linux/Image`
  - QEMU kernel stage address: `0x70000000`
  - Kernel load/entry: `0x48080000`
  - Initrd image: `sdk/images/linux/Initrd`
  - QEMU initrd stage address: `0x74000000`
  - Initrd load: `0x4c000000`
  - DTB: `sdk/images/linux/sima-linux.dtb`
  - RAM identity window: `0x48000000-0x50000000`
  - pCPUs: 1, 4, 6, 7
  - Boot console: `console=ttyAMA0 earlycon=pl011,0x09000000`
  - Login: `root` / `root`
- pCPU3 is intentionally shared by VM0 and VM1 AP vCPUs.
  Each VM's vCPU0/BSP pCPU is private: VM0 uses pCPU2, VM1 uses pCPU5,
  and VM2 uses pCPU1.

QEMU and rk356x both keep guest images under `sdk/images`. LK and Zephyr are
embedded RTOS raw images. Linux `Image`, `Initrd`, `sima-linux.dts`, and
`sima-linux.dtb` live under `sdk/images/linux`; `Image` and `Initrd` are staged
by a loader and copied by SIMA, while `sima-linux.dtb` is embedded as the
Linux-on-SIMA DTB module.

The first `create_vcpu()` call creates vCPU0/BSP. The ARM64 creation order keeps
the service VM BSP away from pCPU0 and keeps every VM BSP on a pCPU that is not
used by another VM.

## Design Invariants

- VM configuration is static for QEMU and rk356x RTOS bring-up.
- Stage-2 guest RAM is identity-mapped: IPA/GPA equals HPA/PA.
- vGIC and vPL011 guest IPA windows are not mapped as RAM; data aborts trap to
  MMIO emulation.
- Zephyr and LK are RTOS raw images and use `GUEST_FLAG_NO_FW`. This skips
  external ACPI/FDT boot modules; ARM64 can still pass a synthetic static vFDT.
- VM2 Linux clears `GUEST_FLAG_NO_FW`, uses loader/module delivery for `Image`
  and `Initrd`, and receives the embedded `sima-linux.dtb`.
- VM2 Linux keeps PL011 earlycon enabled so `vsh 2` can replay kernel logs even
  before the normal PL011 console driver is registered.
- The host owns CNTP for scheduler ticks. Guest physical/virtual timer sysreg
  accesses are backed by vCPU CNTV state, with `timer_virq` preserving the
  guest-visible PPI ABI.
- Shared-pCPU scheduling requires vCPU-owned EL1 sysregs, CNTV state, and vGIC
  state to be saved/restored on context switch.

## Shell Validation

Useful SIMA shell commands:

- `vcpus`: VM/vCPU pCPU binding and state.
- `schedstat`: scheduler algorithm plus per-pCPU timer, switch, reschedule,
  runnable-thread, and current-thread statistics.
- `vmap`: host stage-1 and VM stage-2 maps.
- `dumpstat [vm id]`: ARM64 VM/vCPU register, scheduler state, raw guest stack,
  and saved host stack for the vCPU thread on its bound pCPU. Guest stack
  entries remain raw addresses because guest symbols are not embedded. Host
  return addresses resolve through the symbol table embedded in
  `sima.debug.out`; offline vCPUs skip stack output.
- `irqs`: interrupt names, active state, and per-pCPU counts.
- `vsh 0`: switch to Zephyr console, expect `zero ~>`.
- `vsh 1`: switch to LK console, expect `beau ~>`.
- Ctrl-D: return from VM console to `console:\>`.

`schedstat` field meaning:

- `timer`: scheduler timer callback count on the pCPU. It represents periodic
  opportunities to charge runtime and request rescheduling.
- `switches`: number of times `schedule()` actually selected a different
  thread. A tick may not switch; wake/sleep/yield can also switch.
- `resched`: number of requests raised through `make_reschedule_request()`.
- `runqueue`: number of runnable threads currently bound to the pCPU.

## Runtime Checklist

For VM/scheduler/console changes, validate:

1. QEMU finishes boot logs; pressing Enter prints `console:\>`.
2. VM0, VM1, and VM2 autostart.
3. `vcpus` shows VM0 on pCPU0,2,3,4, VM1 on pCPU3,5,6,7, and VM2 on pCPU1,4,6,7.
4. `schedstat` shows `sched_iorr`; pCPU3 context switches grow when VM0 and VM1 run.
5. `vsh 0` reaches `zero ~>` and the switch banner appears before replayed VM0
   logs.
6. `vsh 1` reaches `beau ~>`.
7. `vsh 2` reaches `clou login:` and logs in as `root` / `root`.
8. Ctrl-D returns to SIMA shell.
9. No `[cut here]`, `unexpected arm64 trap`, or host unexpected IRQ appears.

## Commenting Rules

Use English comments for:

- module ownership and architectural boundaries;
- stage-2 identity assumptions and validation;
- EL1/EL2 state ownership during vCPU scheduling;
- interrupt routing and vGIC LR sync/load/save;
- timer virtualization and host/guest timer ownership;
- console ownership and ring-buffer ordering.

Avoid comments that repeat simple assignments or function names.
