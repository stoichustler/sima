#!/usr/bin/env python3
import argparse
import os
import shlex
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CWD = Path.cwd()


def relpath(path):
    path = Path(path).expanduser()
    return path if path.is_absolute() else CWD / path


def render(cmd, toolchains=None):
    cmd = shlex.join([str(arg) for arg in cmd])
    return f"PATH={shlex.quote(str(toolchains))}:$PATH {cmd}" if toolchains else cmd


def parse_args():
    kernel = relpath(os.environ["CLAN_KERNEL"]) if "CLAN_KERNEL" in os.environ else ROOT / "build/clan.debug.out"
    toolchains = os.getenv("CLAN_TOOLCHAINS", os.getenv("CLAN_TOOLCHAIN"))
    toolchains = relpath(toolchains) if toolchains else None

    parser = argparse.ArgumentParser(description="Build and launch the ARM64 QEMU image.")
    parser.add_argument("-k", "--kernel", default=kernel, type=relpath)
    parser.add_argument("--qemu", default=os.getenv("QEMU_SYSTEM_AARCH64", "qemu-system-aarch64"))
    parser.add_argument("--smp", default=os.getenv("CLAN_QEMU_SMP", "8"))
    parser.add_argument("-m", "--memory", default=os.getenv("CLAN_QEMU_MEM", "1024M"))
    parser.add_argument("--toolchains", "--toolchain", default=toolchains, type=relpath)
    parser.add_argument("--cross-prefix", default=os.getenv("CLAN_CROSS_COMPILE", "aarch64-none-elf-"))
    parser.add_argument("--build", action="store_true")
    parser.add_argument("-n", "--dry-run", action="store_true")
    args, extra = parser.parse_known_args()
    if extra[:1] == ["--"]:
        extra = extra[1:]
    args.extra = extra
    return args


def main():
    args = parse_args()
    env = os.environ.copy()
    if args.toolchains:
        env["PATH"] = f"{args.toolchains}{os.pathsep}{env.get('PATH', '')}"

    build_cmd = [
        "make",
        "ARCH=arm64",
        "PLATFORM=qemu",
        f"CROSS_COMPILE={args.cross_prefix}",
        f"-j{os.cpu_count() or 1}",
    ]
    qemu_cmd = [
        args.qemu,
        "-machine",
        "virt,virtualization=on,gic-version=3",
        "-cpu",
        "cortex-a57",
        "-smp",
        args.smp,
        "-m",
        args.memory,
        "-nographic",
        "-serial",
        "mon:stdio",
        "-kernel",
        str(args.kernel),
        *args.extra,
    ]

    if args.dry_run:
        if args.build:
            print(render(build_cmd, args.toolchains))
        print(render(qemu_cmd))
        return

    if args.build:
        if args.toolchains and not args.toolchains.is_dir():
            raise SystemExit(f"Toolchain bin dir not found: {args.toolchains}")
        compiler = f"{args.cross_prefix}gcc"
        if shutil.which(compiler, path=env.get("PATH")) is None:
            raise SystemExit(f"Compiler not found: {compiler}")
        subprocess.run(build_cmd, cwd=ROOT, env=env, check=True)

    if not args.kernel.is_file():
        print(f"Kernel image not found: {args.kernel}")
        print("Build it with:")
        print(f"  {render(build_cmd, args.toolchains)}")
        raise SystemExit(1)

    qemu = shutil.which(args.qemu)
    if qemu is None:
        raise SystemExit(f"QEMU binary not found: {args.qemu}")
    qemu_cmd[0] = qemu
    os.chdir(ROOT)
    os.execvp(qemu, qemu_cmd)


if __name__ == "__main__":
    main()
