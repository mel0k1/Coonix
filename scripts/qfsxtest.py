#!/usr/bin/env python3
"""Headless Coonix test for the fs porting set (fsx): namei, getdents,
symlinks, cwd, pipes, dup2 redirection, fcntl. Usage: qfsxtest.py [pc|q35]"""
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from qthrtest import main_setup, run_shell  # noqa: E402


def main():
    machine = sys.argv[1] if len(sys.argv) > 1 else "pc"
    qemu, q, ser = main_setup(machine)

    results = []
    results.append(run_shell(q, ser, "fsx", expect="fsx: OK", timeout=90))
    # mini-shell builtins still fine after the argv change
    results.append(run_shell(q, ser, "hello", expect="hello from ring 3"))
    results.append(run_shell(q, ser, "forktest", expect="cow works"))
    # busybox multiplexer rides the same ext2 + symlink machinery
    results.append(run_shell(q, ser, "busybox uname -m", expect="x86_64",
                             timeout=60))

    qemu.terminate()
    qemu.wait()
    print("=" * 40)
    print(f"{machine}: {'ALL OK' if all(results) else 'FAILURES PRESENT'}")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
