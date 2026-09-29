#!/usr/bin/env python3
"""Flak probe: repeatedly run dynamic-binary starts incl. sh pipelines.
Usage: qflakprobe.py [pc|q35] [rounds]"""
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from qthrtest import main_setup, run_shell  # noqa: E402


def main():
    machine = sys.argv[1] if len(sys.argv) > 1 else "pc"
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    qemu, q, ser = main_setup(machine)

    results = []
    for i in range(rounds):
        results.append(run_shell(q, ser, "sh -c 'ls /bin | grep busybox'",
                                 expect="busybox", timeout=120))
        results.append(run_shell(q, ser, "hello_dyn",
                                 expect="malloc works too", timeout=120))
        results.append(run_shell(q, ser, "dltest", expect="dltest: PASS",
                                 timeout=120))

    qemu.terminate()
    qemu.wait()
    print("=" * 40)
    print(f"{machine}: {results.count(True)}/{len(results)} ok")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
