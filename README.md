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
- [x] syscalls в стиле Linux: read/write/fork/exec/wait/exit
- [x] юзерспейс: мини-libc, shell, программы
- [ ] VFS и файловая система
- [ ] копирование страниц при fork (COW)
- [ ] портирование glibc

## Как это устроено

```
kernel/   ядро: загрузка, консоль, память, планировщик, syscalls
libc/     мини-libc для юзерспейса (пока gcc -nostdlib)
user/     программы: shell, hello
```

Ядро грузится Limine на `0xffffffff80000000` (higher half), физическая база
`0x200000`. Syscall-интерфейс: `int 0x80`, номера как в Linux — это задел
под будущую glibc: когда ядро научится делать `mmap/brk/clone/...`,
юзерспейс-бинарники с glibc смогут запускаться как есть.

## Лицензия

GPL-3.0 — см. [LICENSE](LICENSE). `kernel/limine.h` — BSD Zero Clause (Limine),
`kernel/font8x8.h` — Public Domain.
