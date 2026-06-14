---
name: clan-hypervisor-dev
description: Develop, debug, validate, and document the CLAN hypervisor in this repository, especially the ARM64 QEMU virt bring-up, static VM configuration, stage-2 identity mappings, vCPU scheduling/shared-pCPU behavior, vGIC/vPL011/vUART console paths, hypercalls, shell commands, and SDK documentation. Use when Codex is asked to modify CLAN hypervisor code, investigate VM boot/runtime issues, add observability commands, update ARM64 virtualization logic, or run QEMU/build validation for this tree.
---

# CLAN Hypervisor Development

## Core Workflow

1. Read local context first:
   - Start with `sdk/sdk.md` for current architecture, verified behavior, and limitations.
   - Read only the relevant source files after that; use `rg` before broad file reads.
   - Treat the repository as possibly dirty. Do not revert unrelated changes.

2. Keep the ARM64 QEMU bring-up simple:
   - Preserve static VM configuration unless the user explicitly asks for dynamic policy.
   - Preserve QEMU RTOS stage-2 RAM identity mapping: guest IPA/GPA equals HPA/PA.
   - Keep Zephyr/LK RTOS boot paths independent of external ACPI/FDT modules.
   - Do not move future Linux boot support into `.incbin`; use loader/module design instead.

3. Make focused changes:
   - Follow existing module boundaries: `arch/arm64` for architecture logic, `core` for common scheduler/vCPU/VM logic, `sdk/debug` for shell and console tools, `sdk/boot` for boot loaders.
   - Add English comments for design intent, architecture invariants, ownership, and failure modes.
   - Avoid comments that restate assignments or obvious branches.

4. Validate before final response:
   - Build ARM64 QEMU:
     ```bash
     ./scripts/kick.py --toolchains /path/to/clan-arm64-none-elf/bin --build --dry-run
     ./scripts/kick.py --toolchains /path/to/clan-arm64-none-elf/bin --build
     ```
   - For debug/shell/console changes, force rebuild stale debug artifacts first:
     ```bash
     rm -f build/modules/libdebug.a build/clan.out build/clan.debug.out build/clan.debug.bin
     ```
   - Check QEMU command:
     ```bash
     ./scripts/kick.py --dry-run
     ```
   - Run the boot regression when touching boot, scheduler, IRQ, memory-map, or
     console behavior:
     ```bash
     ./scripts/regress.py --toolchains /path/to/clan-arm64-none-elf/bin
     ```
   - Run `./scripts/kick.py` for boot/runtime validation when behavior changes.

## Common Tasks

### VM Boot Or Bring-Up Bugs

- Inspect `arch/arm64/platform/qemu/vm_configurations.c`, `arch/arm64/guest/vm.c`, `sdk/boot/guest/vboot_info.c`, and `sdk/boot/guest/rawimage_loader.c`.
- Verify VM RAM windows, load/entry addresses, `GUEST_FLAG_NO_FW`, static vFDT placement, and stage-2 identity logs.
- In QEMU, check `vcpus`, `schedstat`, `vsh 0`, and `vsh 1`.

### Shared pCPU Scheduling

- Inspect `core/vm.c`, `core/schedule.c`, `core/sched_iorr.c`, and ARM64 vCPU context switch files.
- Keep VM0 on ordinary pCPUs and allow VM1 to mix ordinary/performance pCPUs.
- Ensure shared pCPUs are time-sliced by scheduler behavior, not by adding ad hoc VM placement hacks.
- Use `schedstat` to compare scheduler `timer`, `resched`, and `switches`.

### Console Or Shell Work

- Inspect `sdk/debug/shell.c`, `sdk/debug/shell_priv.h`, `sdk/debug/console.c`, and `arch/arm64/guest/vpl011.c`.
- For `vsh`, make ownership markers appear before replaying buffered VM output.
- Remember selected and non-selected VM console output use per-VM async rings.
- Keep shell observability fields actionable for live diagnosis. Do not add
  cumulative time totals or averaged-from-total latency fields to shell command
  output by default; prefer current, recent, and high-water values such as
  `since.us`, `lastwait.us`, and `maxwait.us`. Interrupt counts and scheduler
  or vCPU `switches` are allowed as event-count exceptions.

### Hypercall Work

- Inspect `arch/arm64/guest/hypercall.c`, `arch/arm64/guest/vcpu_exit.c`, and common hypercall headers.
- Keep ARM64 hypercall dispatch explicit and small.
- Return `-ENOTTY` for x86-only or unimplemented calls until their ARM64 dependencies exist.

## References

Read these only when needed:

- `references/architecture.md`: current ARM64 QEMU architecture, invariants, commands, and validation checklist.
