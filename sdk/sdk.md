## Codex Working Requirements

When Codex works on this repository, it must read this file first and also
apply the repository-local skill under `sdk/codex`. Use `sdk/codex/SKILL.md` as
the active BEAU hypervisor development workflow, and read
`sdk/codex/references/architecture.md` when architecture details, invariants,
commands, or validation checklists are needed.

Codex must keep changes focused on the existing BEAU ARM64/QEMU/rk356x design,
preserve the documented static VM and image-loading assumptions unless an
explicit task changes them, announce intended file or area edits before making
them, and follow the English design-comment rules for ARM64 virtualization
code.

BEAU development uses a human-run build and validation flow. Codex must not run
BEAU builds, QEMU boots, hardware flashing, or `scripts/regress.py` unless the
user explicitly asks for that specific run in the current task. By default,
Codex should provide the exact build, launch, regression, and hardware
validation commands for the user to run manually, and should treat their logs or
reported results as the source of validation truth. This repository-level rule
overrides any build or regression guidance in `sdk/codex/SKILL.md` or
`sdk/codex/references/architecture.md`.

Codex may run lightweight local checks that do not build or boot BEAU, such as
text searches, `git diff --check`, script syntax checks, DT source compilation
after an approved DTS change, and command dry-runs that only print commands.
The final response must clearly separate lightweight checks Codex actually ran
from manual build, QEMU, regression, or hardware validation that still requires
human confirmation.

Optimizations or behavior changes under `core/` require explicit human
confirmation before implementation. Document and discuss common-code timer,
scheduler, vCPU, VM, IRQ, or memory-management optimizations first, then wait for
approval before editing shared core code.

Changes to `sdk/image/linux/beau-linux.dts` require explicit human confirmation
before implementation. Treat VM2 Linux bootargs, CPU topology, device nodes,
memory ranges, interrupt/timer properties, and debug-only boot parameters in
this DTS as user-controlled. Discuss the intended DTS change first; after
approval, update the generated `sdk/image/linux/beau-linux.dtb` only as part of
that approved DTS change.

## ARM64 Development Status

The ARM64 bring-up currently targets QEMU `virt` for the standard manual
validation path and rk356x for hardware-platform bring-up. QEMU uses
`qemu-system-aarch64` with the `virt` machine, GICv3, EL2 virtualization
enabled, and 8 physical CPUs. Use the wrapper script for the default QEMU
launch:

```bash
./scripts/kick.py
```

The wrapper expands to the ARM64 QEMU `virt` command with:

```bash
qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=3,its=on \
  -cpu cortex-a57 \
  -smp 8 \
  -m 1024M \
  -nographic \
  -serial mon:stdio \
  -kernel out/qemu_out/beau.debug.out \
  -device loader,file=sdk/image/linux/Image,addr=0x70000000,force-raw=on \
  -device loader,file=sdk/image/linux/Initramfs.cpio.gz,addr=0x74000000,force-raw=on
```

For manual validation, set `BEAU_TOOLCHAINS` to the bare-metal toolchain bin
directory, then build:

```bash
./scripts/kick.py --build --dry-run
./scripts/kick.py --build
```

The rk356x hardware-platform skeleton builds with the same toolchain:

```bash
PATH=${BEAU_TOOLCHAINS}:$PATH \
make ARCH=arm64 PLATFORM=rk356x CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
```

The default build output directories are platform-scoped:
`out/qemu_out` for `PLATFORM=qemu` and `out/rk356x_out` for
`PLATFORM=rk356x`.

Hardware validation for rk356x is manual for now: flash the generated BEAU
image with the board workflow and inspect serial logs for EL2 boot, MMU setup,
GIC init, the BEAU shell prompt, and VM launch logs.

LK and Zephyr stay as `.incbin` RTOS images under `sdk/image`:
`sdk/image/lk.bin` and `sdk/image/zephyr.bin`. VM2 Linux uses
`sdk/image/linux/Image` and `sdk/image/linux/Initramfs.cpio.gz`; QEMU stages them with
`-device loader` at `0x70000000` and `0x74000000`, then BEAU copies them into
VM2 guest RAM. The Linux DTB is `sdk/image/linux/beau-linux.dtb` and remains embedded
as a small `.incbin` module because it describes Linux running on BEAU.

Current QEMU VM layout:

- VM0 is the Zephyr service VM.
  - Image: `sdk/image/zephyr.bin`
  - Raw image tag: `zephyr`
  - Load address and entry: `0x42000000`
  - Identity RAM window: `0x42000000-0x48000000`
  - vCPUs: 4, running on ordinary-core pCPU0, pCPU2, pCPU3, and pCPU4
- VM1 is the LK pre-launched VM.
  - Image: `sdk/image/lk.bin`
  - Raw image tag: `lk`
  - Load address and entry: `0x40100000`
  - Identity RAM window: `0x40000000-0x42000000`
  - vCPUs: 4, using mixed pCPUs 3, 5, 6, and 7
- VM2 is the Linux pre-launched VM.
  - Kernel image: `sdk/image/linux/Image`
  - Kernel module tag: `linux`
  - QEMU kernel stage address: `0x70000000`
  - Kernel load address and entry: `0x48080000`
  - Initramfs image: `sdk/image/linux/Initramfs.cpio.gz`
  - Initramfs module tag: `Initramfs.cpio.gz`
  - QEMU initramfs stage address: `0x74000000`
  - Initramfs load address: `0x4c000000`
  - DTB image: `sdk/image/linux/beau-linux.dtb`
  - DTB module tag: `beau-linux-dtb`
  - Boot console: `console=ttyAMA0 rdinit=/init earlycon=pl011,0x09000000`
  - Identity RAM window: `0x48000000-0x50000000`
  - vCPUs: 4, running on pCPU1, pCPU4, pCPU6, and pCPU7
  - QEMU vITS window: `0x08080000-0x0809ffff`
  - Initramfs shell: `uos` prompt as root
