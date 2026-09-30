#!/usr/bin/env python3
"""run loop4 with LD_DEBUG=all; capture the debug trace around failures."""
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

    time.sleep(1.0)
    q.type_str("LD_DEBUG=all busybox sh /tmp/loop4.sh\n", delay=0.05)
    out = wait_for("PARDONE", 240)
    print(f"finished: {'PARDONE' if 'PARDONE' in out else 'HANG'}")
    qemu.terminate()
    qemu.wait()
    # analyze the whole serial file
    full = open("/tmp/coonix-serial.log", "rb").read().decode(errors="replace")
    open("/tmp/lddebug.log", "w").write(full)
    i = full.find("Relink")
    j = full.find("Inconsistency")
    k = full.find("unexpected PLT")
    m = full.find("KERNEL PANIC")
    print(f"relink@{i} assert@{j} plt@{k} panic@{m} len={len(full)}")
    if j >= 0:
        print(full[max(0, j - 3500):j + 300])
    elif i >= 0:
        print(full[max(0, i - 3500):i + 300])
    return 0


if __name__ == "__main__":
    sys.exit(main())
