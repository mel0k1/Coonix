#!/usr/bin/env python3
"""Headless Coonix test for /proc/<pid>/fd: fd listing, readlink targets
(console / pipe / file), plus ps + fsx regressions.
Usage: qfdtest.py [pc|q35]"""
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from qthrtest import main_setup, run_shell  # noqa: E402


def main():
    machine = sys.argv[1] if len(sys.argv) > 1 else "pc"
    qemu, q, ser = main_setup(machine)

    results = []
    # 1. the shell's own fd dir lists the three console fds (/bin/ls, the
    # mini shell's builtin ls has no args)
    results.append(run_shell(q, ser, "/bin/ls /proc/self/fd",
                             expect="2", timeout=90))
    # 2. ls -l shows the readlink targets of the console fds
    results.append(run_shell(q, ser, "/bin/ls -l /proc/self/fd",
                             expect="/dev/console", timeout=90))
    # 3. a pipe: inside busybox sh, ls fd 1 is a pipe end
    results.append(run_shell(q, ser, "sh -c '/bin/ls -l /proc/self/fd | cat'",
                             expect="pipe:[", timeout=120))
    # 4. a regular file: opened /etc/motd shows as its path
    results.append(run_shell(q, ser,
                             "sh -c 'exec 3</etc/motd; "
                             "/bin/ls -l /proc/$$/fd; exec 3<&-'",
                             expect="/etc/motd", timeout=120))
    # 5. regression: ps still parses stat with the new cpu fields
    results.append(run_shell(q, ser, "ps", expect="shell", timeout=60))
    # 6. regression: fs ops incl. pipes/dups untouched
    results.append(run_shell(q, ser, "fsx", expect="fsx: OK", timeout=180))

    qemu.terminate()
    qemu.wait()
    print("=" * 40)
    print(f"{machine}: {'ALL OK' if all(results) else 'FAILURES PRESENT'}")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
