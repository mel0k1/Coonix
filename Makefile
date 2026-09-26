NASM    ?= nasm
CC      ?= gcc
LD      ?= ld
OBJCOPY ?= objcopy
XORRISO ?= xorriso
QEMU    ?= qemu-system-x86_64

LIMINE_DIR  ?= build/limine
BUILD       := build
KERNEL      := $(BUILD)/coonix.bin
ISO         := coonix.iso

KCFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
	   -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
	   -mcmodel=kernel -Wall -Wextra -O2 -Ikernel -MMD

UCFLAGS := -nostdlib -nostartfiles -static -no-pie -fno-pic -fno-pie \
	   -fno-stack-protector -fno-builtin -Wall -Wextra -O2 \
	   -I libc/include -MMD

KSRC  := $(wildcard kernel/*.c)
KASM  := $(wildcard kernel/*.asm)
KOBJ  := $(KSRC:%.c=$(BUILD)/%.o) $(KASM:%.asm=$(BUILD)/%.o)

USERS := shell hello
ULIBC := $(BUILD)/libc/string.o $(BUILD)/libc/stdio.o
UELF  := $(USERS:%=$(BUILD)/user/%.elf)
UBIN  := $(USERS:%=$(BUILD)/user/%_bin.o)

.PHONY: all iso run run-headless clean

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

# embed elf binaries into the kernel image
$(BUILD)/user/%_bin.o: $(BUILD)/user/%.elf
	cd $(BUILD)/user && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 $*.elf $*_bin.o

$(KERNEL): $(KOBJ) $(UBIN) linker.ld
	$(LD) -nostdlib -static -T linker.ld -z max-page-size=0x1000 \
	    -o $@ $(KOBJ) $(UBIN)

# --- iso ---

$(LIMINE_DIR)/limine: $(LIMINE_DIR)/limine.c
	$(CC) -O2 $< -o $@

$(LIMINE_DIR)/limine.c $(LIMINE_DIR)/limine-bios.sys:
	git clone --depth=1 --branch=v9.x-binary \
	    https://github.com/limine-bootloader/limine.git $(LIMINE_DIR)

iso: $(KERNEL) limine.conf $(LIMINE_DIR)/limine.c $(LIMINE_DIR)/limine
	@mkdir -p $(BUILD)/iso_root/boot $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/coonix.bin
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

run: iso
	$(QEMU) -M q35 -m 2G -cdrom $(ISO) -display gtk

run-headless: iso
	$(QEMU) -M q35 -m 2G -cdrom $(ISO) -display none -serial stdio

clean:
	rm -rf $(BUILD) $(ISO)

-include $(KOBJ:.o=.d) $(ULIBC:.o=.d)