- pCPU0-pCPU5 model ordinary cores.
- pCPU6-pCPU7 model performance cores.
- VM0 uses ordinary cores only. VM1 may mix ordinary and performance cores; the
  static QEMU scenario uses pCPU3 as the shared ordinary core plus pCPU5-pCPU7.
  Each VM's vCPU0/BSP is kept on a pCPU that no other VM uses: VM0 on pCPU2,
  VM1 on pCPU5, and VM2 on pCPU1.

The generated `out/qemu_out/beau.debug.out` has been boot-tested on QEMU. The
build also emits `out/qemu_out/beau.out` as the base link image and
`out/qemu_out/beau.debug.bin` as the raw debug image. Zephyr and LK autostart
from embedded RTOS images. VM2 Linux autostarts when QEMU stages `Image` and
`Initramfs.cpio.gz`; BEAU supplies the embedded `beau-linux.dtb`.
The BEAU shell stays quiet during late guest AP bring-up; press Enter after the
boot logs settle to show the `console:\>` prompt.

## Implemented

- ARM64 build path for the QEMU `virt` platform.
- `scripts/kick.py` QEMU launcher with `--kernel`, `--qemu`, `--smp`,
  `--memory`, `--toolchains`, `--cross-prefix`, `--build`, `--dry-run`, and
  extra QEMU argument support.
- `scripts/regress.py` boot regression harness for build, QEMU launch, BEAU
  shell commands, VM console switching, and fatal boot-log checks.
- QEMU platform code and static board/scenario configuration under
  `arch/arm64/platform/qemu`.
- Bare-boot image embedding for LK and Zephyr raw images from `sdk/image`.
- Linux VM2 loader tags for externally staged `Image` and `Initramfs.cpio.gz`, plus the
  embedded `beau-linux.dtb` module.
- Static QEMU VM configuration for Zephyr as the service VM and LK/Linux as
  pre-launched VMs.
- QEMU guest RAM, GIC, and PL011 layout is centralized in each VM's
  `vm_configs[].arch` entry, following the ACRN-style `vm_configurations.c`
  source-of-truth pattern. Generic ARM64 code gets those values through the
  `asm/platform.h` platform API instead of hard-coded QEMU constants.
- Static QEMU RTOS VMs use a KISS stage-2 layout: each configured guest RAM
  window is mapped GPA/IPA == PA/HPA.
- Raw-image copies are checked against the owning VM's configured guest RAM
  range before writing into the 1:1 RAM window.
- Stage-2 initialization validates the 1:1 RAM invariant. The shared guest vGIC
  and vPL011 IPA windows are registered as vio MMIO instead of RAM mappings.
- Zephyr and LK are marked with `GUEST_FLAG_NO_FW`, so the boot-info path does
  not require external ACPI/FDT modules for the current RTOS images. VM2 Linux
  clears that flag, receives `beau-linux.dtb`, and uses the loader/module path
  instead of platform `.incbin` image embedding for `Image` and `Initramfs.cpio.gz`.
- EL2 entry, exception vector setup, MMU enablement, and 1:1 host mappings.
  The primary and secondary EL2 entry paths explicitly select `SP_EL2` with
  `SPSel=1`, so host scheduling, guest entry, and guest exit use the same EL2
  stack register.
- ARM64 guest entry/exit path for running EL1 guests under stage-2 translation.
- ARM64 vCPU entry builds the guest restore frame on the current vCPU thread
  stack and synchronizes guest-exit registers with `vcpu->arch.regs`, so
  scheduler context switches no longer treat the persistent register block as
  the live EL2 stack.
- ARM64 data-abort MMIO dispatch into the common MMIO vio path.
- Static vFDT generation for static ARM64 VMs, including CPU nodes, memory,
  PSCI, GICv3, timer, and PL011.
- Raw-image loader support for ARM64 guest RAM start and FDT placement.
- PSCI virtualization for guest `CPU_ON`, `CPU_OFF`, `AFFINITY_INFO`,
  `SYSTEM_OFF`, and `SYSTEM_RESET`.
- BEAU shell `reboot` command wired to host PSCI system reset.
- PSCI-based host secondary CPU bring-up with `MAX_PCPU_NUM=8`.
- VM0 and VM1 vCPUs share pCPU3 through the existing `sched_iorr` scheduler.
  VM0 uses ordinary-core pCPU0, pCPU2, pCPU3, and pCPU4; VM1 uses mixed pCPU3,
  pCPU5, pCPU6, and pCPU7. VM2 uses pCPU1, pCPU4, pCPU6, and pCPU7. The
  vCPU0/BSP pCPUs are private to each VM.
- ARM64 vCPU switch-in/out now saves and restores guest EL1 translation,
  exception, timer, TPIDR, and vGIC state, so two VMs can time-share one pCPU
  without inheriting each other's EL1 address-space context.
- ARM64 local physical timer setup enables the scheduler tick PPI on every pCPU.
  Guest timer state is kept on the virtual timer path so guest timer activity
  does not overwrite the host scheduler's physical timer deadline.
- ARM64 WFI/WFE trapping is disabled in the default QEMU scenario. Guest WFI/WFE
  stays on the virtual CPU interface so idle/spin paths do not exit to EL2 on
  every instruction; the trapped handler remains only as non-default debug
  plumbing.
- Shell running as a scheduler thread and VM launch gated to host BSP after all
  APs are running.
- ARM64 HVC dispatch recognizes ACRN hypercall IDs separately from PSCI HVC
  calls. The current ARM64 implementation supports basic API/HW info and
  GPA-to-HPA translation; x86-specific VM/device management hypercalls return
  not-supported until their ARM64 dependencies exist.
- VM and vCPU debug commands:
  - `vcpus`
  - `threads`
  - `schedstat`
  - `vmap`
  - `irqstat`
  - `constat [vm id]`
  - `dumpstat [vm id]`
  - `vsh <vm id>`
- Press Tab in the BEAU shell to display `registered commands`; `help` is not
  registered as a BEAU console command.
- `vsh <vm id>` switches the serial console to a VM vPL011/vUART console.
  Ctrl-D switches back to the BEAU shell.
