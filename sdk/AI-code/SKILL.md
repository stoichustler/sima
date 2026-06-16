---
name: sima-hypervisor-dev
description: Develop, debug, validate, and document the SIMA hypervisor in this repository, especially the ARM64 QEMU virt and rk356x bring-up, static VM configuration, stage-2 identity mappings, vCPU scheduling/shared-pCPU behavior, vGIC/vPL011/vUART console paths, hypercalls, shell commands, and SDK documentation. Use when Codex is asked to modify SIMA hypervisor code, investigate VM boot/runtime issues, add observability commands, update ARM64 virtualization logic, or run QEMU/build validation for this tree.
---

# SIMA Hypervisor Development

## Core Workflow

1. Read local context first:
   - Start with `sdk/sdk.md` for current architecture, verified behavior, and limitations.
   - Read only the relevant source files after that; use `rg` before broad file reads.
   - Treat the repository as possibly dirty. Do not revert unrelated changes.
   - Before making any file modification, explicitly notify the user in the
     conversation which files or areas will be changed and why. Do not edit
     silently.
   - The project skill can be combined with other available skills when they
     are explicitly requested or their trigger rules apply; `superpowers` is
     available and not excluded from this repository workflow.

2. Keep ARM64 platform bring-up simple:
   - Preserve static VM configuration unless the user explicitly asks for dynamic policy.
   - Preserve QEMU RTOS stage-2 RAM identity mapping: guest IPA/GPA equals HPA/PA.
   - Keep Zephyr/LK RTOS boot paths independent of external ACPI/FDT modules.
   - Keep LK and Zephyr image inputs under `sdk/images`, and keep Linux
     `Image`, `Initrd`, `sima-linux.dts`, and `sima-linux.dtb` under
     `sdk/images/linux`.
   - Do not move Linux `Image` or `Initrd` into `.incbin`; use loader/module
     delivery. The Linux-on-SIMA DTB may remain an embedded module.

3. Make focused changes:
   - Follow existing module boundaries: `arch/arm64` for architecture logic, `core` for common scheduler/vCPU/VM logic, `sdk/debug` for shell and console tools, `sdk/boot` for boot loaders.
   - Newly added code must include succinct English comments that explain the
     underlying principle and the purpose of the code, especially around boot,
     interrupt, timer, scheduler, vCPU, and console behavior.
   - Add English comments for design intent, architecture invariants, ownership, and failure modes.
   - Avoid comments that restate assignments or obvious branches.
   - Do not write machine-local directory or file paths into documentation,
     code design notes, or this skill. Use variables such as `SIMA_TOOLCHAINS`
     or repository-relative paths instead.

4. Validate before final response:
   - Before any build or regression, confirm `SIMA_TOOLCHAINS` is set and
     points to a valid bare-metal toolchain bin directory.
   - Build ARM64 QEMU:
     ```bash
     ./scripts/kick.py --build --dry-run
     ./scripts/kick.py --build
     ```
   - Build rk356x when changing platform selection, common ARM64 platform
     guards, guest image embedding, or rk356x files:
     ```bash
     PATH=${SIMA_TOOLCHAINS}:$PATH \
     make ARCH=arm64 PLATFORM=rk356x CROSS_COMPILE=aarch64-none-elf- -j$(nproc)
     ```
   - Expect platform-scoped default output directories:
     `out/qemu_out` for QEMU and `out/rk356x_out` for rk356x.
   - For debug/shell/console changes, force rebuild stale debug artifacts first:
     ```bash
     rm -f out/qemu_out/modules/libdebug.a out/qemu_out/sima.out out/qemu_out/sima.debug.out out/qemu_out/sima.debug.bin
     ```
   - Check QEMU command:
     ```bash
     ./scripts/kick.py --dry-run
     ```
   - Run the boot regression when touching boot, scheduler, IRQ, memory-map, or
     console behavior:
     ```bash
     ./scripts/regress.py
     ```
   - Run `./scripts/kick.py` for boot/runtime validation when behavior changes.

## Common Tasks

### VM Boot Or Bring-Up Bugs

- Inspect `arch/arm64/platform/<platform>/vm_config.c`, `arch/arm64/guest/vm.c`, `sdk/boot/guest/vboot_info.c`, and `sdk/boot/guest/rawimage_loader.c`.
- Verify VM RAM windows, load/entry addresses, `GUEST_FLAG_NO_FW`, static vFDT placement, and stage-2 identity logs.
- In QEMU, check `vcpus`, `schedstat`, `vsh 0`, `vsh 1`, and VM2 Linux
  `root` / `root` login through `vsh 2`.

### rk356x Platform Work

- Keep rk356x-specific RAM, GIC, UART, and VM layout data under
  `arch/arm64/platform/rk356x`.
- Keep guest image embedding pointed at `sdk/images/lk.bin`,
  `sdk/images/zephyr.bin`, and small DTB modules. Keep Linux `Image` and
  `Initrd` under `sdk/images` but load them through module/loader staging.
- Treat rk356x hardware validation as manual flashing and serial-log inspection
  until an automated board workflow exists.
- Do not add board flashing commands or local artifact paths to documentation
  or skills.

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

- `references/architecture.md`: current ARM64 QEMU/rk356x architecture,
  invariants, commands, and validation checklist.
