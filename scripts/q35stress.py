#!/usr/bin/env python3
"""q35 flak hunt: run a headless Coonix test N times on q35, keep serial
logs of failures, summarize. Usage: q35stress.py <script> <n>
  script: qbbtest|qthrtest|qfstest|qldflak4 (scripts/<name>.py)
each run gets a fresh /tmp/coonix-serial.log; failures are copied to
/tmp/q35stress/run-<i>.log with the driver output."""
import os
import shutil
import subprocess
import sys

RUNS_DIR = "/tmp/q35stress"


def main():
    script = sys.argv[1] if len(sys.argv) > 1 else "qbbtest"
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    os.makedirs(RUNS_DIR, exist_ok=True)
    path = f"scripts/{script}.py"
    fails = 0
    for i in range(n):
        log = f"/tmp/coonix-serial.log"
        open(log, "wb").close()
        r = subprocess.run(
            [sys.executable, path, "q35"],
            capture_output=True, text=True, timeout=900)
        out = r.stdout[-2000:] + ("\n[stderr] " + r.stderr[-1000:]
                                  if r.stderr.strip() else "")
        # verdict-style scripts print ALL OK; content-style (qfstest)
        # rely on rc plus absence of kernel/app failure markers
        bad_markers = ("segfault", "panic", "exception at", "unknown:",
                       "REDLINE", "FRAME-MISMATCH")
        ok = r.returncode == 0 and (
            "ALL OK" in r.stdout or not any(m in r.stdout for m in bad_markers))
        status = "OK  " if ok else "FAIL"
        print(f"[{status}] run {i+1}/{n} {script} q35 rc={r.returncode}")
        if not ok:
            fails += 1
            shutil.copy(log, f"{RUNS_DIR}/{script}-run-{i}.log")
            print(out)
    print("=" * 40)
    print(f"{script} q35: {n - fails}/{n} OK, {fails} failures")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
