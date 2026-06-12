## ARM64 Development Status

The ARM64 bring-up currently targets `qemu-system-aarch64` with the `virt`
machine, GICv3, EL2 virtualization enabled, and 8 physical CPUs. Use the
wrapper script for the default QEMU launch:

```bash
./scripts/kick.sh
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
  -kernel build/acrn.out
```

Build with the local bare-metal toolchain:

```bash
PATH=/home/beau/clan-arm64-none-elf/bin:$PATH \
make ARCH=arm64 PLATFORM=qemu CROSS_COMPILE=aarch64-none-elf-
```

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
- pCPU0-pCPU5 model ordinary cores.
- pCPU6-pCPU7 model performance cores.
- VM0 uses ordinary cores only. VM1 may mix ordinary and performance cores; the
  static QEMU scenario uses pCPU3 as the shared ordinary core plus pCPU5-pCPU7.

The generated `build/acrn.out` has been boot-tested on QEMU. Both RTOS guests
autostart without external ACPI/FDT modules. The CLAN shell stays quiet during
late guest AP bring-up; press Enter after the boot logs settle to show the
`console:\>` prompt.

## Implemented

- ARM64 build path for the QEMU `virt` platform.
- `scripts/kick.sh` QEMU launcher with `--kernel`, `--qemu`, `--smp`, `--memory`,
  `--dry-run`, and extra QEMU argument support.
- QEMU platform code and static board/scenario configuration under
  `arch/arm64/platform/qemu`.
- Bare-boot image embedding for both LK and Zephyr raw images.
- Static QEMU VM configuration for Zephyr as the service VM and LK as a
  pre-launched VM.
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
  not require external ACPI/FDT modules for the current RTOS images. Static
  ARM64 raw images still receive the synthetic QEMU vFDT boot ABI. Linux
  bring-up should clear that flag and use a loader/module path instead of
  platform `.incbin` image embedding.
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
- CLAN shell `reboot` command wired to host PSCI system reset.
- CLAN shell `crash` command for intentionally triggering an ARM64 host data
  abort, printing the exception stack, and cold rebooting automatically.
- PSCI-based host secondary CPU bring-up with `MAX_PCPU_NUM=8`.
- VM0 and VM1 vCPUs share pCPU3 through the existing `sched_iorr` scheduler.
  VM0 uses ordinary-core pCPU0, pCPU2, pCPU3, and pCPU4; VM1 uses mixed pCPU3,
  pCPU5, pCPU6, and pCPU7.
- ARM64 vCPU switch-in/out now saves and restores guest EL1 translation,
  exception, timer, TPIDR, and vGIC state, so two VMs can time-share one pCPU
  without inheriting each other's EL1 address-space context.
- ARM64 local physical timer setup enables the scheduler tick PPI on every pCPU.
  Guest timer state is kept on the virtual timer path so guest timer activity
  does not overwrite the host scheduler's physical timer deadline.
- Shell running as a scheduler thread and VM launch gated to host BSP after all
  APs are running.
- ARM64 HVC dispatch recognizes ACRN hypercall IDs separately from PSCI HVC
  calls. The current ARM64 implementation supports basic API/HW info and
  GPA-to-HPA translation; x86-specific VM/device management hypercalls return
  not-supported until their ARM64 dependencies exist.
- VM and vCPU debug commands:
  - `vcpus`
  - `threads`
  - `sched`
  - `vmap`
  - `irqs`
  - `vdump [vm id] [vcpu id]`
  - `vsh <vm id>`
  - `bench [loops] [mem_kb]`
- `vsh <vm id>` switches the serial console to a VM vPL011/vUART console.
  Ctrl-D switches back to the CLAN shell.
- `sched` prints the scheduler name, scheduler tick count, context switch
  count, current thread, and thread state for each pCPU.
  - `ticks` is the number of scheduler timer callbacks observed on that pCPU.
    It is a periodic opportunity to charge runtime and request rescheduling.
  - `ctx_switches` is the number of times `schedule()` actually selected a
    different thread. A tick can happen without a context switch, and a context
    switch can also be caused by events such as wake/sleep/yield paths.
- `irqs` prints a short IRQ name/purpose column when the architecture can decode
  it. ARM64 maps ACRN IRQ numbers back to GIC SGI/PPI/SPI sources and names the
  EL2-owned SMP-call, physical-timer, virtual-timer, and vGIC-maintenance
  interrupts. Per-pCPU counts use aligned `cpuN:count` fields, and `active`
  shows whether the IRQ is allocated in the common IRQ table.
- `bench` runs a bounded CLAN shell CPU/memory stress loop. `loops` controls
  the stress loop count, and `mem_kb` controls how much of the static scratch
  buffer is touched per loop. The command prints elapsed ticks/time, loop
  throughput, current per-pCPU thread occupancy, and visible memory occupancy.
- VM vPL011 TX output from the currently selected `vsh` VM is written into a
  Xen-style per-VM async console ring buffer, using monotonic producer/consumer
  indexes and a 4KB power-of-two data area with 4095 bytes of usable capacity.
  The console timer path runs every 10ms and drains it to the host serial
  console in bounded batches, so guest PL011 writes no longer wait for host
  serial output. The ring is internal only; there is no shell command for
  changing console output mode or reading normal console-ring stats.
