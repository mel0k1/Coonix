#!/bin/bash
# unpacked-without-root toolchain env: source this before make/run
export PATH="/home/z/tools/local/usr/bin:/home/z/tools/nasm-root/usr/bin:$PATH"
export LD_LIBRARY_PATH="/home/z/tools/local/usr/lib/x86_64-linux-gnu:/home/z/tools/local/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QEMU_DATA_DIR=/home/z/tools/local/usr/share/qemu
