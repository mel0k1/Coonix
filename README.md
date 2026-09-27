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
- [x] настоящий диск: ATA PIO LBA48 драйвер + ext2 (read-only) поверх VFS
- [x] mmap (anonymous) / mprotect / munmap / brk — задел под glibc
- [ ] запись на диск (ext2 rw)
- [ ] AHCI/NVMe, tmpfs поверх VFS
- [ ] портирование glibc

## Как это устроено

```
kernel/   ядро: загрузка, консоль, память, планировщик, vfs, ata, ext2, syscalls
libc/     мини-libc для юзерспейса (пока gcc -nostdlib)
user/     программы: shell, hello, forktest, mtest
ramdisk/  общий staging: попадает и в initramfs, и на диск (etc/motd; bin/ при сборке)
tools/    mkdisk.py — сборка ext2-образа без root, checkdisk.py — проверка образа
```

Ядро грузится Limine на `0xffffffff80000000` (higher half), физическая база
`0x200000`.

**Настоящий диск.** `make run` поднимает QEMU на `-M pc` (PIIX3 IDE с legacy
портами): ATA-драйвер в polling/LBA48 умеет `identify` и чтение секторов,
поверх него работает read-only ext2 (суперблок, group descriptors, иноды,
direct+indirect блоки, block cache), и он монтируется как корень. Образ диска
(`build/disk.img`) собирает `tools/mkdisk.py` из того же staging-каталога, что
и initramfs, — `execve` берёт `/bin/*` уже с настоящего диска. Если диска с
ext2 нет, ядро откатывается на tmpfs + initramfs (ustar-модуль Limine).

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
