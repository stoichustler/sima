#!/usr/bin/env python3
import argparse
import codecs
import os
import selectors
import shlex
import shutil
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CWD = Path.cwd()
PROMPT = "console:\\>"
ENTER = "\r"
CTRL_D = b"\x04"
FATAL_PATTERNS = (
    "[cut here]",
    "unexpected arm64 trap",
    "unexpected irq",
    "unhandled arm64 vcpu exit",
    "failed to handle arm64 vcpu exit",
)


def relpath(path):
    path = Path(path).expanduser()
    return path if path.is_absolute() else CWD / path


def quote(cmd):
    return shlex.join([str(arg) for arg in cmd])


def render(cmd, toolchains=None):
    cmd = quote(cmd)
    return f"PATH={shlex.quote(str(toolchains))}:$PATH {cmd}" if toolchains else cmd


def build_env(toolchains):
    env = os.environ.copy()
    if toolchains:
        env["PATH"] = f"{toolchains}{os.pathsep}{env.get('PATH', '')}"
    return env


def parse_args():
    toolchains = os.getenv("CLAN_TOOLCHAINS", os.getenv("CLAN_TOOLCHAIN"))
    toolchains = relpath(toolchains) if toolchains else None

    parser = argparse.ArgumentParser(description="Run the ARM64 QEMU boot regression.")
    parser.add_argument("--toolchains", "--toolchain", default=toolchains, type=relpath)
    parser.add_argument("--cross-prefix", default=os.getenv("CLAN_CROSS_COMPILE", "aarch64-none-elf-"))
    parser.add_argument("--kernel", default=ROOT / "build/clan.debug.out", type=relpath)
    parser.add_argument("--qemu", default=os.getenv("QEMU_SYSTEM_AARCH64", "qemu-system-aarch64"))
    parser.add_argument("--smp", default=os.getenv("CLAN_QEMU_SMP", "8"))
    parser.add_argument("-m", "--memory", default=os.getenv("CLAN_QEMU_MEM", "1024M"))
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--log", default=ROOT / "build/regress.log", type=relpath)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args, extra = parser.parse_known_args()
    if extra[:1] == ["--"]:
        extra = extra[1:]
    args.extra = extra
    return args


def make_cmd(args):
    return [
        "make",
        "ARCH=arm64",
        "PLATFORM=qemu",
        f"CROSS_COMPILE={args.cross_prefix}",
        f"-j{os.cpu_count() or 1}",
    ]


def qemu_cmd(args):
    return [
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


def run_build(args, cmd):
    if args.toolchains and not args.toolchains.is_dir():
        raise SystemExit(f"Toolchain bin dir not found: {args.toolchains}")

    compiler = f"{args.cross_prefix}gcc"
    env = build_env(args.toolchains)
    if shutil.which(compiler, path=env.get("PATH")) is None:
        raise SystemExit(f"Compiler not found: {compiler}")

    print(f"[regress] build: {quote(cmd)}", flush=True)
    subprocess.run(cmd, cwd=ROOT, env=env, check=True)


class QemuSession:
    def __init__(self, cmd, log_path, timeout):
        self.cmd = cmd
        self.log_path = log_path
        self.timeout = timeout
        self.output = ""
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self.decoder_finalized = False
        self.proc = None
        self.selector = selectors.DefaultSelector()

    def __enter__(self):
        self.proc = subprocess.Popen(
            self.cmd,
            cwd=ROOT,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
        )
        self.selector.register(self.proc.stdout, selectors.EVENT_READ)
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        self.write_log()

    def close(self):
        if self.proc is None or self.proc.poll() is not None:
            return
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)

    def write_log(self):
        if self.log_path:
            self.flush_decoder()
            self.log_path.parent.mkdir(parents=True, exist_ok=True)
            self.log_path.write_text(self.output, encoding="utf-8", errors="replace")

    def flush_decoder(self):
        if not self.decoder_finalized:
            self.output += self.decoder.decode(b"", final=True)
            self.decoder_finalized = True

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def read_some(self, deadline):
        wait = max(0.0, min(0.25, deadline - time.monotonic()))
        for key, _ in self.selector.select(wait):
            data = os.read(key.fileobj.fileno(), 4096)
            if not data:
                return
            self.output += self.decoder.decode(data)
            self.check_fatal()

    def check_fatal(self):
        lower = self.output.lower()
        for pattern in FATAL_PATTERNS:
            if pattern in lower:
                raise RuntimeError(f"fatal QEMU output matched: {pattern}")

    def expect(self, pattern, name, timeout=None, keepalive=None):
        print(f"[regress] wait: {name}", flush=True)
        start_len = len(self.output)
        deadline = time.monotonic() + (timeout or self.timeout)
        next_keepalive = time.monotonic() + 2.0

        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"QEMU exited early with status {self.proc.returncode}")
            self.read_some(deadline)
            if pattern in self.output[start_len:]:
                print(f"[pass] {name}", flush=True)
                return self.output[start_len:]
            if keepalive and time.monotonic() >= next_keepalive:
                self.send(keepalive)
                next_keepalive = time.monotonic() + 2.0

        tail = self.output[-3000:]
        raise TimeoutError(f"timed out waiting for {name}: {pattern!r}\n--- output tail ---\n{tail}")

    def command(self, line, patterns, rejects=None):
        rejects = [] if rejects is None else rejects
        self.send(line + ENTER)
        text = self.expect(PROMPT, f"{line} returns to CLAN shell")
        for pattern in patterns:
            if pattern not in text:
                raise RuntimeError(f"{line!r} output missing {pattern!r}")
        for pattern in rejects:
            if pattern in text:
                raise RuntimeError(f"{line!r} output contains rejected {pattern!r}")
        print(f"[pass] {line}: expected output found", flush=True)


