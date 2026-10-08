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

USERS := shell hello forktest mtest fstest dtest fsx ps nicetest dpagetest nettest tcptest msgtest
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

GLIBC_PROGS := $(RDISK_ROOT)/bin/hello_dyn \
	$(RDISK_ROOT)/bin/pthreadtest $(RDISK_ROOT)/bin/sigtest \
	$(RDISK_ROOT)/bin/sigwaittest $(RDISK_ROOT)/bin/iotest \
	$(RDISK_ROOT)/bin/dltest $(RDISK_ROOT)/bin/wxtest \
	$(RDISK_ROOT)/bin/timetest $(RDISK_ROOT)/lib/libfoo.so \
	$(LIBSTAMP)

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

# dlopen: runtime-loaded shared object via the real loader
$(RDISK_ROOT)/bin/dltest: user/dltest.c
	@if gcc -O2 -Wall -o $@ $< 2>/dev/null; then \
		echo "glibc: built $@"; \
	else \
		rm -f $@; echo "glibc: dltest skipped"; \
	fi

# W^X enforcement: NX on data, mprotect promote/demote, RO text
$(RDISK_ROOT)/bin/wxtest: user/wxtest.c
	@if gcc -O2 -Wall -o $@ $< 2>/dev/null; then \
		echo "glibc: built $@"; \
	else \
		rm -f $@; echo "glibc: wxtest skipped"; \
	fi

# cpu accounting: getrusage(98) and times(43) via the real libc
$(RDISK_ROOT)/bin/timetest: user/timetest.c
	@if gcc -O2 -Wall -o $@ $< 2>/dev/null; then \
		echo "glibc: built $@"; \
	else \
		rm -f $@; echo "glibc: timetest skipped"; \
	fi

# signal waits: pause / sigsuspend / sigtimedwait via the real libc
$(RDISK_ROOT)/bin/sigwaittest: user/sigwaittest.c
	@if gcc -O2 -Wall -o $@ $< 2>/dev/null; then \
		echo "glibc: built $@"; \
	else \
		rm -f $@; echo "glibc: sigwaittest skipped"; \
	fi

# freestanding .so (no DT_NEEDED) so the disk needs no extra libs
$(RDISK_ROOT)/lib/libfoo.so: user/libfoo.c
	@mkdir -p $(dir $@)
	@if gcc -shared -fPIC -nostdlib -O2 -o $@ $< 2>/dev/null; then \
		echo "glibc: built $@"; \
	else \
		rm -f $@; echo "glibc: libfoo skipped"; \
	fi

$(RDISK_ROOT)/lib64/.stamp:
	@mkdir -p $(RDISK_ROOT)/lib64 $(RDISK_ROOT)/lib/x86_64-linux-gnu
	@# interp path is hardcoded to /lib64; the rest lives only in the
	@# multiarch dir (ld.so default search path) to save disk space
	cp /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 $(RDISK_ROOT)/lib64/ 2>/dev/null || true
	@for f in libc.so.6 libm.so.6 libresolv.so.2; do \
		cp /usr/lib/x86_64-linux-gnu/$$f $(RDISK_ROOT)/lib/x86_64-linux-gnu/ 2>/dev/null || true; \
	done
	@touch $@

# --- busybox: host-gcc dynamic glibc build riding the existing ld.so path

BUSYBOX_VER := 1_36_1
BUSYBOX_DIR := build/busybox-$(BUSYBOX_VER)
BUSYBOX_TAR := build/busybox-$(BUSYBOX_VER).tar.gz
BB_APPLETS := sh ash ls cat echo pwd cp mv rm mkdir rmdir touch env sleep \
              head tail wc grep seq true false uname clear printf test \
              sync free date basename dirname which id whoami ln stat \
              cmp cut tr od xargs find nice

$(BUSYBOX_TAR):
	@mkdir -p build
	curl -sL --max-time 300 -o $@ \
	    https://github.com/mirror/busybox/archive/refs/tags/$(BUSYBOX_VER).tar.gz

$(BUSYBOX_DIR)/Makefile: $(BUSYBOX_TAR)
	tar xzf $< -C build

$(BUSYBOX_DIR)/busybox: $(BUSYBOX_DIR)/Makefile
	@if [ ! -f $(BUSYBOX_DIR)/.config ]; then \
		$(MAKE) -C $(BUSYBOX_DIR) defconfig; \
		sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' $(BUSYBOX_DIR)/.config; \
	fi
	$(MAKE) -C $(BUSYBOX_DIR) -j$$(nproc)
	strip $(BUSYBOX_DIR)/busybox

$(RDISK_ROOT)/bin/busybox: $(BUSYBOX_DIR)/busybox
	@mkdir -p $(dir $@)
	cp $< $@

# applet links: the kernel vfs resolves fast symlinks, busybox dispatches
# on basename(argv[0])
$(RDISK_ROOT)/bin/.bblinks: $(RDISK_ROOT)/bin/busybox
	@for a in $(BB_APPLETS); do ln -sf busybox $(RDISK_ROOT)/bin/$$a; done
	@touch $@

# scratch dir for the tests (ext2 dirs persist across runs)
$(RDISK_ROOT)/tmp/.keep:
	@mkdir -p $(RDISK_ROOT)/tmp
	@touch $@

# --- disk: real ext2 image for the ata driver ---

DISK := $(BUILD)/disk.img

$(DISK): $(USERS:%=$(RDISK_ROOT)/bin/%) $(RDISK_ROOT)/etc/motd $(GLIBC_PROGS) \
        $(RDISK_ROOT)/bin/.bblinks $(RDISK_ROOT)/tmp/.keep tools/mkdisk.py
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
