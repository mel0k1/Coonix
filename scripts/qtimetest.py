#!/usr/bin/env python3
"""Headless Coonix test for cpu accounting: getrusage(98)/times(100) via
real glibc, plus /proc/<pid>/stat utime/stime/starttime.
Usage: qtimetest.py [pc|q35]"""
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from qthrtest import main_setup, run_shell  # noqa: E402


def main():
    machine = sys.argv[1] if len(sys.argv) > 1 else "pc"
    qemu, q, ser = main_setup(machine)

    results = []
    # self + children accounting through the real libc
    results.append(run_shell(q, ser, "timetest", expect="timetest: ALL OK",
                             timeout=180))
    # busybox free/date exercise sysinfo+gettimeofday; ps reads proc stat
    results.append(run_shell(q, ser, "ps", expect="shell", timeout=60))

    qemu.terminate()
    qemu.wait()
    print("=" * 40)
    print(f"{machine}: {'ALL OK' if all(results) else 'FAILURES PRESENT'}")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
