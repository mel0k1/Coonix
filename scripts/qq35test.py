#!/usr/bin/env python3
"""Headless q35 test: AHCI disk, ext2 read+write over the new driver."""
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
        json.loads(self.f.readline())

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
        return self.cmd("send-key", {"keys": keys})

    def type_str(self, text, delay=0.03):
        specials = {" ": "spc", "\n": "ret", "-": "minus", "=": "equal",
                    "[": "bracket_left", "]": "bracket_right",
                    "/": "slash", ".": "dot", ",": "comma", ";": "semicolon"}
        for ch in text:
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
            elif ch == "_":
                q = "minus"
                shift = True
            else:
                continue
            if shift:
                self.key("shift", q)
            else:
                self.key(q)
            time.sleep(delay)


def main():
    open("/tmp/coonix-serial.log", "wb").close()
    ser = open("/tmp/coonix-serial.log", "rb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-L", "/home/z/tools/local/usr/share/qemu",
        "-M", "q35", "-m", "2G",
        "-cdrom", "coonix.iso",
        "-drive", "file=build/disk.img,format=raw,if=none,id=hd0",
        "-device", "ich9-ahci,id=ahci0",
        "-device", "ide-hd,drive=hd0,bus=ahci0.0",
        "-display", "none", "-serial", "file:/tmp/coonix-serial.log",
        "-qmp", f"unix:{QMP},server,nowait", "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)

    q = Qmp(QMP)
    q.cmd("qmp_capabilities")
    deadline = time.time() + 30
    seen = b""
    while time.time() < deadline:
        data = ser.read()
        seen += data
        if b"coonix> " in seen:
            break
        time.sleep(0.4)
    boot = seen.decode(errors="replace")

    def run(cmd, wait_s=4.0):
        q.type_str(cmd + "\n")
        time.sleep(wait_s)
        return ser.read().decode(errors="replace")

    print("=== BOOT (q35/ahci) ===")
    print(boot)
    print("=== mtest ===")
    print(run("mtest", 15))
    print("=== fstest ===")
    print(run("fstest", 15))
    print("=== glibc_hello ===")
    print(run("glibc_hello", 12))
    print("=== hello_dyn ===")
    print(run("hello_dyn", 15))
    print("=== cat /test.txt ===")
    print(run("cat /test.txt"))
    print("=== ls ===")
    print(run("ls"))

    qemu.terminate()
    qemu.wait()
    return 0


if __name__ == "__main__":
    sys.exit(main())
