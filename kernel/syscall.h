// linux-style syscalls via int 0x80 — numbers match x86_64 linux
#pragma once
#include <stdint.h>
#include "idt.h"

#define SYS_read    0
#define SYS_write   1
#define SYS_open    2
#define SYS_close   3
#define SYS_fstat   5
#define SYS_mmap    9
#define SYS_mprotect 10
#define SYS_munmap  11
#define SYS_brk     12
#define SYS_writev  20
#define SYS_getpid  39
#define SYS_uname   63
#define SYS_getuid  102
#define SYS_getgid  104
#define SYS_geteuid 107
#define SYS_getegid 108
#define SYS_fork    57
#define SYS_execve  59
#define SYS_exit    60
#define SYS_wait4   61
#define SYS_arch_prctl 158
#define SYS_gettid  186
#define SYS_set_tid_address 218
#define SYS_exit_group 231
#define SYS_openat  257
#define SYS_set_robust_list 273
#define SYS_getrandom 318

uint64_t syscall_dispatch(struct regs *r);