def run_qemu(args, cmd):
    if not args.kernel.is_file():
        raise SystemExit(f"Kernel image not found: {args.kernel}")
    if shutil.which(args.qemu) is None:
        raise SystemExit(f"QEMU binary not found: {args.qemu}")

    print(f"[regress] qemu: {quote(cmd)}", flush=True)
    with QemuSession(cmd, args.log, args.timeout) as qemu:
        qemu.expect(PROMPT, "CLAN shell prompt", keepalive=ENTER)
        qemu.command("vcpus", ["vmid", "vcpu", "pcpu_mode", "isolate", "shared", "switches", "since.us"])
        qemu.command("schedstat", [
            "schedstat algorithm:sched_iorr",
            "tick:1ms",
            "pcpu",
            "timer",
            "switches",
            "resched",
            "runqueue",
        ])
        qemu.command("vmap", ["arm64 memory mappings", "vm-0 s2", "vm-1 s2", "vm-2 s2"])
        qemu.command("irqs", ["irq statistics"])
        qemu.command(
            "dumpstat 0",
            [
                "┌─  dumpstat vm0",
                "┌─  vm0/vcpu0",
                "sched:",
                "├─  guest regs",
                "├─  guest stack symbols:none",
                "├─  host stack symbols:clan",
                "│   pcpu:",
                "source:vcpu-thread",
                "+0x",
            ],
            ["depth:", "vcpu saved stack", "vcpu vm stack", "host stack source:", "fp   0x",
             "live pcpu sample timed out"],
        )

        qemu.send("vsh 0" + ENTER)
        qemu.expect("zero ~>", "VM0 Zephyr shell", keepalive=ENTER)
        qemu.send(CTRL_D)
        qemu.expect(PROMPT, "return from VM0 shell")

        qemu.send("vsh 1" + ENTER)
        qemu.expect("beau ~>", "VM1 LK shell", keepalive=ENTER)
        qemu.send(CTRL_D)
        qemu.expect(PROMPT, "return from VM1 shell")

        qemu.send("vsh 2" + ENTER)
        qemu.expect("login:", "VM2 clot login prompt", keepalive=ENTER)
        qemu.send("clot" + ENTER)
        qemu.expect("password:", "VM2 clot password prompt")
        qemu.send("clot" + ENTER)
        qemu.expect("clot ~>", "VM2 clot shell")
        qemu.send(CTRL_D)
        qemu.expect(PROMPT, "return from VM2 shell")

    print(f"[pass] regression complete; log: {args.log}", flush=True)


def main():
    args = parse_args()
    build = make_cmd(args)
    qemu = qemu_cmd(args)

    if args.dry_run:
        if not args.no_build:
            print(render(build, args.toolchains))
        print(quote(qemu))
        print("checks: prompt, vcpus, schedstat, vmap, irqs, vsh 0, ctrl-d, vsh 1, ctrl-d, vsh 2, login")
        return 0

    if not args.no_build:
        run_build(args, build)
    run_qemu(args, qemu)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
