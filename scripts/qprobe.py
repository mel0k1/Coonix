#!/usr/bin/env python3
"""Interactive timing probe: types one command, timestamps the reply."""
import socket
import json
import subprocess
import time
import select

QMP = "/tmp/coonix-qmp.sock"


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.f = self.s.makefile("rw")
        json.loads(self.f.readline())

    def cmd(self, name, args=None):
        m = {"execute": name}
        if args:
            m["arguments"] = args
        self.f.write(json.dumps(m) + "\n")
        self.f.flush()
        while True:
            line = json.loads(self.f.readline())
            if "return" in line or "error" in line:
                return line

    def key(self, *qcodes):
        keys = [{"type": "qcode", "data": q} for q in qcodes]
        return self.cmd("send-key", {"keys": keys})


def type_str(q, text, delay=0.01):
    specials = {" ": "spc", "\n": "ret", "-": "minus", "/": "slash", ".": "dot"}
    for ch in text:
        if ch in specials:
            q.key(specials[ch])
        elif ch.isupper():
            q.key("shift", ch.lower())
        else:
            q.key(ch)
        time.sleep(delay)


def main():
    open("/tmp/coonix-serial.log", "wb").close()
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-M", "pc", "-m", "2G",
        "-cdrom", "coonix.iso",
        "-drive", "file=build/disk.img,format=raw,if=ide,index=0",
        "-display", "none", "-serial", "file:/tmp/coonix-serial.log",
        "-qmp", f"unix:{QMP},server,nowait", "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ser = open("/tmp/coonix-serial.log", "rb")
    time.sleep(1)
    q = Qmp(QMP)
    q.cmd("qmp_capabilities")

    t0 = time.time()
    seen = b""
    while time.time() - t0 < 40:
        seen += ser.read()
        if b"coonix> " in seen:
            break
        time.sleep(0.3)
    print(f"boot+prompt at {time.time()-t0:.1f}s")

    def probe(cmd):
        t = time.time()
        type_str(q, cmd + "\n")
        # watch serial for the next prompt
        buf = b""
        deadline = time.time() + 25
        while time.time() < deadline:
            buf += ser.read()
            # prompt appeared after our echo = command finished
            if buf.count(b"coonix> ") >= 1 and (cmd[:2].encode() in buf or b"unknown" in buf):
                # wait a bit more to be sure
                pass
            if buf.rstrip().endswith(b"coonix>") or buf.count(b"coonix> ") >= 1:
                if b"\n" in buf or b"unknown" in buf:
                    time.sleep(0.3)
                    buf += ser.read()
                    print(f"[{cmd}] took {time.time()-t:.2f}s -> {buf!r}")
                    return
            time.sleep(0.2)
        print(f"[{cmd}] TIMEOUT 25s -> {buf!r}")

    probe("pid")          # builtin, no fork
    probe("hello")        # fork+exec from disk
    probe("hello")
    probe("mtest")
    probe("forktest")
    qemu.terminate()
    qemu.wait()


if __name__ == "__main__":
    main()