- `schedstat` prints the scheduler algorithm and one physical-CPU row with
  `pcpu`, scheduler `timer` callbacks, context `switches`, `resched` requests,
  runnable-thread count, and current `thread`.
  - `timer` is the number of scheduler timer callbacks observed on that pCPU.
  - `switches` is the number of times `schedule()` actually selected a
    different thread.
  - `resched` counts requests raised through `make_reschedule_request()`,
    including tick, wake, yield, and remote reschedule paths.
  - `runqueue` is the current count of runnable threads bound to that pCPU.
- `irqstat` prints a short IRQ name/purpose column when the architecture can
  decode it. ARM64 maps ACRN IRQ numbers back to GIC SGI/PPI/SPI sources and
  names the EL2-owned SMP-call, physical-timer, virtual-timer, and
  vGIC-maintenance interrupts. It suppresses IRQs that have neither an active
  handler nor any recorded count, so unused INTIDs do not fill the table.
  Per-pCPU counts include every pCPU in aligned `cpuN:count` fields, and
  `active` shows whether the IRQ is allocated in the common IRQ table. IRQ
  counters saturate at `UINT64_MAX` instead of wrapping; saturated per-pCPU
  fields print as `cpuN:sat`, total prints `sat`, and `overflow` reports whether
  any per-pCPU counter or total sum has saturated.
- `dumpstat [vm id]` prints all created vCPUs in the VM, including the saved
  ARM64 register image, scheduler state, current-thread status, recent vCPU
  exit reason, recent virtual IRQ injection, recent guest timer programming or
  injection, a raw guest stack trace from the VM register image, and the host
  stack saved by the vCPU thread on its bound pCPU. IRQ exits report `ec:n/a`
  because `ESR_EL2.EC` is only meaningful for synchronous exits. Guest stack
  entries are raw addresses because BEAU does not embed guest symbol tables. The
  debug image embeds the BEAU symbol table, so host stack return addresses are
  printed as `function+offset`. Offline vCPUs skip stack output.
- `constat [vm id]` prints focused VM console state for live diagnostics:
  selected/input VM IDs, host input backlog, async TX ring usage and drops,
  vUART RX/TX state, vPL011 pending/assert/deassert counters, and the guest UART
  SPI state in the vGIC. Use it with `dumpstat`, `vcpus`, `schedstat`, and
  `irqstat` when debugging `vsh` responsiveness.
- ARM64 host exception call traces resolve return addresses through the
  embedded BEAU symbol table and print `function+offset` beside the raw LR.
- VM vPL011 TX output from the currently selected `vsh` VM is written into a
  Xen-style per-VM async console ring buffer, using monotonic producer/consumer
  indexes and a 4KB power-of-two data area with 4095 bytes of usable capacity.
  The console timer path runs every 5ms and drains up to
  `CONFIG_VM_CONSOLE_DRAIN_BUDGET` bytes, default 512, to the host serial
  console per pass. If the selected VM has at least half a ring of queued
  output, the drain path can temporarily use
  `CONFIG_VM_CONSOLE_DRAIN_BURST_BUDGET`, default 2048. This keeps `vsh 2` from
  synchronously replaying a full Linux boot-log ring while still clearing deep
  Linux console backlog faster than the first 128-byte drain experiment.
  Host-to-VM console input is first buffered in a small host backlog, then fed to
  the guest with `CONFIG_VM_CONSOLE_RX_BUDGET`, default 4 bytes per pass, and
  only while the guest RX FIFO is below `CONFIG_VM_CONSOLE_RX_LOW_WATERMARK`,
  default 1 byte. Ordinary input beyond the backlog can be dropped under
  sustained key repeat. Guest RX reads refill one queued byte while budget
  remains, so short commands stay ordered and Ctrl-D still switches back to the
  BEAU shell. This keeps a held key from monopolizing the console timer path or
  keeping Linux in continuous UART RX IRQ and shell echo handling.
- Non-selected VM console output is not replayed into the BEAU shell.
- VM exception logs have a separate 4KB/4095-byte per-VM ring reserved for VM
  trap/oops capture. The ring is internal debug plumbing and is no longer
  exposed as a BEAU shell command.
- vUART/vPL011 layering:
  - `vuart` owns the upper VM console interface and host console integration.
  - `vpl011` is the ARM64 PL011 backend implementation.
  - Backend notification is routed through `vuart_notify_rx()` instead of direct
    console-to-vPL011 calls.
- ARM64 vGICv3 keeps ordinary level IRQ pending while the sampled device line is
  asserted and clears it only through the device deassert path. vPL011 uses this
  to enter the vGIC injection path only on visible UART IRQ line changes, so a
  held console key no longer needs repeated `arch_trigger_level_intr(assert=true)`
  calls or vCPU event requests to redeliver RX interrupts. Guest UART MMIO exits
  on an already asserted line refresh the current vGIC LR in place so active RX
  interrupts can become active+pending without a remote wakeup storm.
- ARM64 vtimer stuck rescue has diagnostic plumbing for the pending-only timer
  LR case where Linux reaches idle with EL1 IRQs still masked and CNTV remains
  host-masked. This path is still under active investigation; it is not yet a
  proven stability fix for VM2 Linux held-Enter/vsh-switch stress.
- ARM64 vPL011 vio for guest PL011 MMIO and RX interrupt notification.
- ARM64 vPL011 avoids running the vGIC level-deassert path for ordinary PL011
  polling reads, especially Linux's frequent `FR` reads around console output.
  RX reads, interrupt status reads, interrupt-mask writes, interrupt clears, and
  newly raised TX-ready state still update the virtual IRQ line. vPL011 keeps
  internal counters for TX bytes, TX-ready raises, pending state, and virtual
  IRQ assert/deassert transitions for temporary diagnostics.
- Initial vGICv3 model:
  - VM and vCPU vGIC state.
  - ICH LR save/load/sync/flush.
  - VMCR/HCR setup.
  - Virtual interrupt injection.
  - Maintenance IRQ handler.
  - SGI system-register trap handling through `ICC_SGI1R_EL1`.
  - GICD/GICR MMIO vio and 64-bit MMIO split handling.
  - Redistributor frames are addressed by guest vCPU slot and mapped to the
    VM's local IRQ bank.
  - GICD/GICR `IPRIORITYR` byte/halfword/word access support.
  - GICD/GICR `ICFGR` read/write support for SGI/PPI/SPI trigger type state.
  - GICD `IROUTER` low/high word access and SPI target-vCPU delivery.
  - Virtual timer PPI handling and injection back into the running guest vCPU.
