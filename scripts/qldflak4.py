#!/usr/bin/env python3
"""parallel exec stress (staged /tmp/loop4.sh, minimal typing).
On failure or hang: cats the children's output files for forensics.
Usage: qldflak4.py [pc|q35]"""
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from qthrtest import main_setup  # noqa: E402


def main():
    machine = sys.argv[1] if len(sys.argv) > 1 else "pc"
    qemu, q, ser = main_setup(machine)

    def wait_for(marker, timeout):
        deadline = time.time() + timeout
        acc = b""
        while time.time() < deadline:
            acc += ser.read()
            if marker.encode() in acc:
                break
            time.sleep(0.3)
        return acc.decode(errors="replace")

    def cmd(text, marker, timeout):
        time.sleep(1.0)
        q.type_str(text + "\n", delay=0.05)
        return wait_for(marker, timeout)

    time.sleep(1.0)
    q.type_str("busybox sh /tmp/loop4.sh\n", delay=0.05)
    out = wait_for("PARDONE", 200)
    fails = 0
    for m in ["AFAIL", "BFAIL", "CFAIL"]:
        if m in out:
            fails += 1
            print(f"[FAIL] {m}")
    if "PARDONE" not in out:
        fails += 1
        print("[FAIL] hang/crash before PARDONE")

    if not fails:
        print("[OK] loop4 completed clean")
    else:
        # raw serial tail first (before any further interaction)
        full = open("/tmp/coonix-serial.log", "rb").read().decode(errors="replace")
        print("=== raw serial tail 1200 ===")
        print(full[-1200:])
        # forensics: what did the children actually produce?
        for f in ["/tmp/a.out", "/tmp/b.out", "/tmp/c.out"]:
            o = cmd(f"busybox cat {f}", "coonix> ", 25)
            print(f"--- cat {f} ---")
            print(o[-600:])
        print("=== accumulated serial tail 2500 ===")
        print(out[-2500:])
        qemu.terminate()
        qemu.wait()
        return 1

    qemu.terminate()
    qemu.wait()
    print("=" * 40)
    print(f"ldflak4 {machine}: ALL OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
