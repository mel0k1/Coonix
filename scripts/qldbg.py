#!/usr/bin/env python3
"""debug loop2 hang: boot, run loop2.sh, dump everything, then ping."""
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
    q.type_str("busybox sh /tmp/loop2.sh\n", delay=0.05)
    out = wait_for("MIX15DONE", 240)
    print("=== loop2 output (tail 3000) ===")
    print(out[-3000:])

    # alive?
    time.sleep(1.0)
    q.type_str("echo alive\n", delay=0.05)
    out2 = wait_for("alive", 30)
    print("=== ping ===")
    print(out2[-600:])

    qemu.terminate()
    qemu.wait()
    return 0


if __name__ == "__main__":
    sys.exit(main())
