// linux-style syscalls via int 0x80 — numbers match x86_64 linux
#pragma once
#include <stdint.h>
#include "idt.h"

#define SYS_read    0
#define SYS_write   1
#define SYS_open    2
#define SYS_close   3
#define SYS_fstat   5
#define SYS_lseek   8
#define SYS_mmap    9
#define SYS_mprotect 10
#define SYS_munmap  11
#define SYS_brk     12
#define SYS_rt_sigaction 13
#define SYS_rt_sigprocmask 14
#define SYS_rt_sigreturn 15
#define SYS_ioctl   16
#define SYS_pread64 17
#define SYS_writev  20
#define SYS_madvise 28
#define SYS_nanosleep 35
#define SYS_getpid  39
/* x86_64 ABI: times = 100 (43 is i386 / accept here!), getrusage = 98 */
#define SYS_times   100
#define SYS_getrusage 98
#define SYS_futex   202
#define SYS_clone   56
#define SYS_fork    57
#define SYS_vfork   58
#define SYS_execve  59
#define SYS_exit    60
#define SYS_wait4   61
#define SYS_kill    62
#define SYS_uname   63
#define SYS_getuid  102
#define SYS_getgid  104
#define SYS_geteuid 107
#define SYS_getegid 108
#define SYS_getppid 110
#define SYS_arch_prctl 158
#define SYS_fstatat 262
#define SYS_gettid  186
#define SYS_tkill   200
#define SYS_time    201
#define SYS_sysinfo 99
#define SYS_sched_getaffinity 203
#define SYS_set_tid_address 218
#define SYS_clock_gettime 228
#define SYS_clock_nanosleep 230
#define SYS_exit_group 231
#define SYS_tgkill  234
#define SYS_prlimit64 302
#define SYS_openat  257
#define SYS_set_robust_list 273
#define SYS_getrandom 318

// userspace porting set (busybox & friends)
#define SYS_mremap  25
#define SYS_access  21
#define SYS_pipe    22
#define SYS_symlink 88
#define SYS_link    86
#define SYS_dup     32
#define SYS_dup2    33
#define SYS_fcntl   72
#define SYS_ftruncate 77
#define SYS_getcwd  79
#define SYS_chdir   80
#define SYS_rename  82
#define SYS_mkdir   83
#define SYS_rmdir   84
#define SYS_unlink  87
#define SYS_readlink 89
#define SYS_chmod   90
#define SYS_fchmod  91
#define SYS_gettimeofday 96
#define SYS_getpgid 121
#define SYS_setpgid 109
#define SYS_sigaltstack 131
#define SYS_rt_sigpending 127
#define SYS_getdents64 217
#define SYS_faccessat 269
#define SYS_fchmodat 268
#define SYS_utimensat 280
#define SYS_dup3    292
#define SYS_pipe2   293

uint64_t syscall_dispatch(struct regs *r);
