#!/usr/bin/env python3
"""Headless dynamic-linker debug: boots, runs hello_dyn, dumps everything."""
import sys
import time

sys.path.insert(0, "scripts")
from qfstest import Qmp, QMP  # noqa: E402


def main():
    open("/tmp/coonix-serial.log", "wb").close()
    ser = open("/tmp/coonix-serial.log", "rb")
    import subprocess
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-L", "/home/z/tools/local/usr/share/qemu",
        "-M", "pc", "-m", "2G",
        "-cdrom", "coonix.iso",
        "-drive", "file=build/disk.img,format=raw,if=ide,index=0",
        "-display", "none", "-serial", "file:/tmp/coonix-serial.log",
        "-qmp", f"unix:{QMP},server,nowait", "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    import os
    for _ in range(50):
        if os.path.exists(QMP):
            break
        time.sleep(0.2)
    time.sleep(0.5)

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

    print("=== hello_dyn (LD_DEBUG=all) ===")
    q.type_str("hello_dyn\n")
    time.sleep(20)
    print(ser.read().decode(errors="replace"))

    qemu.terminate()
    qemu.wait()
    return 0


if __name__ == "__main__":
    sys.exit(main())
