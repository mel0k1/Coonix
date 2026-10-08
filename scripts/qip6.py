#!/usr/bin/env python3
"""ip6test: boots with a virtio-net nic, runs ip6test (ipv6 loopback).

The fatal assertions run over ::1 (icmpv6 echo, udp6 echo, tcp6
stream); the slirp link-local ping is non-fatal inside the test.
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
        print("qip6: boot timeout")
        qemu.terminate()
        return 1
    q.type_str("ip6test\n")
    time.sleep(30)
    out = ser.read().decode(errors="replace")
    qemu.terminate()
    qemu.wait()
    print(out)
    if "[ip6test] all ok" in out:
        print("qip6: all ok")
        return 0
    print("qip6: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
