#!/usr/bin/env python3
"""CI boot test: boots coonix.iso in headless QEMU, drives the shell over
QMP, asserts per-command output markers on serial. Exits non-zero on any
failure. Works under TCG (no KVM needed)."""
import json
import os
import socket
import subprocess
import sys
import time

QMP = "/tmp/coonix-ci-qmp.sock"
SERIAL = "/tmp/coonix-ci-serial.log"

# command -> substrings that must all appear in its output
CHECKS = [
    ("hello",    ["hello from ring 3!"]),
    ("mtest",    ["mtest: all ok"]),
    ("forktest", ["cow works"]),
    ("fstest",   ["fstest: all ok"]),
    ("fsx",      ["fsx: OK"]),
    ("dtest",    ["dtest done"]),
    ("ps",       []),
]

# serial text that means the kernel is dead regardless of markers
FATALS = ["panic", "PANIC", " triple fault", "#DF"]


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.f = self.s.makefile("rw")
        json.loads(self.f.readline())  # greeting

    def cmd(self, name, args=None):
        m = {"execute": name}
        if args:
            m["arguments"] = args
        self.f.write(json.dumps(m) + "\n")
        self.f.flush()
        while True:
            line = json.loads(self.f.readline())
            if "error" in line:
                raise RuntimeError(f"QMP {name}: {line['error']}")
            if "return" in line:
                return line["return"]

    def key(self, *qcodes):
        keys = [{"type": "qcode", "data": q} for q in qcodes]
        self.cmd("send-key", {"keys": keys})

    def type_str(self, text, delay=0.01):
        specials = {" ": "spc", "\n": "ret", "-": "minus", "=": "equal",
                    "[": "bracket_left", "]": "bracket_right",
                    "/": "slash", ".": "dot", ",": "comma", ";": "semicolon"}
        for ch in text:
            if ch in specials:
                self.key(specials[ch])
            elif ch.isupper():
                self.key("shift", ch.lower())
            elif ch.isalnum() or ch == "_":
                self.key(ch)
            else:
                continue
            time.sleep(delay)


def wait_prompt(ser, deadline_s, needle=b"coonix> "):
    deadline = time.time() + deadline_s
    seen = b""
    while time.time() < deadline:
        chunk = ser.read()
        if chunk:
            seen += chunk
            if needle in seen:
                return True, seen
        else:
            time.sleep(0.2)
    return False, seen


def main():
    open(SERIAL, "wb").close()
    ser = open(SERIAL, "rb")
    qemu_cmd = [
        "qemu-system-x86_64", "-M", "pc", "-m", "2G",
        "-cdrom", "coonix.iso",
        "-drive", "file=build/disk.img,format=raw,if=ide,index=0",
        "-display", "none", "-serial", f"file:{SERIAL}",
        "-qmp", f"unix:{QMP},server,nowait", "-no-reboot"]
    # unpacked-without-root setups point QEMU_DATA_DIR at their rom dir
    if os.environ.get("QEMU_DATA_DIR"):
        qemu_cmd += ["-L", os.environ["QEMU_DATA_DIR"]]
    qemu = subprocess.Popen(qemu_cmd,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1)
        if qemu.poll() is not None:
            print("FAIL: qemu exited immediately", file=sys.stderr)
            return 1
        q = Qmp(QMP)
        q.cmd("qmp_capabilities")

        print("== waiting for shell prompt (TCG can be slow) ==")
        ok, boot = wait_prompt(ser, 240)
        print(boot.decode(errors="replace")[-2000:])
        if not ok:
            print("FAIL: no shell prompt within timeout", file=sys.stderr)
            return 1

        failures = []
        for cmd, markers in CHECKS:
            q.type_str(cmd + "\n")
            ok, out = wait_prompt(ser, 180)
            text = out.decode(errors="replace")
            print(f"---- {cmd} ----")
            print(text)
            if not ok:
                failures.append(f"{cmd}: no prompt back (timeout)")
                continue
            for m in markers:
                if m not in text:
                    failures.append(f"{cmd}: missing marker '{m}'")
            for f in FATALS:
                if f in text:
                    failures.append(f"{cmd}: fatal pattern '{f}' in serial")

        print("================")
        if failures:
            for f in failures:
                print(f"FAIL: {f}")
            return 1
        print("ci_test: all ok")
        return 0
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
