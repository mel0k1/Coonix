#!/usr/bin/env python3
"""Priority scheduler test: boots QEMU, runs nicetest, checks nice bias.

PASS criteria (parsed from the serial log):
  - "nicetest: PASS" from the in-guest priority api checks
  - the normal hog out-runs the niced hog by > 5x
  - equal-nice hogs stay within 3x of each other (round robin alive)
"""
import re
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
        print("qnice: boot timeout")
        qemu.terminate()
        return 1

    def run(cmd, wait_s):
        q.type_str(cmd + "\n")
        time.sleep(wait_s)
        return ser.read().decode(errors="replace")

    out = run("nicetest", 16)
    print(out)
    qemu.terminate()
    qemu.wait()

    if "[nicetest] PASS" not in out:
        print("qnice: in-guest checks failed")
        return 1
    a = re.search(r"A\(\+19\) count (\d+)", out)
    b = re.search(r"B\(0\)\s+count (\d+)", out)
    r1 = re.search(r"R1 count (\d+)", out)
    r2 = re.search(r"R2 count (\d+)", out)
    if not (a and b and r1 and r2):
        print("qnice: missing count lines")
        return 1
    bias = int(b.group(1)) / max(int(a.group(1)), 1)
    lo = min(int(r1.group(1)), int(r2.group(1)))
    hi = max(int(r1.group(1)), int(r2.group(1)))
    rr = hi / max(lo, 1)
    print(f"qnice: bias B/A = {bias:.1f} (need > 5), rr R2/R1 = {rr:.2f} (need < 3)")
    if bias <= 5 or rr >= 3:
        print("qnice: FAIL")
        return 1
    print("qnice: all ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
