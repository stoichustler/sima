## ARM64 Development Status

The ARM64 bring-up currently targets QEMU `virt` for automated validation and
rk356x for hardware-platform bring-up. QEMU uses `qemu-system-aarch64` with the
`virt` machine, GICv3, EL2 virtualization enabled, and 8 physical CPUs. Use the
wrapper script for the default QEMU launch:

```bash
./scripts/kick.py
```

The wrapper expands to the ARM64 QEMU `virt` command with:

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

Set `SIMA_TOOLCHAINS` to the bare-metal toolchain bin directory, then build:

```bash
./scripts/kick.py --build --dry-run
./scripts/kick.py --build
```

The rk356x hardware-platform skeleton builds with the same toolchain:

```bash
PATH=${SIMA_TOOLCHAINS}:$PATH \
make ARCH=arm64 PLATFORM=rk356x CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
```

The default build output directories are platform-scoped:
`out/qemu_out` for `PLATFORM=qemu` and `out/rk356x_out` for
`PLATFORM=rk356x`.

Hardware validation for rk356x is manual for now: flash the generated SIMA
image with the board workflow and inspect serial logs for EL2 boot, MMU setup,
GIC init, the SIMA shell prompt, and VM launch logs.

LK and Zephyr stay as `.incbin` RTOS images under `sdk/images`:
`sdk/images/lk.bin` and `sdk/images/zephyr.bin`. VM2 Linux uses
`sdk/images/linux/Image` and `sdk/images/linux/Initrd`; QEMU stages them with
`-device loader` at `0x70000000` and `0x74000000`, then SIMA copies them into
VM2 guest RAM. The Linux DTB is `sdk/images/linux/sima-linux.dtb` and remains embedded
as a small `.incbin` module because it describes Linux running on SIMA.

Current QEMU VM layout:

- VM0 is the Zephyr service VM.
  - Image: `sdk/images/zephyr.bin`
  - Raw image tag: `zephyr`
  - Load address and entry: `0x42000000`
  - Identity RAM window: `0x42000000-0x48000000`
  - vCPUs: 4, running on ordinary-core pCPU0, pCPU2, pCPU3, and pCPU4
- VM1 is the LK pre-launched VM.
  - Image: `sdk/images/lk.bin`
  - Raw image tag: `lk`
  - Load address and entry: `0x40100000`
  - Identity RAM window: `0x40000000-0x42000000`
  - vCPUs: 4, using mixed pCPUs 3, 5, 6, and 7
- VM2 is the Linux pre-launched VM.
  - Kernel image: `sdk/images/linux/Image`
  - Kernel module tag: `linux`
  - QEMU kernel stage address: `0x70000000`
  - Kernel load address and entry: `0x48080000`
  - Initrd image: `sdk/images/linux/Initrd`
  - Initrd module tag: `linux-initrd`
  - QEMU initrd stage address: `0x74000000`
  - Initrd load address: `0x4c000000`
  - DTB image: `sdk/images/linux/sima-linux.dtb`
  - DTB module tag: `sima-linux-dtb`
  - Boot console: `console=ttyAMA0 earlycon=pl011,0x09000000`
  - Identity RAM window: `0x48000000-0x50000000`
  - vCPUs: 4, running on pCPU1, pCPU4, pCPU6, and pCPU7
  - Login: `root` / `root`
- pCPU0-pCPU5 model ordinary cores.
- pCPU6-pCPU7 model performance cores.
- VM0 uses ordinary cores only. VM1 may mix ordinary and performance cores; the
  static QEMU scenario uses pCPU3 as the shared ordinary core plus pCPU5-pCPU7.
  Each VM's vCPU0/BSP is kept on a pCPU that no other VM uses: VM0 on pCPU2,
  VM1 on pCPU5, and VM2 on pCPU1.

The generated `out/qemu_out/sima.debug.out` has been boot-tested on QEMU. The
build also emits `out/qemu_out/sima.out` as the base link image and
`out/qemu_out/sima.debug.bin` as the raw debug image. Zephyr and LK autostart
from embedded RTOS images. VM2 Linux autostarts when QEMU stages `Image` and
`Initrd`; SIMA supplies the embedded `sima-linux.dtb`.
The SIMA shell stays quiet during late guest AP bring-up; press Enter after the
boot logs settle to show the `console:\>` prompt.

