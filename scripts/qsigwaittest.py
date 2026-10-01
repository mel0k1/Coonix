#!/usr/bin/env python3
"""Headless Coonix test for signal waits: pause(34)/rt_sigsuspend(130)/
rt_sigtimedwait(128) via real glibc. Usage: qsigwaittest.py [pc|q35]"""
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from qthrtest import main_setup, run_shell  # noqa: E402


def main():
    machine = sys.argv[1] if len(sys.argv) > 1 else "pc"
    qemu, q, ser = main_setup(machine)

    results = []
    # the wait primitives themselves
    results.append(run_shell(q, ser, "sigwaittest", expect="sigwaittest: ALL OK",
                             timeout=180))
    # the pre-existing signal suite must stay green alongside
    results.append(run_shell(q, ser, "sigtest", expect="sigtest: OK",
                             timeout=180))

    qemu.terminate()
    qemu.wait()
    print("=" * 40)
    print(f"{machine}: {'ALL OK' if all(results) else 'FAILURES PRESENT'}")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
