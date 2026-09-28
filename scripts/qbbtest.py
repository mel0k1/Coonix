#!/usr/bin/env python3
"""Headless Coonix test for busybox: applets, real sh pipelines, fs ops.
Usage: qbbtest.py [pc|q35]"""
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from qthrtest import main_setup, run_shell  # noqa: E402


def main():
    machine = sys.argv[1] if len(sys.argv) > 1 else "pc"
    qemu, q, ser = main_setup(machine)

    results = []
    # multiplexer basics
    results.append(run_shell(q, ser, "busybox echo coonix-bb",
                             expect="coonix-bb", timeout=60))
    results.append(run_shell(q, ser, "busybox uname -m", expect="x86_64",
                             timeout=60))
    # getdents64 + newfstatat through ls
    results.append(run_shell(q, ser, "busybox ls /bin",
                             expect="busybox", timeout=60))
    # real shell: pipeline through two children (pipe2 + dup2 + fork+exec)
    results.append(run_shell(q, ser,
                             "busybox sh -c 'ls /bin | grep busybox'",
                             expect="busybox", timeout=90))
    # redirect: stdout into a file, then cat it back
    results.append(run_shell(q, ser,
                             "busybox sh -c 'echo piped > /tmp/f; cat /tmp/f'",
                             expect="piped", timeout=90))
    # cp/mv/rm on ext2 (openat/ftruncate/unlink/rename); ls emits ansi
    # escapes, so match on the marker name
    results.append(run_shell(q, ser,
                             "busybox sh -c 'cp /etc/motd /tmp/mmv; "
                             "mv /tmp/mmv /tmp/zzmarker; ls /tmp'",
                             expect="zzmarker", timeout=120))
    results.append(run_shell(q, ser,
                             "busybox sh -c 'rm /tmp/zzmarker; echo rmdone'",
                             expect="rmdone", timeout=90))
    results.append(run_shell(q, ser,
                             "busybox seq 1 9 > /tmp/s; "
                             "busybox tail -n 3 /tmp/s",
                             expect="9", timeout=90))
    # mini-shell + glibc regressions in the same boot
    results.append(run_shell(q, ser, "dltest", expect="dltest: PASS",
                             timeout=90))
    results.append(run_shell(q, ser, "fsx", expect="fsx: OK", timeout=90))
    results.append(run_shell(q, ser, "hello_dyn",
                             expect="malloc works too", timeout=90))

    qemu.terminate()
    qemu.wait()
    print("=" * 40)
    print(f"{machine}: {'ALL OK' if all(results) else 'FAILURES PRESENT'}")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