## Implemented

- ARM64 build path for the QEMU `virt` platform.
- `scripts/kick.py` QEMU launcher with `--kernel`, `--qemu`, `--smp`,
  `--memory`, `--toolchains`, `--cross-prefix`, `--build`, `--dry-run`, and
  extra QEMU argument support.
- `scripts/regress.py` boot regression harness for build, QEMU launch, SIMA
  shell commands, VM console switching, and fatal boot-log checks.
- QEMU platform code and static board/scenario configuration under
  `arch/arm64/platform/qemu`.
- Bare-boot image embedding for LK and Zephyr raw images from `sdk/images`.
- Linux VM2 loader tags for externally staged `Image` and `Initrd`, plus the
  embedded `sima-linux.dtb` module.
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
  clears that flag, receives `sima-linux.dtb`, and uses the loader/module path
  instead of platform `.incbin` image embedding for `Image` and `Initrd`.
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
- SIMA shell `reboot` command wired to host PSCI system reset.
- SIMA shell `crash` command for intentionally triggering an ARM64 host data
  abort, printing the exception stack, and cold rebooting automatically.
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
  - `irqs`
  - `dumpstat [vm id]`
  - `vsh <vm id>`
- `vsh <vm id>` switches the serial console to a VM vPL011/vUART console.
  Ctrl-D switches back to the SIMA shell.
- `schedstat` prints the scheduler algorithm and one physical-CPU row with
  `pcpu`, scheduler `timer` callbacks, context `switches`, `resched` requests,
  runnable-thread count, and current `thread`.
  - `timer` is the number of scheduler timer callbacks observed on that pCPU.
  - `switches` is the number of times `schedule()` actually selected a
    different thread.
  - `resched` counts requests raised through `make_reschedule_request()`,
    including tick, wake, yield, and remote reschedule paths.
  - `runqueue` is the current count of runnable threads bound to that pCPU.
- `irqs` prints a short IRQ name/purpose column when the architecture can decode
  it. ARM64 maps ACRN IRQ numbers back to GIC SGI/PPI/SPI sources and names the
  EL2-owned SMP-call, physical-timer, virtual-timer, and vGIC-maintenance
  interrupts. Per-pCPU counts use aligned `cpuN:count` fields, and `active`
  shows whether the IRQ is allocated in the common IRQ table.
- `dumpstat [vm id]` prints all created vCPUs in the VM, including the saved
  ARM64 register image, scheduler state, current-thread status, recent vCPU
  exit reason, recent virtual IRQ injection, recent guest timer programming or
  injection, a raw guest stack trace from the VM register image, and the host
  stack saved by the vCPU thread on its bound pCPU. IRQ exits report `ec:n/a`
  because `ESR_EL2.EC` is only meaningful for synchronous exits. Guest stack
  entries are raw addresses because SIMA does not embed guest symbol tables. The
  debug image embeds the SIMA symbol table, so host stack return addresses are
  printed as `function+offset`. Offline vCPUs skip stack output.
- ARM64 host exception call traces resolve return addresses through the
  embedded SIMA symbol table and print `function+offset` beside the raw LR.
- VM vPL011 TX output from the currently selected `vsh` VM is written into a
  Xen-style per-VM async console ring buffer, using monotonic producer/consumer
  indexes and a 4KB power-of-two data area with 4095 bytes of usable capacity.
  The console timer path runs every 10ms and drains it to the host serial
  console in bounded batches, so guest PL011 writes no longer wait for host
  serial output. The ring is internal only; there is no shell command for
  changing console output mode or reading normal console-ring stats.
- Non-selected VM console output is not replayed into the SIMA shell.
- VM exception logs have a separate 4KB/4095-byte per-VM ring reserved for VM
  trap/oops capture. The ring is internal debug plumbing and is no longer
  exposed as a SIMA shell command.
- vUART/vPL011 layering:
  - `vuart` owns the upper VM console interface and host console integration.
  - `vpl011` is the ARM64 PL011 backend implementation.
  - Backend notification is routed through `vuart_notify_rx()` instead of direct
    console-to-vPL011 calls.
