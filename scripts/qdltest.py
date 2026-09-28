#!/usr/bin/env python3
"""Headless Coonix test for dlopen: runtime .so loading through glibc.
Usage: qdltest.py [pc|q35]"""
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from qthrtest import Qmp, main_setup, run_shell  # noqa: E402


def main():
    machine = sys.argv[1] if len(sys.argv) > 1 else "pc"
    qemu, q, ser = main_setup(machine)

    results = []
    results.append(run_shell(q, ser, "dltest", expect="dltest: PASS",
                             timeout=60))
    # sanity: dynamic hello still boots the same machinery
    results.append(run_shell(q, ser, "hello_dyn", expect="malloc works too",
                             timeout=60))

    qemu.terminate()
    qemu.wait()
    print("=" * 40)
    print(f"{machine}: {'ALL OK' if all(results) else 'FAILURES PRESENT'}")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
