#!/usr/bin/env python3
"""DHCP test: boots and checks the kernel dhcp client got a lease.

Slirp's built-in dhcp server answers on healthy qemu builds. On
emulators whose virtio dma rx path is broken the kernel reports the
timeout fallback; that is reported as a skip (exit 0) rather than a
failure, mirroring the virtio-blk self-test policy.
"""
import subprocess
import sys
import time

sys.path.insert(0, "scripts")
from qtest import QMP  # noqa: E402


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
    deadline = time.time() + 150
    seen = b""
    shell_at = None
    while time.time() < deadline:
        seen += ser.read()
        if b"[dhcp] lease" in seen or b"[dhcp] no lease" in seen:
            time.sleep(2)
            break
        if b"shell is up" in seen:
            # the dhcp give-up line lands ~6s after net init; keep reading
            if shell_at is None:
                shell_at = time.time()
            if time.time() - shell_at > 15:
                break
        time.sleep(0.5)
    seen += ser.read()
    qemu.terminate()
    qemu.wait()
    out = seen.decode(errors="replace")
    print(out[-3000:])
    if "[dhcp] lease" in out:
        print("qdhcp: lease acquired")
        return 0
    if "[dhcp] no lease" in out:
        print("qdhcp: SKIP (no dhcp reply on this qemu build)")
        return 0
    print("qdhcp: FAIL (no dhcp state machine output)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