- Non-selected VM console output is not replayed into the CLAN shell.
- VM exception logs have a separate 4KB/4095-byte per-VM ring reserved for VM
  trap/oops capture. The ring is internal debug plumbing and is no longer
  exposed as a CLAN shell command.
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
  - GICD/GICR `IPRIORITYR` byte/halfword/word access support.
  - Virtual timer PPI handling and injection back into the running guest vCPU.
- ARM64 IRQ domains for CPU-local and GIC interrupts.
- ARM64 memory logging for host stage-1 and VM stage-2 mappings.
- ARM64 exception stack dumps print directly to the console without per-line
  log prefixes.
- CLAN log and shell/console strings are lowercase source literals; there is no
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
- VM0 Zephyr vCPU0-vCPU3 bind to ordinary-core pCPU2, pCPU0, pCPU3, and pCPU4.
- VM1 LK vCPU0-vCPU3 bind to mixed pCPU5, pCPU3, pCPU6, and pCPU7, sharing
  pCPU3 with VM0.
- VM0 Zephyr enters EL1 at `0x42000000`.
- VM1 LK enters EL1 at `0x40100000`.
- Boot logs show each VM stage-2 RAM map as identity mapped:
  VM0 `ipa[0x42000000-0x48000000]:pa[0x42000000-0x48000000]` and VM1
  `ipa[0x40000000-0x42000000]:pa[0x40000000-0x42000000]`.
- `sched` reports `sched_iorr`, per-pCPU scheduler ticks, and per-pCPU context
  switch counters.
- `vcpus` reports both VMs and all 8 guest vCPUs. VM0 uses pCPU0, pCPU2,
  pCPU3, and pCPU4; VM1 uses pCPU3, pCPU5, pCPU6, and pCPU7.
- `vsh 0` enters the Zephyr console and reaches `zero ~>`.
- `vsh 1` enters the LK console and reaches `beau ~>`.
- VM0 Zephyr `help` and VM1 LK `help` both complete through the async VM
  console path.
- VM0 Zephyr `symtab list` completes through `vsh 0` and returns to the
  `zero ~>` prompt with async batched VM console output.
- VM console output bypasses vUART TX FIFO forwarding for the selected `vsh`
  VM console.
- `bench 0 0` prints CPU and memory occupancy without stress, and
  `bench 100 4` runs a bounded CPU/memory stress loop and reports touched
  memory and loop throughput.
- Ctrl-D returns from VM console mode to `console:\>`.
- The `crash` command prints an ARM64 exception stack with `[cut here]` and
  `[end here]` markers, then cold reboots CLAN automatically.
- Five repeated QEMU cold boots reached VM0/VM1 AP vCPU guest entry without
  `[cut here]` or `unexpected arm64 trap`, covering the prior `SP_EL2`/`SPSel`
  boot race.
- Five repeated QEMU cold boots also covered the later `vcpu_exit_return`
  restore-frame issue where EL2 `sp` could drift to guest RAM and fault at
  `far:0x80000000`.
- `vmap` shows VM0/VM1 stage-2 RAM identity mappings plus vGICD, vGICR, and
  vPL011 vio windows.
- Boot logs show each VM image copied to 1:1 RAM.
- `irqs` uses a narrow-screen-friendly format and shows the virtual timer PPI
  handler receiving counts on Zephyr AP pCPUs.
- Zephyr no longer traps on `GICD_IPRIORITYR` byte writes.
- Zephyr AP virtual timer interrupts no longer hit the host unexpected IRQ path.
- LK still boots with 4 CPUs after Zephyr became the service VM.
- The `reboot` shell command resets QEMU and restarts CLAN.

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

The ARM64 port is now capable of booting the current Zephyr and LK raw images on
QEMU, but it is still a bring-up target rather than a complete architectural
virtualization port.

- vGICv3 is sufficient for the current Zephyr and LK boot path, but it is not a
  complete GICv3 model yet. Routing, active-state edge cases, full redistributor
  behavior, and full distributor register coverage still need work.
- QEMU VM layout is still statically configured in `vm_configurations.c`;
  QEMU FDT parsing is not yet used to derive VM layout.
- The current Zephyr and LK VMs are RTOS raw images and boot with
  `GUEST_FLAG_NO_FW`; Linux boot support still needs a non-`.incbin` image
  delivery path plus FDT/ACPI handoff.
- VM console output from SMP guests can interleave because multiple guest CPUs
  write concurrently to the same PL011 console.
- Stage-2 mapping is still static for the QEMU `virt` platform.
- No automated regression harness exists yet for the multi-VM boot sequence.

## Next Steps

1. Add a Linux-oriented loader/module path that does not rely on platform
   `.incbin` image embedding, then clear `GUEST_FLAG_NO_FW` for Linux guests.
2. Extend vGICv3 coverage for routing, active-state handling, redistributor SGI
   and PPI registers, and remaining distributor registers used by richer guests.
3. Add a repeatable boot regression script that checks build, `scripts/kick.sh`
   launch, VM states, Zephyr `zero ~>`, LK `beau ~>`, `vmap`, and `irqs`.
4. Move QEMU platform memory and device discovery toward host-FDT-derived data
   where it helps reduce static board assumptions.
5. Add a repeatable automated console regression for async per-VM output,
   Ctrl-D switching, and long-output overflow behavior.