- Initial QEMU GICv3 ITS/vGICv3 ITS support:
  - QEMU launch and regression use `-machine
    virt,virtualization=on,gic-version=3,its=on`.
  - Host GICv3 early init detects and quiesces the QEMU ITS at
    `0x08080000-0x0809ffff`.
  - VM2 Linux has a guest ITS window and DT `msi-controller@8080000` node.
    VM0 Zephyr and VM1 LK do not expose ITS/LPI capability in the QEMU
    scenario.
  - vGICv3 advertises LPIs only for VMs with `guest_its_size != 0`, including
    GICD `LPIS/NumLPIs/IDbits` and GICR `PLPIS`.
  - vITS models CTLR/IIDR/TYPER/CBASER/CWRITER/CREADR/BASERn/TRANSLATER and
    the GICR LPI registers needed by Linux ITS setup.
  - vITS command queue supports MAPD, MAPC, MAPI, MAPTI, MOVI, DISCARD, INT,
    CLEAR, INV, INVALL, SYNC, and MOVALL for the current software model.
  - `arm64_vgicv3_inject_msi()` provides the hypervisor-side entry point for
    injecting a mapped `(device_id, event_id)` as an LPI.
- ARM64 IRQ domains for CPU-local and GIC interrupts.
- ARM64 memory logging for host stage-1 and VM stage-2 mappings.
- ARM64 exception stack dumps print directly to the console without per-line
  log prefixes.
- BEAU log and shell/console strings are lowercase source literals; there is no
  output-layer lowercase conversion.
- ARM64 memory virtualization, interrupt virtualization, and vCPU
  virtualization code now includes English comments for module responsibilities,
  design boundaries, and key state transitions.

## Verified

The following have been verified on QEMU with `-smp 8`:

- The hypervisor accepts Enter after boot and prints the `console:\>` shell
  prompt.
- VM0 Zephyr autostarts as the service VM.
- VM1 LK autostarts as a pre-launched VM.
- VM2 Linux autostarts as a pre-launched VM.
- VM0 Zephyr vCPU0-vCPU3 bind to ordinary-core pCPU2, pCPU0, pCPU3, and pCPU4.
- VM1 LK vCPU0-vCPU3 bind to mixed pCPU5, pCPU3, pCPU6, and pCPU7, sharing
  pCPU3 with VM0.
- VM2 Linux vCPU0-vCPU3 bind to pCPU1, pCPU4, pCPU6, and pCPU7.
- VM0 Zephyr enters EL1 at `0x42000000`.
- VM1 LK enters EL1 at `0x40100000`.
- Boot logs show each VM stage-2 RAM map as identity mapped:
  VM0 `ipa[0x42000000-0x48000000]:pa[0x42000000-0x48000000]` and VM1
  `ipa[0x40000000-0x42000000]:pa[0x40000000-0x42000000]`.
- `schedstat` reports `sched_iorr`, per-pCPU scheduler timer callbacks,
  reschedule requests, runnable-thread counts, and context switch counters.
- `vcpus` reports all three VMs and nine guest vCPUs. VM0 uses pCPU0, pCPU2,
  pCPU3, and pCPU4; VM1 uses pCPU3, pCPU5, pCPU6, and pCPU7; VM2 Linux uses
  pCPU1, pCPU4, pCPU6, and pCPU7.
- `vsh 0` enters the Zephyr console and reaches `zero ~>`.
- `vsh 1` enters the LK console and reaches `beau ~>`.
- `vsh 2` enters the Linux console and should reach the initramfs `uos` root
  shell. The VM2 regression now verifies root identity with `id` instead of
  using the old login flow.
- VM0 Zephyr `help` and VM1 LK `help` complete through the async VM console
  path. VM2 Linux shell commands remain part of the active timer/vGIC stability
  investigation.
- VM0 Zephyr `symtab list` completes through `vsh 0` and returns to the
  `zero ~>` prompt with async batched VM console output.
- VM console output bypasses vUART TX FIFO forwarding for the selected `vsh`
  VM console.
- Ctrl-D returns from VM console mode to `console:\>`.
- Five repeated QEMU cold boots reached VM0/VM1/VM2 guest entry without
  `[cut here]` or `unexpected arm64 trap`, covering the prior `SP_EL2`/`SPSel`
  boot race.
- Five repeated QEMU cold boots also covered the later `vcpu_exit_return`
  restore-frame issue where EL2 `sp` could drift to guest RAM and fault at
  `far:0x80000000`.
- `vmap` shows VM0/VM1/VM2 stage-2 RAM identity mappings plus vGICD, vGICR,
  and vPL011 vio windows.
- Boot logs show each VM image copied to 1:1 RAM.
- `irqstat` uses a narrow-screen-friendly format and shows the virtual timer
  PPI handler receiving counts on Zephyr AP pCPUs.
- Zephyr no longer traps on `GICD_IPRIORITYR` byte writes.
- Zephyr AP virtual timer interrupts no longer hit the host unexpected IRQ path.
- LK still boots with 4 CPUs after Zephyr became the service VM.
- The `reboot` shell command resets QEMU and restarts BEAU.
- `PLATFORM=rk356x` builds a hardware image. Boot correctness is pending manual
  flashing and serial-log validation.
- VM2 Linux no longer stops at `smp: Bringing up secondary CPUs ...` in the
  current 4-vCPU path: logs show CPU1, CPU2, and CPU3 entering EL1 and Linux
  reporting `smp: Brought up 1 node, 4 CPUs`.
- ITS regression `out/qemu_out/regress-its-vm2-only.log` was run after a
  successful QEMU build. VM0 Zephyr and VM1 LK passed the regression gate, and
  VM2 Linux enumerated the virtual ITS:
  `ITS [mem 0x08080000-0x0809ffff]`,
  `ITS: Using hypervisor restricted LPI range [8192]`, and allocated LPI
  pending tables for CPU0-CPU3. This predates the switch to the direct
  initramfs `uos` shell.

