#!/usr/bin/env python3
"""TCP test: boots with a virtio-net nic, runs tcptest (loopback echo).

The whole tcp handshake + echo + fin teardown runs over 127.0.0.1, so
it is independent of the nic dma path and must always pass.
"""
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
        "-drive", "file=build/disk.img,format=raw,if=none,id=dsk",
        "-device", "virtio-blk-pci,drive=dsk,ioeventfd=off",
        "-device", "virtio-net-pci,netdev=nd,disable-legacy=off",
        "-netdev", "user,id=nd",
        "-display", "none", "-serial", "file:/tmp/coonix-serial.log",
        "-qmp", f"unix:{QMP},server,nowait", "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    q = Qmp(QMP)
    q.cmd("qmp_capabilities")
    deadline = time.time() + 150
    seen = b""
    while time.time() < deadline:
        seen += ser.read()
        if b"shell is up" in seen:
            break
        time.sleep(0.5)
    if b"shell is up" not in seen:
        print("qtcp: boot timeout")
        qemu.terminate()
        return 1
    q.type_str("tcptest\n")
    time.sleep(20)
    out = ser.read().decode(errors="replace")
    qemu.terminate()
    qemu.wait()
    print(out)
    if "[tcptest] PASS" in out:
        print("qtcp: all ok")
        return 0
    print("qtcp: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
