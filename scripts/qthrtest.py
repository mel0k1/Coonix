#!/usr/bin/env python3
"""Headless Coonix test for the glibc-depth work: pthreads, signals, tty.
Boots with the ext2 disk, runs pthreadtest/sigtest/iotest through the
shell over QMP, watches serial. Usage: qthrtest.py [pc|q35]"""
import json
import socket
import subprocess
import sys
import time

QMP = "/tmp/coonix-qmp.sock"


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
                print(f"QMP ERROR {name}: {line['error']}", file=sys.stderr)
                return line
            if "return" in line:
                return line

    def key(self, *qcodes):
        keys = [{"type": "qcode", "data": q} for q in qcodes]
        try:
            return self.cmd("send-key", {"keys": keys})
        except (BrokenPipeError, OSError):
            return None

    def type_str(self, text, delay=0.03):
        specials = {" ": "spc", "\n": "ret", "-": "minus", "=": "equal",
                    "[": "bracket_left", "]": "bracket_right",
                    "/": "slash", ".": "dot", ",": "comma", ";": "semicolon",
                    "`": "grave_accent", "'": "apostrophe",
                    "\\\\": "backslash"}
        shifted = {"|": "backslash", ">": "dot", "<": "comma",
                   "\"": "apostrophe", "~": "grave_accent",
                   "$": "4", "#": "3", "&": "7", "*": "8",
                   "(": "9", ")": "0", "+": "equal", "_": "minus",
                   ":": "semicolon", "!": "1", "?": "slash",
                   "%": "5", "@": "2", "^": "6", "{": "bracket_left",
                   "}": "bracket_right"}
        for ch in text:
            if ch in shifted:
                self.key("shift", shifted[ch])
                time.sleep(delay)
                continue
            if ch in specials:
                q = specials[ch]
                shift = False
            elif ch.isupper():
                q = ch.lower()
                shift = True
            elif ch.isdigit():
                q = ch
                shift = False
            elif ch.isalpha():
                q = ch
                shift = False
            else:
                continue
            if shift:
                self.key("shift", q)
            else:
                self.key(q)
            time.sleep(delay)


def main_setup(machine):
    """boot qemu headless, connect qmp, wait for the shell prompt"""
    open("/tmp/coonix-serial.log", "wb").close()
    ser = open("/tmp/coonix-serial.log", "rb")

    if machine == "q35":
        drive = ["-L", "/home/z/tools/local/usr/share/qemu",
                 "-drive", "file=build/disk.img,format=raw,if=none,id=hd0",
                 "-device", "ich9-ahci,id=ahci0",
                 "-device", "ide-hd,drive=hd0,bus=ahci0.0"]
    else:
        drive = ["-L", "/home/z/tools/local/usr/share/qemu",
                 "-drive", "file=build/disk.img,format=raw,if=ide,index=0"]

    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-M", machine, "-m", "2G",
        "-cdrom", "coonix.iso", *drive,
        "-display", "none", "-serial", "file:/tmp/coonix-serial.log",
        "-qmp", f"unix:{QMP},server,nowait", "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)

    deadline = time.time() + 30
    q = None
    while time.time() < deadline:
        try:
            q = Qmp(QMP)
            break
        except (ConnectionRefusedError, FileNotFoundError, OSError):
            if qemu.poll() is not None:
                print("qemu died early", file=sys.stderr)
                raise SystemExit(1)
            time.sleep(0.5)
    if q is None:
        print("qmp socket never came up", file=sys.stderr)
        raise SystemExit(1)
    q.cmd("qmp_capabilities")
    deadline = time.time() + 30
    seen = b""
    while time.time() < deadline:
        seen += ser.read()
        if b"coonix> " in seen:
            break
        time.sleep(0.4)
    return qemu, q, ser


def run_shell(q, ser, cmd, expect=None, timeout=40):
    """type a command at the shell, wait for expected output"""
    # the prompt prints slightly before read() re-arms; give it a beat
    time.sleep(0.6)
    q.type_str(cmd + "\n")
    acc = b""
    start = time.time()
    while time.time() - start < timeout:
        acc += ser.read()
        if expect and expect.encode() in acc:
            break
        time.sleep(0.3)
    out = acc.decode(errors="replace")
    ok = not expect or expect in out
    print(f"[{'OK' if ok else 'FAIL'}] {cmd}")
    if not ok:
        print(out)
    elif len(out) < 800:
        print(out.rstrip())
    return ok


def main():
    machine = sys.argv[1] if len(sys.argv) > 1 else "pc"
    qemu, q, ser = main_setup(machine)

    results = []
    results.append(run_shell(q, ser, "pthreadtest", expect="pthreadtest: OK",
                             timeout=120))
    results.append(run_shell(q, ser, "sigtest", expect="sigtest: OK",
                             timeout=120))
    # iotest: typed interaction (canonical line, then raw key)
    q.type_str("iotest\n")
    time.sleep(3)
    q.type_str("hello\n")
    time.sleep(2)
    q.type_str("z")
    time.sleep(2)
    out = ser.read().decode(errors="replace")
    print(out.rstrip())
    ok = ("canonical line" in out and "raw key z" in out and
          "iotest: OK" in out)
    print(f"[{'OK' if ok else 'FAIL'}] iotest")
    results.append(ok)
    # ctrl-C must not kill the shell: interrupt the raw read mid-way
    # (iotest exited; shell is at prompt)
    results.append(run_shell(q, ser, "hello", expect="hello from ring 3"))
    results.append(run_shell(q, ser, "forktest", expect="cow works"))

    qemu.terminate()
    qemu.wait()
    print("=" * 40)
    print(f"{machine}: {'ALL OK' if all(results) else 'FAILURES PRESENT'}")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
