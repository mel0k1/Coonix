# Coonix 🦝

```
   /\     /\
  /  \___/  \    Coonix
 |  o     o  |   x86_64 hobby OS
 |     ^     |   built for glibc
  \   \_/   /
   \_______/
```

Coonix — хобби-операционная система для x86_64. Загрузчик [Limine](https://github.com/limine-bootloader/limine),
ядро на C (freestanding) + немного NASM. Дальняя цель — портировать glibc,
поэтому syscalls с самого начала повторяют Linux ABI (номера и регистры).

## Сборка

Нужны: `gcc`, `nasm`, `make`, `xorriso`, `git`, для запуска — `qemu-system-x86_64`.

```sh
make          # соберёт ядро (build/coonix.bin)
make iso      # соберёт загрузочный coonix.iso (клонирует limine сам)
make run      # запустить в qemu
```

## Roadmap

- [x] загрузка через Limine, framebuffer-консоль
- [x] GDT/IDT, исключения, прерывания (PIT + PS/2 клавиатура)
- [x] память: физический менеджер, paging, kmalloc
- [x] планировщик, Ring 3
- [x] syscalls в стиле Linux: read/write/open/close/fork/exec/wait/exit
- [x] юзерспейс: мини-libc, shell, программы
- [x] VFS + tmpfs, initramfs (ustar-модуль Limine)
- [x] copy-on-write fork
- [x] настоящий диск: ATA PIO LBA48 драйвер + ext2 поверх VFS
- [x] mmap (anonymous) / mprotect / munmap / brk — задел под glibc
- [x] ext2 на запись: битовые карты, create/truncate, write-back кеш
- [x] AHCI (PCI SATA, DMA) для q35 + ATA на pc, общий blkdev-слой
- [x] file-backed mmap: ленивое заполнение из файла, MAP_SHARED writeback,
      MAP_FIXED с резкой регионов, поддиапазонные munmap/mprotect
- [x] **glibc запускается**: статические бинарники и динамические через
      настоящий ld.so + libc.so.6 с диска (auxv, TLS/arch_prctl, pread64,
      fstat/fstatat, writev, exit_group, getrandom)
- [x] **углубление ядра под glibc**: rt_sigaction/rt_sigprocmask/rt_sigreturn
      с доставкой сигналов в ring 3 (трамполин + sigreturn, SIGSEGV-хендлер
      ловит настоящий #PF), futex WAIT/WAKE/REQUEUE (очереди по физическому
      адресу слова) — на них работают настоящие pthread mutex/condvar,
      ioctl(терминал): termios TCGETS/TCSETS, TIOCGWINSZ, isatty — плюс
      канонический ввод с эхом и raw-режимом в клавиатурном драйвере,
      clone(CLONE_VM|THREAD|SETTLS|CHILD_CLEARTID) для pthread_create,
      нативный вход сисколлов через LSTAR/EFER.SCE (glibc вызывает syscall,
      а не int 0x80), наносон/kill/tgkill/set_tid_address
- [x] **dlopen работает**: glibc-программа грузит .so с диска в рантайме
      (dlopen/dlsym/dlclose через настоящий ld.so, релокации + .data/.bss
      в загруженном .so)
- [x] **fs-слой под userspace**: getdents64, chdir/getcwd (cwd в task),
      pipe/pipe2 (блокирующиеся кольцевые буферы + EPIPE/SIGPIPE),
      dup/dup2/dup3, fcntl (FD_CLOEXEC переживает exec правильно),
      rename (с заменой цели), mkdir/rmdir/unlink/link/chmod/ftruncate,
      sysinfo, faccessat, mremap (для realloc), gettimeofday, setpgid,
      TIOCGPGRP/TIOCSPGRP; argv/envp передаются через execve, симлинки
      ext2 (fast symlink) + разрешение в VFS, сигнатурный фикс wait4:
      ECHILD вместо вечного сна + атомарный скан зомби под cli
- [x] **busybox как userspace-набор**: собранный host-gcc против glibc
      (динамический, живёт на уже настроенном ld.so-пути), applet-симлинки
      на диске, sh с пайплайнами и редиректами, ls/cp/mv/rm/grep/cat и
      ещё ~30 апплетов; тесты: qbbtest.py, qfsxtest.py, qdltest.py
- [x] **procfs**: оверлей-монтирование /proc поверх корня (mount-таблица
      в VFS), /proc/<pid>/stat (linux-формат) / status / cmdline / cwd,
      ps-апплет поверх getdents64; коммит argv в task для cmdline
- [x] **W^X финализирован**: ELF-сегменты уже NX кроме PT_X, kernel
      heap/stacks NX, mmap/brk NX без PROT_EXEC; fork теперь сохраняет NX
      в COW-копиях (флаг копировался как pte&0xfff и терял бит 63), плюс
      wxtest: RW-страница не исполняется, mprotect rx/rw туда-обратно,
      .text только чтение, стек не исполняется
- [x] **учёт процессорного времени**: getrusage(98) (SELF = сумма по
      тред-группе, CHILDREN, THREAD) и times(100 — x86_64-номер; 43 это
      accept!) через настоящую glibc; utime/stime копятся в планировщике
      по ring-у прерванного кадра, cutime/cstime переезжают к родителю на
      exit; /proc/<pid>/stat теперь несёт utime/stime/cutime/cstime и
      starttime
- [x] **/proc/<pid>/fd**: fd-каталог с симлинками 0..N (ls -l показывает
      цели: /dev/console, pipe:[id], путь открытого файла — struct file
      хранит путь с момента open), fstatat научился AT_SYMLINK_NOFOLLOW
      (lstat больше не следует по линкам и не падает на /dev/console);
      тесты: qfdtest.py (fd/симлинки/пайпы + ps/fsx регрессии)
- [ ] портирование glibc дальше: полноценный libc userspace

## Как это устроено

```
kernel/   ядро: загрузка, консоль, память, планировщик, vfs, ata/ahci,
          blkdev, ext2, elf+ld.so, syscalls
libc/     мини-libc для юзерспейса (пока gcc -nostdlib)
user/     программы: shell, hello, forktest, mtest, fstest, dtest, glibc_hello
ramdisk/  общий staging: попадает и в initramfs, и на диск (etc/motd; bin/ при сборке)
tools/    mkdisk.py — сборка ext2-образа без root, checkdisk.py — проверка образа
```

Ядро грузится Limine на `0xffffffff80000000` (higher half), физическая база
`0x200000`.

**Настоящий диск.** `make run` поднимает QEMU на `-M pc` (legacy IDE порты),
`make run-q35` — на `-M q35` (ICH9 AHCI: PCI 00:1f.2, BAR5 MMIO, командные
листы и FIS в DMA). Оба драйвера сидят под общим blkdev-интерфейсом, поверх
которого работает ext2 уже **с записью** (суперблок, group descriptors,
иноды, direct+indirect блоки, битовые карты блоков/инодов, block+write-back
кеш). Образ диска (`build/disk.img`) собирает `tools/mkdisk.py` из того же
staging-каталога, что и initramfs, — `execve` берёт `/bin/*` уже с
настоящего диска. Если диска с ext2 нет, ядро откатывается на tmpfs +
initramfs (ustar-модуль Limine).

**Динамические glibc-бинарники.** `hello_dyn` собран обычным gcc (динамически
связан) и запускается на Coonix: ядро грузит `ld-linux-x86-64.so.2` по
PT_INTERP на фиксированную базу, передаёт auxv (AT_PHDR/AT_BASE/AT_ENTRY/
AT_RANDOM/AT_EXECFN/...), ld.so через pread64/mmap с диска грузит libc.so.6,
делает релокации и версионный биндинг символов — и программа печатает через
настоящую glibc (printf/malloc). file-backed mmap здесь несёт основную
нагрузку: ленивое заполнение страниц из файла по #PF, честная замена только
запрошенного диапазона при MAP_FIXED (регионы режутся, а не выбрасываются),
запись назад для MAP_SHARED.

`fork` работает через copy-on-write: страницы помечаются read-only
(софт-бит COW в PTE), refcounts ведутся в PMM, первый write ловит #PF,
и ядро приватизирует страницу. `forktest` это проверяет: трое детей
портят общую память, родительские данные остаются целыми.

**Сигналы, futex, pthreads, tty.** Сигналы доставляются «перезаписью» кадра:
на границе сисколла или по таймеру ядро строит glibc-совместимый rt_sigframe
на стеке пользователя, подменяет rip на хендлер, а возврат идёт через
rt_sigreturn — заблокированные сисколлы после хендлера просто переигрываются
(SA_RESTART-семантика). SIGSEGV при этом ловит настоящий page fault: ядро
мапит сегменты ELF с честными правами (W^X), запись в RO-страницу даёт #PF,
и siglongjmp из хендлера работает. futex держит очереди ожидания по
физическому адресу слова (потоки и процессы видят один ключ) — на нём стоят
pthread_mutex/pthread_cond из настоящей glibc. pthread_create идёт через
clone(CLONE_VM|CLONE_THREAD|CLONE_SETTLS|CLONE_PARENT_SETTID|
CLONE_CHILD_CLEARTID): общий pml4, свой kstack и TLS, join — по futex на
clear_child_tid. Клавиатура ведёт себя как tty: канонический режим с эхом
и Ctrl-C (ISIG), raw-режим через tcsetattr — iotest проверяет isatty,
tcgetattr, TIOCGWINSZ и оба режима чтения.

Syscall-интерфейс: `int 0x80` + нативный `syscall` (LSTAR, EFER.SCE), номера
как в Linux — glibc-бинарники вызывают сисколлы как на настоящем Linux.

## Лицензия

GPL-3.0 — см. [LICENSE](LICENSE). `kernel/limine.h` — BSD Zero Clause (Limine),
`kernel/font8x8.h` — Public Domain.
