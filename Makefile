NASM    ?= nasm
CC      ?= gcc
LD      ?= ld
OBJCOPY ?= objcopy
XORRISO ?= xorriso
TAR     ?= tar
QEMU    ?= qemu-system-x86_64

LIMINE_DIR  ?= build/limine
BUILD       := build
KERNEL      := $(BUILD)/coonix.bin
ISO         := coonix.iso
RAMDISK     := $(BUILD)/initramfs.tar

KCFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
	   -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
	   -mcmodel=kernel -Wall -Wextra -O2 -Ikernel -MMD

UCFLAGS := -nostdlib -nostartfiles -static -no-pie -fno-pic -fno-pie \
	   -fno-stack-protector -fno-builtin -Wall -Wextra -O2 \
	   -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
	   -I libc/include -MMD

KSRC  := $(wildcard kernel/*.c)
KASM  := $(wildcard kernel/*.asm)
KOBJ  := $(KSRC:%.c=$(BUILD)/%.o) $(KASM:%.asm=$(BUILD)/%.o)

USERS := shell hello forktest mtest fstest dtest
ULIBC := $(BUILD)/libc/string.o $(BUILD)/libc/stdio.o
UELF  := $(USERS:%=$(BUILD)/user/%.elf)

.PHONY: all iso disk run run-headless run-disk run-disk-headless run-q35 run-q35-headless clean

# keep linked user binaries around (make deletes intermediates otherwise)
.PRECIOUS: $(UELF) $(BUILD)/user/%.o

all: $(KERNEL)

$(BUILD)/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/kernel/%.o: kernel/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

# --- user space ---

$(BUILD)/user/crt0.o: libc/src/crt0.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(BUILD)/libc/%.o: libc/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/%.elf: $(BUILD)/user/%.o $(BUILD)/user/crt0.o $(ULIBC)
	$(LD) -nostdlib -static -z max-page-size=0x1000 --build-id=none \
	    -Ttext=0x400000 -e _start -o $@ $^

$(KERNEL): $(KOBJ) linker.ld
	$(LD) -nostdlib -static -T linker.ld -z max-page-size=0x1000 \
	    -o $@ $(KOBJ)

# --- initramfs: user binaries ride as a ustar module, kernel unpacks it ---

RDISK_ROOT := $(BUILD)/initramfs_root

$(RDISK_ROOT)/bin/%: $(BUILD)/user/%.elf
	@mkdir -p $(dir $@)
	cp $< $@

$(RDISK_ROOT)/etc/motd: ramdisk/etc/motd
	@mkdir -p $(dir $@)
	cp $< $@

$(RAMDISK): $(USERS:%=$(RDISK_ROOT)/bin/%) $(RDISK_ROOT)/etc/motd
	$(TAR) --format=ustar -C $(RDISK_ROOT) -cf $@ bin etc

# --- glibc: static host-glibc binary, best effort (needs libc.a) ---

LIBSTAMP := $(RDISK_ROOT)/lib64/.stamp

GLIBC_PROGS := $(RDISK_ROOT)/bin/glibc_hello $(RDISK_ROOT)/bin/hello_dyn \
	$(RDISK_ROOT)/bin/pthreadtest $(RDISK_ROOT)/bin/sigtest \
	$(RDISK_ROOT)/bin/iotest \
	$(LIBSTAMP)

$(RDISK_ROOT)/bin/glibc_hello: user/glibc_hello.c
	@if gcc -static -O2 -o $@ $< 2>/dev/null; then \
	        echo "glibc: built $@"; \
	else \
	        rm -f $@; echo "glibc: no static libc, skipping"; \
	fi

# dynamically linked hello: needs the real ld.so + libc on the disk
$(RDISK_ROOT)/bin/hello_dyn: user/glibc_hello.c
	@if gcc -O2 -o $@ $< 2>/dev/null; then \
	        echo "glibc: built $@"; \
	else \
	        rm -f $@; echo "glibc: dynamic hello skipped"; \
	fi

# glibc feature tests: threads/signals/tty through the real libc
$(RDISK_ROOT)/bin/pthreadtest: user/pthreadtest.c
	@if gcc -O2 -Wall -o $@ $< 2>/dev/null; then \
	        echo "glibc: built $@"; \
	else \
	        rm -f $@; echo "glibc: pthreadtest skipped"; \
	fi

$(RDISK_ROOT)/bin/sigtest: user/sigtest.c
	@if gcc -O2 -Wall -o $@ $< 2>/dev/null; then \
	        echo "glibc: built $@"; \
	else \
	        rm -f $@; echo "glibc: sigtest skipped"; \
	fi

$(RDISK_ROOT)/bin/iotest: user/iotest.c
	@if gcc -O2 -Wall -o $@ $< 2>/dev/null; then \
	        echo "glibc: built $@"; \
	else \
	        rm -f $@; echo "glibc: iotest skipped"; \
	fi

$(RDISK_ROOT)/lib64/.stamp:
	@mkdir -p $(RDISK_ROOT)/lib64 $(RDISK_ROOT)/lib/x86_64-linux-gnu
	@for d in lib64 lib/x86_64-linux-gnu; do \
	        for f in ld-linux-x86-64.so.2 libc.so.6; do \
	                cp /usr/lib/x86_64-linux-gnu/$$f $(RDISK_ROOT)/$$d/ 2>/dev/null || true; \
	        done; \
	done
	@touch $@


# --- disk: real ext2 image for the ata driver ---

DISK := $(BUILD)/disk.img

$(DISK): $(USERS:%=$(RDISK_ROOT)/bin/%) $(RDISK_ROOT)/etc/motd $(GLIBC_PROGS) tools/mkdisk.py
	python3 tools/mkdisk.py $(RDISK_ROOT) $@

disk: $(DISK)

# --- iso ---

$(LIMINE_DIR)/limine: $(LIMINE_DIR)/limine.c
	$(CC) -O2 $< -o $@

$(LIMINE_DIR)/limine.c $(LIMINE_DIR)/limine-bios.sys:
	git clone --depth=1 --branch=v9.x-binary \
	    https://github.com/limine-bootloader/limine.git $(LIMINE_DIR)

iso: $(KERNEL) $(RAMDISK) limine.conf $(LIMINE_DIR)/limine.c $(LIMINE_DIR)/limine
	@mkdir -p $(BUILD)/iso_root/boot $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/coonix.bin
	cp $(RAMDISK) $(BUILD)/iso_root/boot/initramfs.tar
	cp limine.conf $(BUILD)/iso_root/
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin \
	   $(LIMINE_DIR)/limine-uefi-cd.bin $(BUILD)/iso_root/boot/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(BUILD)/iso_root/EFI/BOOT/
	$(XORRISO) -as mkisofs -R -r -J -b boot/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
	    -apm-block-size 2048 --efi-boot boot/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    -o $(ISO) $(BUILD)/iso_root
	$(LIMINE_DIR)/limine bios-install $(ISO)
	@echo "ISO ready: $(ISO)"

# -M pc: piix3 ide with legacy ports, that is what the ata driver talks to
run: iso disk
	$(QEMU) -M pc -m 2G -cdrom $(ISO) -drive file=$(DISK),format=raw,if=ide,index=0 -display gtk

run-headless: iso disk
	$(QEMU) -M pc -m 2G -cdrom $(ISO) -drive file=$(DISK),format=raw,if=ide,index=0 -display none -serial stdio

clean:
	rm -rf $(BUILD) $(ISO)

-include $(KOBJ:.o=.d) $(ULIBC:.o=.d)
run-q35: iso disk
	$(QEMU) -M q35 -m 2G -cdrom $(ISO) -drive file=$(DISK),format=raw,if=none,id=hd0 -device ide-hd,drive=hd0,bus=ahci.0 -display gtk

run-q35-headless: iso disk
	$(QEMU) -M q35 -m 2G -cdrom $(ISO) -drive file=$(DISK),format=raw,if=none,id=hd0 -device ide-hd,drive=hd0,bus=ahci.0 -display none -serial stdio
