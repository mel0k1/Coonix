#!/usr/bin/env python3
"""Demand paging test: boots QEMU, runs dpagetest, expects PASS."""
import subprocess
import sys
import time

sys.path.insert(0, "scripts")
from qtest import Qmp, QMP  # noqa: E402


def main():
    open("/tmp/coonix-serial.log", "wb").close()
    ser = open("/tmp/coonix-serial.log", "rb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-M", "pc", "-m", "2G",
        "-cdrom", "coonix.iso",
        "-drive", "file=build/disk.img,format=raw,if=ide,index=0",
        "-display", "none", "-serial", "file:/tmp/coonix-serial.log",
        "-qmp", f"unix:{QMP},server,nowait", "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    q = Qmp(QMP)
    q.cmd("qmp_capabilities")
    deadline = time.time() + 60
    seen = b""
    while time.time() < deadline:
        data = ser.read()
        seen += data
        if b"coonix> " in seen:
            break
        time.sleep(0.4)
    if b"coonix> " not in seen:
        print("qdpage: boot timeout")
        qemu.terminate()
        return 1

    q.type_str("dpagetest\n")
    time.sleep(8)
    out = ser.read().decode(errors="replace")
    print(out)
    qemu.terminate()
    qemu.wait()

    if "[dpagetest] PASS" in out:
        print("qdpage: all ok")
        return 0
    print("qdpage: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
