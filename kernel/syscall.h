// linux-style syscalls via int 0x80 — numbers match x86_64 linux
#pragma once
#include <stdint.h>
#include "idt.h"

#define SYS_read    0
#define SYS_write   1
#define SYS_open    2
#define SYS_close   3
#define SYS_mmap    9
#define SYS_mprotect 10
#define SYS_munmap  11
#define SYS_brk     12
#define SYS_getpid  39
#define SYS_fork    57
#define SYS_execve  59
#define SYS_exit    60
#define SYS_wait4   61

uint64_t syscall_dispatch(struct regs *r);
