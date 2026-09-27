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
- [ ] портирование glibc дальше: полноценный libc userspace, dlopen и т.п.

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

Syscall-интерфейс: `int 0x80`, номера как в Linux — это задел
под будущую glibc: когда ядро научится делать `mmap/brk/clone/...`,
юзерспейс-бинарники с glibc смогут запускаться как есть.

## Лицензия

GPL-3.0 — см. [LICENSE](LICENSE). `kernel/limine.h` — BSD Zero Clause (Limine),
`kernel/font8x8.h` — Public Domain.