- ARM64 vPL011 vio for guest PL011 MMIO and RX interrupt notification.
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
- ARM64 IRQ domains for CPU-local and GIC interrupts.
- ARM64 memory logging for host stage-1 and VM stage-2 mappings.
- ARM64 exception stack dumps print directly to the console without per-line
  log prefixes.
- SIMA log and shell/console strings are lowercase source literals; there is no
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
- `vsh 2` enters the Linux console. During the 2026-06-16 VM2 debug session,
  4-vCPU Linux progressed through secondary CPU bring-up and reached
  `clou login:` in at least one run, but root-shell login is not yet stable.
- VM0 Zephyr `help` and VM1 LK `help` complete through the async VM console
  path. VM2 Linux root login and `help` are still part of the active
  timer/vGIC stability investigation.
- VM0 Zephyr `symtab list` completes through `vsh 0` and returns to the
  `zero ~>` prompt with async batched VM console output.
- VM console output bypasses vUART TX FIFO forwarding for the selected `vsh`
  VM console.
- Ctrl-D returns from VM console mode to `console:\>`.
- The `crash` command prints an ARM64 exception stack with `[cut here]` and
  `[end here]` markers, including a symbolized host call trace, then cold
  reboots SIMA automatically.
- Five repeated QEMU cold boots reached VM0/VM1/VM2 guest entry without
  `[cut here]` or `unexpected arm64 trap`, covering the prior `SP_EL2`/`SPSel`
  boot race.
- Five repeated QEMU cold boots also covered the later `vcpu_exit_return`
  restore-frame issue where EL2 `sp` could drift to guest RAM and fault at
  `far:0x80000000`.
- `vmap` shows VM0/VM1/VM2 stage-2 RAM identity mappings plus vGICD, vGICR,
  and vPL011 vio windows.
- Boot logs show each VM image copied to 1:1 RAM.
- `irqs` uses a narrow-screen-friendly format and shows the virtual timer PPI
  handler receiving counts on Zephyr AP pCPUs.
- Zephyr no longer traps on `GICD_IPRIORITYR` byte writes.
- Zephyr AP virtual timer interrupts no longer hit the host unexpected IRQ path.
- LK still boots with 4 CPUs after Zephyr became the service VM.
- The `reboot` shell command resets QEMU and restarts SIMA.
- `PLATFORM=rk356x` builds a hardware image. Boot correctness is pending manual
  flashing and serial-log validation.
- VM2 Linux no longer stops at `smp: Bringing up secondary CPUs ...` in the
  current 4-vCPU path: logs show CPU1, CPU2, and CPU3 entering EL1 and Linux
  reporting `smp: Brought up 1 node, 4 CPUs`.

## VM2 Linux Debug Snapshot

Status as of 2026-06-16:

- Development hygiene: avoid broad, low-signal local `rg` searches over large
  source trees. Prefer known relevant files in this repository and targeted
  reads from explicitly named external sources, such as `~/linux-7.1`.
- Latest cleaned validation:
  `SIMA_TOOLCHAINS=$HOME/sima-cc/bin ./scripts/regress.py --timeout 180
  --log out/qemu_out/regress-gate-cleaned.log`.
  VM0 Zephyr and VM1 LK passed the regression gate (`dumpstat 0`, `vsh 0`,
  and `vsh 1`). VM2 Linux still timed out waiting for `clou login:`.
- Keep VM2 Linux at 4 vCPUs; no single-vCPU fallback validation is being used.
- Keep Linux assets under `sdk/images/linux`; the active Linux DT source is
  `sdk/images/linux/sima-linux.dts`.
- `sdk/images/linux/sima-linux.dts` currently keeps the base Linux bootargs and
  adds an `initcall_blacklist` for ACPI/Xen/TPM/ATA initcalls that are not
  useful on the SIMA virtual platform. Do not re-add earlier diagnostic
  bootargs such as `nokaslr`, `cma=0`, or
  `clocksource.arm_arch_timer.evtstrm=0`.
- Timer DT interrupt IDs remain unchanged. The virtual timer still uses
  PPI/INTID 27 from `<1 13 4>`; current evidence does not require changing
  PPI27.