## Manual Regression

Use these commands when validating the QEMU 3OS console, vGIC, vtimer, and
vCPU-exit path manually. Set the toolchain path first:

```bash
export BEAU_TOOLCHAINS=$HOME/beau-cc/bin
```

For debug, shell, or console changes, remove stale debug artifacts before
building so `libdebug.a` and the final linked image are refreshed:

```bash
rm -f out/qemu_out/modules/libdebug.a \
      out/qemu_out/beau.out \
      out/qemu_out/beau.debug.out \
      out/qemu_out/beau.debug.bin
PATH=${BEAU_TOOLCHAINS}:$PATH \
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
```

Lightweight checks before booting:

```bash
python3 -m py_compile scripts/regress.py
sh -n scripts/repack_initramfs.sh
git diff --check
```

Standard QEMU smoke regression:

```bash
BEAU_TOOLCHAINS=${BEAU_TOOLCHAINS} \
./scripts/regress.py --no-build --timeout 240
```

The smoke run should reach the BEAU shell, pass `vcpus`, `schedstat`, `vmap`,
`irqstat`, `dumpstat 0`, enter and leave `vsh 0`, `vsh 1`, and `vsh 2`, and
verify VM2 Linux root identity with `id` showing `gid=0`.

VM console switch and held-Enter pressure regression:

```bash
BEAU_TOOLCHAINS=${BEAU_TOOLCHAINS} \
./scripts/regress.py --no-build --timeout 300 \
  --stress-vsh-switch \
  --stress-rounds 4 \
  --stress-enters 80 \
  --no-terminal-replies
```

This stress run models the manual failure sequence: enter VM2 and press Enter
repeatedly, switch to VM1 and press Enter, switch to VM0, then switch back to
VM2 and run `id`. Passing means VM2 still reaches `uos ~`, `id` returns `gid=0`,
no Linux RCU stall appears, and Ctrl-D still returns to `console:\>`.

If a regression fails, the harness tries to return to the BEAU shell and capture
`vcpus`, `schedstat`, `irqstat`, `constat <vmid>`, and `dumpstat <vmid>` into
`out/qemu_out/regress.log`. If reproducing manually through `./scripts/kick.py`,
run the same commands immediately after Ctrl-D:

```text
vcpus
schedstat
irqstat
constat 2
dumpstat 2
```

For the current VM2 issue, key failure indicators are Linux messages containing
`rcu_preempt kthread timer wakeup didn't happen`,
`Possible timer handling issue on cpu=0 timer-softirq=0`, VM2/vCPU0 stopped at
`elr:0xffff800080f0fe4c`, virtual timer `virq:27` pending, and a pending-only
timer LR such as `0x508000000000001b`.

## VM2 Linux Debug Snapshot

Status as of 2026-06-18:

- Current baseline: VM2 Linux uses `sdk/image/linux/Initramfs.cpio.gz` and
  `rdinit=/init`, enters the initramfs `uos` root shell on PL011, and the
  regression checks root identity with `id`. This basic root-console path is
  working and is the baseline to preserve before making any further VM2 timer
  or vGIC change.
- The baseline was restored after reverting the stale pending-only timer LR
  drop experiment. That experiment made boot and VM console handoff visibly
  slower and must not be treated as the starting point for future work.
- `dumpstat` no longer includes the Linux CSD wait probe or any direct
  `call_single_data_t` parser. Keep `dumpstat` focused on generic vCPU state,
  EL1 state, timer/vGIC state, vtimer trace, SGI dst state, compact local IRQ
  state, and raw guest/host stacks. `dumpstat` output uses `src`/`dst` labels
  instead of `source`/`target` labels.
- Development hygiene: avoid broad, low-signal local `rg` searches over large
  source trees. Prefer known relevant files in this repository and targeted
  reads from explicitly named external references, such as the Linux 7.1 source
  tree used for VM2 symbolication.
- Keep VM2 Linux at 4 vCPUs for validation. `maxcpus=1` or `maxcpu=1` can hide
  the SMP/timer symptom and may be useful as a temporary comparison point, but
  it is not a fix for the 4-vCPU VM2 Linux path.
- Keep Linux assets under `sdk/image/linux`; the active Linux DT source is
  `sdk/image/linux/beau-linux.dts`. This DTS must only be changed by an
  explicit manual edit. Do not let build scripts, regression scripts, or helper
  tools rewrite it implicitly.
- After manually changing `sdk/image/linux/beau-linux.dts`, regenerate the
  embedded DTB explicitly with:
  `dtc -I dts -O dtb -o sdk/image/linux/beau-linux.dtb sdk/image/linux/beau-linux.dts`.
  The BEAU image must then be rebuilt because `beau-linux.dtb` is included by
  `arch/arm64/platform/qemu/platform_image.S`.
- Timer DT interrupt IDs remain unchanged. The virtual timer still uses
  PPI/INTID 27 from `<1 13 4>`; current evidence does not require changing
  PPI27.
- WFI/WFE trapping is kept disabled by default. Reference checks:
  the croc reference traps WFE as yield and WFI as block, while the anoa
  reference traps WFE as yield and uses an IRQ-wait path for WFI with a yield
  threshold plus timeout. In BEAU QEMU, enabling TWI/TWE makes VM0/VM1/VM2
  noticeably slower, so the default relies on hardware/vGIC wakeup.
- The latest ITS validation keeps VM0/VM1 free of ITS/LPI exposure and exposes
  vITS only to VM2 Linux. This avoids perturbing RTOS guests while still
  allowing Linux to exercise GICv3 ITS and LPI table setup.
- vITS currently advertises a 14-bit LPI INTID space with a Linux-visible
  hypervisor restricted LPI range of 8192 IDs, but the in-hypervisor descriptor
  model stores only a compact 256-LPI active window. Mappings outside that
  active window are ignored until a dynamic LPI descriptor allocator is added.

### VM2 Linux RCU Stall Signature

