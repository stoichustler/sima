# BEAU HYPERVISOR

```text
888 88b, 888'Y88     e Y8b     8888 8888     e88 88e    dP"8
888 88P' 888 ,'Y    d8b Y8b    8888 8888    d888 888b  C8b Y
888 8K   888C8     d888b Y8b   8888 8888   C8888 8888D  Y8b
888 88b, 888 ",d  d888888888b  8888 8888    Y888 888P  b Y8D
888 88P' 888,d88 d8888888b Y8b 'Y88 88P'     "88 88"   8edP (2026)
```

## Introduction

BEAU is a compact ARM64 hypervisor bring-up project for QEMU `virt` and rk356x
hardware. It runs at EL2 and focuses on a small, readable virtualization base
for mixed RTOS and Linux guests.

```text
┌──────────────────────────────┐
│ BEAU Hypervisor · ARM64 EL2  │
├──────────────┬───────────────┤
│ VM0 Zephyr   │ service VM    │
│ VM1 LK       │ prelaunch VM  │
│ VM2 Linux    │ prelaunch VM  │
└──────────────┴───────────────┘
```

## Source Base And License

BEAU is based on the Project ACRN hypervisor source tree and keeps ACRN-derived
code under the BSD 3-Clause License.

The ARM64 QEMU porting work is non-Intel work and is marked as
`Copyright (C) 2026 Hustler Lo.` under the same BSD 3-Clause License. Third
party components retain their original notices; see `NOTICE.md` and `LICENSES/`.

License-risk summary: no GPL-only source was found. The notable item is libfdt,
which is dual-licensed `GPL-2.0-or-later OR BSD-2-Clause`; use the BSD-2-Clause
option when distributing BEAU to avoid GPL copyleft obligations.

---

Hustle Embedded OS.
