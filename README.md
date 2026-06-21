# BEAU HYPERVISOR

```text
_____________________   _____   ____ ___  ________    _________
\______   \_   _____/  /  _  \ |    |   \ \_____  \  /   _____/
 |    |  _/|    __)_  /  /_\  \|    |   /  /   |   \ \_____  \
 |    |   \|        \/    |    \    |  /  /    |    \/        \
 |______  /_______  /\____|__  /______/   \_______  /_______  /
        \/        \/         \/                   \/        \/ (2026)
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