- The remaining issue is post-login SMP runtime stability under VM console
  pressure, not pre-login boot: VM2 can reach the root console and run `id`, but
  the held-Enter plus `vsh 2 -> vsh 1 -> vsh 0 -> vsh 2` sequence can still
  trigger Linux RCU stall messages.
- The representative Linux log says
  `rcu_preempt kthread timer wakeup didn't happen`,
  `Possible timer handling issue on cpu=0 timer-softirq=0`, and shows target
  CPUs idling around `default_idle_call()`.
- The useful hypervisor-side evidence is centered on the virtual timer/vGIC
  path for VM2/vCPU0:
  - virtual timer PPI/INTID 27,
  - CNTV deadline/control state and `cntv_el2_masked`,
  - LR state for virtual INTID 27,
  - software vGIC descriptor state for virq 27,
  - host PPI27 enabled/pending/active state,
  - recent vtimer trace events and recent timer sysreg/inject records.
- A stale pending-only timer LR, such as `0x508000000000001b`, combined with an
  empty software descriptor is a diagnostic clue. It should not be repaired by
  blindly forcing EOI maintenance or by dropping the LR from broad sync paths.
- If the vtimer trace shows no timer sysreg writes while Linux is handling the
  interrupt, assume the CPU model may be allowing direct EL1 CNTV access despite
  the intended trap configuration. In that case, fixes must sample live CNTV
  state at real EL2 boundaries instead of relying only on `handle_timer_sysreg()`.

### VM2 Linux RCU Stall Progress, 2026-06-18

- Latest VM2 dumps show improvement from the earlier lost-LR symptom: vCPU0
  still stops around Linux `cpu_do_idle()` return, but the virtual timer LR is
  visible as pending-only, for example `live_lr0:0x508000000000001b` for
  virtual INTID 27. The remaining stall is therefore no longer primarily "timer
  LR disappeared"; it is a forward-progress window after WFI wakeup and before
  Linux handles the pending timer interrupt.
- `constat 2` from failing stress runs shows the VM console input path is not
  the immediate stuck point after Ctrl-D diagnostics: host input backlog is
  empty, vUART RX is empty, vPL011 RX IRQ is not asserted, SPI33 is not pending,
  and the async TX ring may have drops only from the heavy Linux log stream.
  That points the active failure boundary back to VM2 vtimer/vGIC/vCPU-exit
  progress rather than lost host-to-guest console bytes.
- Symbolication for the active VM2 Linux image maps runtime
  `0xffff800080f0fe4c` to `cpu_do_idle+8`, the instruction after WFI and before
  `arch_cpu_idle()` returns toward `default_idle_call()`. Linux reaches this
  point with PSTATE.I still set, then must execute a few more instructions and
  reach `local_irq_enable()`/`daifclr` before the pending timer IRQ can be
  handled normally.
- Representative dump state for the remaining failure:
  - vCPU0 `elr:0xffff800080f0fe4c`, `lr:0xffff800080f0fe60`,
    `spsr:0x600000c5`, and live `DAIF` IRQ masked.
  - virtual timer virq 27 is enabled and pending in the vGIC, active is clear,
    and `cntv_el2_masked:yes`.
  - the vtimer trace repeatedly shows pending-only timer LR preservation rather
    than total loss of the timer interrupt.
  - some failing captures report `wfx:none`, meaning the observed stall can
    occur on the normal physical IRQ return path, not only inside the trapped
    WFI diagnostic path.
- Earlier local fix attempts were limited to ARM64 guest exit/vGIC code:
  - `handle_wfx()` no longer yields WFI when a virtual IRQ is already pending,
    even if the saved guest PSTATE still masks IRQs.
  - the sync-trap and physical-IRQ return paths now refresh the current vtimer
    and vGIC state before honoring host `need_reschedule()`.
  - if a pending guest IRQ is visible after that refresh, the return path skips
    the host reschedule once and returns to EL1 so Linux can leave the idle path
    and unmask interrupts. Physical IRQ exits still run host softirq processing
    before this refresh/schedule decision; skipping host softirqs made the BEAU
    console nearly unresponsive when VM2 kept virq27 pending.
- The 2026-06-18 follow-up is allowed to modify `core/schedule.c` while still
  keeping IORR as the scheduler. The FreeBSD arm64 vmm I/O reference keeps
  vtimer delivery as a vGIC pending/notify problem: vtimer sync asserts or
  deasserts the virtual timer IRQ from the current timer condition, the vGIC
  tracks active/pending IRQs in a queue, and queued injection notifies the vCPU.
  BEAU therefore should not let a host scheduler tick repeatedly consume the
  guest return window when there is no other runnable thread on that pCPU.
- Validation status for the current local tree:
  `python3 -m py_compile scripts/regress.py`, `sh -n scripts/repack_initramfs.sh`,
  and `git diff --check` pass. The QEMU BEAU image rebuilds successfully with:

  ```bash
  PATH=${BEAU_TOOLCHAINS}:$PATH \
  make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
  ```

  Updated outputs are `out/qemu_out/beau.debug.out` and
  `out/qemu_out/beau.debug.bin`.
- Regression status after the PPI27 priority-mask repair and the
  `core/schedule.c` no-op reschedule clear: the standard VM0/VM1/VM2 root-console
  path passes, and the VM console pressure gate
  `--stress-vsh-switch --stress-rounds 4 --stress-enters 80
  --no-terminal-replies` also passes without the previous VM2 Linux
  `timer-softirq=0` RCU stall. Keep this stress gate in future validation; add
  repeated cold boots or longer rounds before treating the path as fully aged.
- A one-shot experiment that cleared TWI after converting WFI rescue into LR
  rescue did not solve the failure and made the 40-enter stress fail earlier.
  Do not treat "clear TWI after LR rescue" by itself as the next root fix.
- Before the PPI27 priority-mask repair, the 2026-06-18 follow-up reproduced with
  IORR unchanged: CPU0/CPU1 were reported idling around `default_idle_call()`,
  RCU said `rcu_preempt kthread timer wakeup didn't happen`, and the warning
  remained `Possible timer handling issue on cpu=0 timer-softirq=0`. This
  supported the narrower forward-progress hypothesis: the pending-only vtimer LR
  was present, but VM2/vCPU0 could still be preempted by host scheduling before
  Linux ran far enough past `cpu_do_idle+8` to execute
  `local_irq_enable()`/`daifclr`.
