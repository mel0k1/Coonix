#!/usr/bin/env python3
"""Headless procfs test: boots, runs ps, cats /proc files, runs fsx."""
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
        return self.cmd("send-key", {"keys": keys})

    def type_str(self, text, delay=0.012):
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
        "-M", "pc", "-m", "2G",
        "-cdrom", "coonix.iso",
        "-drive", "file=build/disk.img,format=raw,if=ide,index=0",
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

    print("=== BOOT ===")
    print(boot)
    print("=== ps ===")
    print(run("ps", 6))
    print("=== busybox ls / (mount overlay: proc visible) ===")
    print(run("/bin/ls /", 8))
    print("=== busybox ls /proc (pid dirs) ===")
    print(run("/bin/ls /proc", 8))
    print("=== cat /proc/1/status ===")
    print(run("cat /proc/1/status", 6))
    print("=== cat /proc/1/stat ===")
    print(run("cat /proc/1/stat", 6))
    print("=== cat /proc/1/cmdline (xxd-style via od) ===")
    print(run("cat /proc/1/cmdline", 6))
    print("=== gone pid: cat /proc/2/stat (expect ENOENT-ish) ===")
    print(run("cat /proc/2/stat", 6))
    print("=== /proc/self: busybox ls -l, cat cmdline via self ===")
    print(run("/bin/ls -l /proc/self", 8))
    print(run("cat /proc/self/exe", 6))
    print("=== cat /proc/meminfo ===")
    print(run("cat /proc/meminfo", 6))
    print("=== busybox free (reads meminfo) ===")
    print(run("free", 8))
    print("=== cat /proc/mounts ===")
    print(run("cat /proc/mounts", 6))
    print("=== cat /proc/uptime ===")
    print(run("cat /proc/uptime", 6))
    print("=== busybox free again (sysinfo=99 fixed) ===")
    print(run("free", 8))
    print("=== readlink test via busybox ===")
    print(run("pwd", 4))
    print("=== fsx regression ===")
    print(run("fsx", 25))

    qemu.terminate()
    qemu.wait()
    return 0


if __name__ == "__main__":
    sys.exit(main())