- WFI/WFE trapping is kept disabled by default. Reference checks:
  `~/nebula/bloc/raan/croc` traps WFE as yield and WFI as block, while
  `~/nebula/bloc/raan/anoa` traps WFE as yield and uses an IRQ-wait path for
  WFI with a yield threshold plus timeout. In SIMA QEMU, enabling TWI/TWE makes
  VM0/VM1/VM2 noticeably slower, so the default relies on hardware/vGIC wakeup.
- A clean QEMU run after restoring native WFI/WFE reached the VM2 PL011 console
  handoff at about Linux timestamp `1.83s`; trapped WFI/WFE variants pushed the
  same area to about `31s` or later. VM0/VM1 still pass the regression gate.
  VM2 Linux login timeout remains a separate post-console issue.
- vtimer experiment `regress-vtimer-backup.log`: arming a software backup timer
  on every vCPU unload, modeled after anoa's full generic-timer context path,
  regressed VM2 heavily (`smp: Brought up` around `46.42s`, PL011 console around
  `52.19s`). Do not keep this in the QEMU 3OS path.
- vtimer experiment `regress-vtimer-line-resample.log`: resampling the live CNTV
  line when a timer LR is completed keeps early boot speed acceptable (`smp:
  Brought up` around `0.29s`, PL011 console around `2.07s`) and VM0/VM1 pass,
  but VM2 still times out before login. The timeout snapshot still shows an
  expired vtimer deadline on VM2/vCPU0 with host PPI27 enabled, so the next
  timer work should focus on active/pending LR lifecycle rather than WFI/WFE or
  broad backup timers.
- Linux CPU1/CPU2/CPU3 reach the guest secondary entry path. The remaining VM2
  instability is now after secondary CPU bring-up and before a stable root
  shell.
- The useful VM2 progress marker is that `vsh 2` can show Linux reaching
  `clou login:`. In the same run, delayed input allowed BusyBox login to time
  out and Linux later printed RCU stall diagnostics, so this is not yet a
  completed `root` / `root` / `help` validation.
- The latest cleaned run does not reach PL011 login before timeout. Earlier
  runs reached `console [ttyAMA0] enabled`; in both cases the VM2 diagnostic
  state remains centered on virtual timer PPI27 rather than a proven PL011
  interrupt issue.
- `dumpstat 2` in the bad state repeatedly points at the virtual timer
  lifecycle on CPU0:
  - LR0 can remain as pending-only virtual INTID 27, for example
    `0x508000000000001b`.
  - The software vGIC descriptor for virq 27 can be `pending:no active:no
    level:yes`.
  - In the latest cleaned run, host PPI27 is enabled/unmasked and CNTV is still
    expired on vCPU0, but the pending-only LR remains.
  - Linux reports `Possible timer handling issue on cpu=0 timer-softirq=0`
    when the stall reproduces.
- `dumpstat` should remain focused on vCPU/timer/vGIC state and must not grow
  vPL011 statistics again.

Experiments already tried:

- Marking pending-only software timer LRs with EOI caused maintenance storms
  and broke AP bring-up. Do not repeat that change.
- Keeping host PPI27 globally unmasked in the virtual timer handler or poll
  path regressed secondary CPU bring-up. Do not repeat that broad change.
- Dropping stale pending-only timer LRs from the general vGIC sync path also
  regressed secondary CPU bring-up.
- Removing the WFI `DAIF.I` guard did not solve the post-console stall and was
  reverted.
- Re-arming the host virtual-timer PPI only from the WFI timer-LR cleanup path
  is conservative and allowed VM2 to reach `clou login:` in one run, but it did
  not eliminate the CPU0 timer-softirq stall.
- Extending that re-arm into the general vGIC sync path regressed AP bring-up
  and was reverted.
- Rewriting the WFI pending-only timer LR as Active+Pending made forward
  progress worse and was reverted.
- Removing WFI/WFE traps and letting hardware wake guest idle, as in the
  reference `prot` path, keeps VM0/VM1 passing and speeds VM2 boot, but does
  not fix the VM2 login timeout by itself.