- The current narrow fix keeps IORR and adds a bounded ARM64 vGIC-only
  no-reschedule window for rescued pending-only virtual timer LRs. While
  `vtimer_lr_rescue` is set and saved guest PSTATE still has DAIF.I masked,
  the timer pending state can block a few host `need_reschedule()` decisions so
  Linux gets a short EL1 run window. After that small budget is consumed, IORR
  fairness is restored so a stuck timer line cannot monopolize a shared pCPU.
  `dumpstat` now prints this as `rescue:... lr:<yes|no> budget:N`.
- The next failing dumps after that change showed an expired CNTV deadline,
  `cntv_el2_masked:Y`, a pending-only timer LR, and no later timer sysreg write.
  A live CNTV_CTL sample could be `0x1` even though EL2 computed the deadline as
  expired. That points to this QEMU CPU model not reliably trapping EL1 CNTV_CTL
  reads and not reliably reporting ISTATUS while the physical PPI27 line is
  disabled. The current repair therefore keeps PPI27 enabled and masks only its
  host priority (`0xff`, below the host PMR threshold) while a virtual timer LR
  owns delivery. When the guest reprograms/EOIs the timer, EL2 restores priority
  `0x80`. Linux can still observe the architecturally expired CNTV timer, while
  the host avoids repeated PPI27 IRQs.
- The core scheduling follow-up adds a narrow no-op reschedule clear:
  `sched_clear_reschedule_if_current_only()` clears `NEED_RESCHEDULE` only when
  the current non-idle thread is the only runnable/running non-blocking object
  on that pCPU. In the ARM64 guest-return path this clear happens before the
  vGIC pending-IRQ check, so VM2/vCPU0 on its private pCPU does not burn the
  bounded virtual-timer LR rescue budget on a scheduler tick that would have
  selected the same thread. Shared pCPUs still keep the bounded pending-IRQ
  window and then fall back to IORR fairness.
- Do not replace IORR for this issue. A dynamic tick-gating experiment reduced
  pCPU1 host physical timer callbacks but still failed the VM2 Linux stress path
  and changed timing rather than fixing the underlying vtimer/vGIC lifecycle.

### RCU Stall Repair Strategy

1. Preserve the fast root-console baseline first. Before any VM2 RCU experiment,
   run the standard QEMU regression and require VM0 Zephyr, VM1 LK, and VM2
   Linux root identity checks to pass.
2. Keep `sdk/image/linux/beau-linux.dts` unchanged. Core scheduler changes
   require explicit confirmation; the approved scope for this follow-up is the
   `core/schedule.c` no-op reschedule clear, not a scheduler algorithm switch.
3. Reproduce the stall from VM2 root shell with the 4-vCPU configuration. On
   the first RCU warning, return to the BEAU shell and capture `dumpstat 2`,
   `vcpus`, `schedstat`, and `irqstat`.
4. In `dumpstat 2`, compare vCPU0 against the other VM2 vCPUs: live CNTV_CTL,
   CNTV_CVAL, CNTVCT, `cntv_el2_masked`, timer virq, LR0/LR1, vGIC descriptor
   pending/active/level bits, AP registers, host PPI27 state, and the
   `rescue` LR budget.
5. Fix in the ARM64 vGIC/vtimer lifecycle, not in Linux bootargs. The likely
   repair boundary is a narrow synchronization point where EL2 already knows a
   timer LR completed or a loaded vCPU is leaving/entering guest execution.
6. Keep guest-visible CNTV state and EL2 host-mask state separate. EL2 may need
   to throttle host PPI delivery, but Linux's timer handler must still be able
   to observe an architecturally expired CNTV timer when it handles PPI27.
7. If a fix samples live CNTV, do it only at bounded points such as vCPU
   switch-in/out, timer IRQ injection, maintenance/EOI handling, or explicit
   guest exits. Avoid background backup timers or always-on polling in the QEMU
   3OS path unless a later design proves they are required.
8. Treat the RCU fix as complete only when VM2 reaches the initramfs `uos` root
   shell, remains responsive long enough to cover the earlier RCU stall window,
   emits no `timer-softirq=0` RCU warning, and VM0/VM1 still pass the regression
   gate.

### Experiments Not To Repeat

- Marking pending-only software timer LRs with EOI caused maintenance storms
  and broke AP bring-up.
- Requesting underflow maintenance (`HCR.UIE`) for rescued pending-only timer
  LRs caused an underflow maintenance storm: dumps showed `HCR.UIE`, `MISR.U`,
  one still-valid pending timer LR, and hundreds of thousands of vGIC
  maintenance interrupts while VM2 Linux timed out. Do not use UIE as a
  re-flush mechanism for this rescued timer window.
- Replacing EL2's live CNTV mask with broad host PPI27 enable/disable logic
  slowed or stalled RTOS console validation and was reverted. Disabling PPI27
  at the redistributor can also make Linux-visible CNTV_CTL.ISTATUS unreliable
  on the QEMU CPU model used here. The current accepted variant keeps the PPI
  enabled and masks only host priority while a vGIC timer LR owns delivery.
- Dropping stale pending-only timer LRs from WFI or broad vGIC sync paths made
  startup visibly slower and was reverted.
- Rewriting the WFI pending-only timer LR as Active+Pending made forward
  progress worse and was reverted.
- Re-enabling WFI/WFE traps slowed the QEMU 3OS scenario and affected VM0/VM1.
  Keep HCR_EL2.TWI/TWE clear unless a specific diagnostic run requires trapped
  idle instructions.
- Porting anoa's full switch-out backup timer model directly to BEAU caused a
  large VM2 slowdown. Do not arm a backup timer on every vCPU unload in the
  current QEMU 3OS path.
- Skipping host softirq processing on physical IRQ return when a guest IRQ is
  pending starved the BEAU shell/console during the VM2 held-Enter scenario and
  was reverted. Keep `do_softirq_no_irqenable()` on the physical IRQ exit path.
