#!/usr/bin/env python3
"""Headless Coonix test for W^X: NX data, mprotect promote/demote, RO text.
Usage: qwxtest.py [pc|q35]"""
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from qthrtest import main_setup, run_shell  # noqa: E402


def main():
    machine = sys.argv[1] if len(sys.argv) > 1 else "pc"
    qemu, q, ser = main_setup(machine)

    results = []
    results.append(run_shell(q, ser, "wxtest", expect="wxtest: ALL OK",
                             timeout=90))
    # mmap/mprotect regression after the fork NX-preserving fix
    results.append(run_shell(q, ser, "mtest", expect="mtest: all ok",
                             timeout=120))
    # fork: the child must inherit NX on data pages (COW flag copy)
    results.append(run_shell(q, ser, "forktest",
                             expect="cow works", timeout=120))

    qemu.terminate()
    qemu.wait()
    print("=" * 40)
    print(f"{machine}: {'ALL OK' if all(results) else 'FAILURES PRESENT'}")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