- Re-enabling WFI/WFE traps with WFI as a pure yield slowed VM2 badly and could
  leave host PPI27 masked. Reworking WFI to block, based on the croc/anoa
  direction, still slowed the QEMU 3OS scenario and affected VM0/VM1 as well.
  Keep HCR_EL2.TWI/TWE clear unless a specific diagnostic run requires trapped
  idle instructions.
- Porting anoa's full switch-out backup timer model directly to SIMA caused a
  large VM2 slowdown. The useful part to keep is local deadline resampling at
  LR completion/EOI boundaries; avoid arming a backup timer on every vCPU
  unload until the GICv3 LR lifecycle is simpler.
- Removing the host PPI27 mask from the timer poll path kept VM0/VM1 passing
  but did not fix VM2 and raised host virtual-timer IRQ counts, so it was
  reverted.
- Expanding the generated QEMU vFDT PL011 clocks to include both `uartclk` and
  `apb_pclk` kept VM0/VM1 passing but did not affect VM2. VM2 uses the embedded
  `sdk/images/linux/sima-linux.dtb`, whose DTS already has both clocks, so the
  generated-vFDT experiment was reverted.
- The current in-tree timer completion change removes a completed active timer
  LR and re-enables the host PPI so the hardware level source can create a
  fresh virtual timer injection instead of preserving stale active state. It
  keeps VM0/VM1 passing, but VM2 still times out before stable login.

Recommended resume point:

1. Before any VM2 experiment, run the full regression and require VM0/VM1 to
   pass (`dumpstat 0`, `vsh 0`, `vsh 1`).
2. Start from the current `arch/arm64/guest/vgicv3.c` timer completion change
   with WFI/WFE trapping disabled in HCR_EL2.
3. Confirm VM2 CPU1/CPU2/CPU3 still boot.
4. Switch with `vsh 2`, wait for `clou login:`, then immediately input
   `root`, password `root`, and run `help`.
5. If Linux stalls before login or emits RCU timer warnings, return with Ctrl-D
   and capture `dumpstat 2`, `vcpus`, `schedstat`, and `irqs`.
6. Treat a VM2 fix as complete only after `root` / `root` / `help` succeeds
   without RCU timer-softirq warnings while VM0 and VM1 remain responsive.

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

- vGICv3 is sufficient for the current Zephyr, LK, and 4-vCPU VM2 Linux
  boot path, but it is not a complete GICv3 model yet. Active-state stress
  cases, deeper redistributor behavior, MSI/LPI paths, and richer distributor
  coverage still need work.
- QEMU VM layout is still statically configured in `vm_config.c`;
  QEMU FDT parsing is not yet used to derive VM layout.
- Zephyr and LK are RTOS raw images and boot with `GUEST_FLAG_NO_FW`. VM2 Linux
  uses a loader/module path for `Image` and `Initrd`, while its Linux-on-SIMA
  DTB remains embedded.
- VM2 Linux runs as a 4-vCPU guest. The path has reached the Linux login prompt
  during QEMU validation; repeated cold-boot coverage is still needed before
  calling the 4-vCPU path stable.
- VM2 Linux keeps `earlycon=pl011,0x09000000` so `vsh 2` can show Linux logs
  before the normal PL011 console driver is registered.
- VM console output from SMP guests can interleave because multiple guest CPUs
  write concurrently to the same PL011 console.
- Stage-2 mapping is still static for the QEMU `virt` platform.
- The automated regression harness covers the core multi-VM boot sequence, but
  broader stress and overflow coverage is still pending.

## Next Steps

1. Add repeated cold-boot and root-shell coverage for VM2 Linux 4-vCPU/SMP,
   with diagnostics for timer, SGI, and pending/active LR state on failures.
2. Harden PL011 RX/TX interrupt behavior for Linux after the real console
   driver takes over.
3. Extend `scripts/regress.py` with more VM2 Linux root-shell commands, reboot
   coverage, repeated cold boots, and saved log artifacts suitable for CI.
4. Move QEMU platform memory and device discovery toward host-FDT-derived data
   where it helps reduce static board assumptions.
5. Bring up rk356x hardware manually, then capture the validated RAM, UART,
   GIC, and boot-image placement assumptions back into the platform files.