- Dynamically disabling IORR scheduler ticks when only the current thread is
  runnable is not the right fix. It can hide pCPU tick pressure but does not
  guarantee VM2 timer forward progress and should stay reverted unless a broader
  scheduler design is reviewed separately.
- Expanding the vITS software model to a static 8192-LPI descriptor array
  caused QEMU to stop producing useful shell output for more than 60 seconds.
  Keep the compact active-window model unless dynamic allocation is added.
- Expanding the generated QEMU vFDT PL011 clocks to include both `uartclk` and
  `apb_pclk` did not affect VM2. VM2 uses the embedded
  `sdk/image/linux/beau-linux.dtb`, whose DTS already has both clocks.

## Code Commenting Guidelines

New ARM64 virtualization code should use English comments for design intent,
not line-by-line narration. The goal is to make the virtualization model
auditable when memory, interrupt, and CPU state crosses an EL2/EL1 boundary.

- Add a module-level comment when a file owns an architectural subsystem or a
  cross-layer boundary, such as host stage-1 mappings, guest stage-2 mappings,
  vGIC state, physical GIC routing, vCPU entry/exit, or trap dispatch.
- Add structure comments for state that mirrors hardware or guest-visible
  state, especially EL2 control registers, vGIC list registers, interrupt
  descriptors, VM layout data, and deferred trap state.
- Add comments before non-obvious state transitions: syncing hardware list
  registers into software state, flushing pending virtual IRQs into LRs,
  translating a stage-2 abort into an MMIO request, loading or unloading EL2
  guest context, switching between host IRQ and guest IRQ paths, and validating
  VM memory isolation.
- For abort/trap handling code, comments must distinguish instruction aborts
  from data aborts and from broader memory abort terminology. State the trigger
  scenario being handled, such as instruction fetch from unmapped or
  execute-never memory, load/store MMIO data aborts, stage-2 translation faults,
  permission faults, or unexpected host EL2 aborts.
- Document the design invariant and failure mode when code enforces isolation,
  ordering, or ownership. Examples: QEMU RTOS stage-2 RAM is identity-mapped,
  vGIC MMIO is protected by the VM vGIC lock, and physical INTIDs must pass
  through the ARM64 IRQ domain before common IRQ dispatch.
- Avoid comments that restate a simple assignment, branch condition, or function
  name. Prefer clear names for local mechanics and reserve comments for intent,
  architecture rules, and concurrency assumptions.
- Keep comments close to the code that enforces the rule. If a later patch
  changes the rule, update the comment in the same patch.

## Current Limitations

The ARM64 port is now capable of booting Zephyr, LK, and VM2 Linux on QEMU, but
it is still a bring-up target rather than a complete architectural
virtualization port.

- vGICv3 is sufficient for the current Zephyr, LK, and 4-vCPU VM2 Linux boot
  path, and now has initial VM2-only vITS/LPI support for Linux ITS
  enumeration. It is not a complete GICv3/ITS model yet: active-state stress
  cases, deeper redistributor behavior, PCI/MSI requester identity plumbing,
  physical ITS passthrough, dynamic LPI allocation, and richer distributor
  coverage still need work.
- `GITS_TRANSLATER` direct MMIO injection currently uses device ID 0 as a local
  test path. Real passthrough/MSI code should call `arm64_vgicv3_inject_msi()`
  after requester identity and event mapping are plumbed.
- QEMU VM layout is still statically configured in `vm_config.c`;
  QEMU FDT parsing is not yet used to derive VM layout.
- Zephyr and LK are RTOS raw images and boot with `GUEST_FLAG_NO_FW`. VM2 Linux
  uses a loader/module path for `Image` and `Initramfs.cpio.gz`, while its Linux-on-BEAU
  DTB remains embedded.
- VM2 Linux runs as a 4-vCPU guest. The path should reach the initramfs `uos`
  shell during QEMU validation. The held-Enter and `vsh` switch pressure test
  is now part of the acceptance gate because it previously reproduced the VM2
  RCU/vtimer failure.
- VM2 Linux keeps `earlycon=pl011,0x09000000` so `vsh 2` can show Linux logs
  before the normal PL011 console driver is registered.
- VM console output from SMP guests can interleave because multiple guest CPUs
  write concurrently to the same PL011 console.
- Stage-2 mapping is still static for the QEMU `virt` platform.
- The regression harness covers the core multi-VM boot sequence and has a manual
  `--stress-vsh-switch` path for the current console pressure scenario. That
  stress path previously reproduced the VM2 RCU/vtimer failure and should remain
  part of the acceptance gate for future fixes.

## Next Steps

1. Add longer/repeated VM2 Linux stress coverage around
   `--stress-vsh-switch --stress-rounds 4 --stress-enters 80
   --no-terminal-replies`, including repeated cold boots, to age the current
   PPI27 priority-mask plus no-op-reschedule fix.
2. Keep using `constat 2`, `dumpstat 2`, `vcpus`, `schedstat`, and `irqstat` on
   failures. If `constat 2` still shows empty input backlog, empty vUART RX, and
   SPI33 not pending, continue focusing on VM2 timer/vGIC state rather than
   host-to-guest console input.
3. Add repeated cold-boot and initramfs-shell coverage for VM2 Linux 4-vCPU/SMP,
   with diagnostics for timer, SGI, and pending/active LR state on failures.
4. Extend `scripts/regress.py` with more VM2 Linux initramfs-shell commands, reboot
   coverage, repeated cold boots, and saved log artifacts suitable for CI.
5. Audit ARM64 abort handling to confirm whether guest instruction aborts are
   trapped and diagnosed correctly. Cover instruction fetch aborts separately
   from data abort MMIO paths, document the expected ESR/FAR/HPFAR trigger
   scenarios, and update comments so instruction abort, data abort, and broader
   memory abort terminology are not conflated.
6. Move QEMU platform memory and device discovery toward host-FDT-derived data
   where it helps reduce static board assumptions.
7. Bring up rk356x hardware manually, then capture the validated RAM, UART,
   GIC, and boot-image placement assumptions back into the platform files.
