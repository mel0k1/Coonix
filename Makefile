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

USERS := shell hello forktest mtest fstest
ULIBC := $(BUILD)/libc/string.o $(BUILD)/libc/stdio.o
UELF  := $(USERS:%=$(BUILD)/user/%.elf)

.PHONY: all iso disk run run-headless run-disk run-disk-headless clean

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

# --- disk: real ext2 image for the ata driver ---

DISK := $(BUILD)/disk.img

$(DISK): $(USERS:%=$(RDISK_ROOT)/bin/%) $(RDISK_ROOT)/etc/motd tools/mkdisk.py
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
