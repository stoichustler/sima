#!/usr/bin/env python3
import argparse
import os
import shlex
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CWD = Path.cwd()
LINUX_IMAGE_STAGE_ADDR = "0x70000000"
LINUX_INITRAMFS_STAGE_ADDR = "0x74000000"


def relpath(path):
    path = Path(path).expanduser()
    return path if path.is_absolute() else CWD / path


def render(cmd, toolchains=None):
    cmd = shlex.join([str(arg) for arg in cmd])
    return f"PATH={shlex.quote(str(toolchains))}:$PATH {cmd}" if toolchains else cmd


def getenv(name, default=None):
    value = os.getenv(name)
    return default if value is None else value


def parse_args():
    kernel_env = getenv("BEAU_KERNEL")
    kernel = relpath(kernel_env) if kernel_env else ROOT / "out/qemu_out/beau.debug.out"
    toolchains = getenv("BEAU_TOOLCHAINS")
    toolchains = getenv("BEAU_TOOLCHAIN", toolchains)
    toolchains = relpath(toolchains) if toolchains else None

    parser = argparse.ArgumentParser(description="Build and launch the ARM64 QEMU image.")
    parser.add_argument("-k", "--kernel", default=kernel, type=relpath)
    parser.add_argument("--qemu", default=os.getenv("QEMU_SYSTEM_AARCH64", "qemu-system-aarch64"))
    parser.add_argument("--smp", default=getenv("BEAU_QEMU_SMP", "8"))
    parser.add_argument("-m", "--memory", default=getenv("BEAU_QEMU_MEM", "1024M"))
    parser.add_argument("--linux-image", default=ROOT / "sdk/image/linux/Image", type=relpath)
    parser.add_argument(
        "--linux-initramfs",
        dest="linux_initramfs",
        default=ROOT / "sdk/image/linux/Initramfs.cpio.gz",
        type=relpath,
    )
    parser.add_argument("--toolchains", "--toolchain", default=toolchains, type=relpath)
    parser.add_argument("--cross-prefix", default=getenv("BEAU_CROSS_COMPILE", "aarch64-none-elf-"))
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
    clean_cmd = [
        "make",
        "ARCH=arm64",
        "PLATFORM=qemu",
        f"CROSS_COMPILE={args.cross_prefix}",
        "clean",
    ]
    qemu_cmd = [
        args.qemu,
        "-machine",
        "virt,virtualization=on,gic-version=3,its=on",
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
        "-device",
        f"loader,file={args.linux_image},addr={LINUX_IMAGE_STAGE_ADDR},force-raw=on",
        "-device",
        f"loader,file={args.linux_initramfs},addr={LINUX_INITRAMFS_STAGE_ADDR},force-raw=on",
        *args.extra,
    ]

    if args.dry_run:
        if args.build:
            print(render(clean_cmd, args.toolchains))
            print(render(build_cmd, args.toolchains))
        print(render(qemu_cmd))
        return

    if args.build:
        if args.toolchains and not args.toolchains.is_dir():
            raise SystemExit(f"Toolchain bin dir not found: {args.toolchains}")
        compiler = f"{args.cross_prefix}gcc"
        if shutil.which(compiler, path=env.get("PATH")) is None:
            raise SystemExit(f"Compiler not found: {compiler}")
        subprocess.run(clean_cmd, cwd=ROOT, env=env, check=True)
        subprocess.run(build_cmd, cwd=ROOT, env=env, check=True)

    if not args.kernel.is_file():
        print(f"Kernel image not found: {args.kernel}")
        print("Build it with:")
        print(f"  {render(build_cmd, args.toolchains)}")
        raise SystemExit(1)
    if not args.linux_image.is_file():
        raise SystemExit(f"Linux Image not found: {args.linux_image}")
    if not args.linux_initramfs.is_file():
        raise SystemExit(f"Linux initramfs not found: {args.linux_initramfs}")

    qemu = shutil.which(args.qemu)
    if qemu is None:
        raise SystemExit(f"QEMU binary not found: {args.qemu}")
    qemu_cmd[0] = qemu
    os.chdir(ROOT)
    os.execvp(qemu, qemu_cmd)


if __name__ == "__main__":
    main()
