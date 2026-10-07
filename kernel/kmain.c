#include <stdint.h>
#include "limine.h"
#include "kernel.h"
#include "console.h"
#include "serial.h"
#include "string.h"
#include "enot.h"
#include "gdt.h"
#include "idt.h"
#include "pit.h"
#include "kbd.h"
#include "signal.h"
#include "task.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "vfs.h"
#include "tmpfs.h"
#include "initramfs.h"
#include "ata.h"
#include "ahci.h"
#include "virtio_blk.h"
#include "ext2.h"
#include "blkdev.h"
#include "procfs.h"

// --- limine boot protocol requests ---

__attribute__((used, section(".limine_requests_start")))
LIMINE_REQUESTS_START_MARKER

__attribute__((used, section(".limine_requests")))
LIMINE_BASE_REVISION(0)

__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST, .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST, .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_stack_size_request stack_size_request = {
    .id = LIMINE_STACK_SIZE_REQUEST, .revision = 0,
    .stack_size = 0x40000
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST, .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_kernel_address_request ka_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST, .revision = 0
};

__attribute__((used, section(".limine_requests_end")))
LIMINE_REQUESTS_END_MARKER

// --- helpers ---

static uint64_t hhdm;

// sse/x87 for user space: glibc binaries use xmm freely
static void enable_fpu_sse(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1UL << 2);   // clear EM (emulate fpu -> #ud)
    cr0 |= (1UL << 1);    // set MP
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1UL << 9);    // OSFXSR: save/restore xmm in fxsave area
    cr4 |= (1UL << 17);   // OSXMMEXCPT: #XF instead of illegal opcode
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    __asm__ volatile("fninit");
}

extern void syscall_entry(void);

// enable the native syscall instruction (linux binaries use it)
static void syscall_init(void) {
    uint64_t efer = rdmsr(0xC0000080);
    wrmsr(0xC0000080, efer | 1);   // EFER.SCE
    // STAR: kernel cs/ss at 32..47, user cs/ss at 48..63 (ss = cs + 8)
    wrmsr(0xC0000081,
          ((uint64_t)SEL_UCODE << 48) | ((uint64_t)SEL_KCODE << 32));
    wrmsr(0xC0000082, (uint64_t)&syscall_entry);            // LSTAR
    // FMASK: rflags &= FMASK on entry (linux value)
    wrmsr(0xC0000084, 0x257FD5UL);
}

static void print_num(uint64_t v) {
    char buf[21];
    int i = 20;
    buf[i] = 0;
    if (!v)
        buf[--i] = '0';
    while (v) {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    }
    console_puts(&buf[i]);
}

uint64_t hhdm_offset(void) { return hhdm; }
void *phys2virt(uint64_t phys) { return (void *)(phys + hhdm); }
uint64_t virt2phys(void *virt) { return (uint64_t)virt - hhdm; }

void panic(const char *msg) {
    cli();
    console_set_fg(0xff5555);
    console_puts("\nKERNEL PANIC: ");
    console_puts(msg ? msg : "unknown");
    console_puts("\nsystem halted.\n");
    for (;;)
        hlt();
}

// --- entry ---

void kmain(void) {
    hhdm = hhdm_request.response ? hhdm_request.response->offset : 0;

    serial_init();
    console_init();

    console_set_fg(CONSOLE_ACCENT);
    console_puts(ENOT_BANNER);
    console_set_fg(CONSOLE_FG);

    if (!fb_request.response || fb_request.response->framebuffer_count < 1)
        serial_puts("console: no framebuffer, serial only\n");

    console_puts("limine says hi, kernel is alive\n");

    enable_nxe(); // we use NX bit in page tables
    enable_fpu_sse();
    gdt_init();
    idt_init();
    syscall_init();
    pit_init(100);
    kbd_init();
    console_puts("gdt, idt, pit, kbd ready\n");
    __asm__ volatile("sti");

    pmm_init();
    // keep pmm hands off the kernel image and boot modules (initramfs tar
    // lives in a reclaimable region we read later)
    if (ka_request.response) {
        extern char __kernel_end;
        pmm_reserve_range(ka_request.response->physical_base,
                          (uint64_t)&__kernel_end - ka_request.response->virtual_base);
    }
    if (module_request.response) {
        for (uint64_t m = 0; m < module_request.response->module_count; m++) {
            struct limine_file *lf = module_request.response->modules[m];
            pmm_reserve_range((uint64_t)lf->address - hhdm,
                              (lf->size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
        }
    }
    console_puts("memory: ");
    print_num(pmm_free_mem() >> 20);
    console_puts("/");
    print_num(pmm_total_mem() >> 20);
    console_puts(" MB free\n");

    idt_setup_ist();   // df/nmi/mc get private stacks from here on
    heap_init();
    // smoke test the heap
    void *a = kmalloc(1234);
    void *b = kzalloc(2048);
    kfree(a);
    kfree(b);
    console_puts("heap ok\n");

    vfs_init();
    // virtio first (fast dma when the disk rides on it), then ahci
    // (q35 has no legacy ide), then ata pio (pc machine)
    if (virtio_blk_init() == 0 && root_disk.ready) {
        console_puts("disk: ");
        console_puts(root_disk.name);
        console_puts(", ");
        print_num(root_disk.sectors >> 11);
        console_puts(" MB, virtio dma\n");
    } else if (ahci_init() == 0 && root_disk.ready) {
        console_puts("disk: ");
        console_puts(root_disk.name);
        console_puts(", ");
        print_num(root_disk.sectors >> 11);
        console_puts(" MB, ahci dma\n");
    } else if (ata_init() && root_disk.ready) {
        console_puts("disk: ");
        console_puts(root_disk.name);
        console_puts(", ");
        print_num(root_disk.sectors >> 11);
        console_puts(" MB, pio lba48\n");
    } else {
        console_puts("disk: none found\n");
    }

    if (root_disk.ready && ext2_mount_root() == 0) {
        console_puts("vfs: ext2 root mounted from disk\n");
    } else {
        tmpfs_mount();
        initramfs_load();
        console_puts("vfs: tmpfs root + initramfs (disk boot failed)\n");
    }

    // /proc over the root, whatever it is (procfs root has no deps)
    if (vfs_mount_at("/proc", procfs_mount(vfs_get_root())) == 0)
        console_puts("vfs: procfs mounted at /proc\n");

    task_init();
    signal_init();
    struct task *shell = task_spawn_user("/bin/shell", current);
    if (!shell)
        panic("no shell");
    // the shell owns the console foreground group (ISIG ctrl-C target)
    shell->pgid = shell->tgid;
    tty_fg_pgid = shell->pgid;
    console_puts("ring 3 shell is up, kernel idles now\n");

    // move the idle task onto its own kernel stack: every tick saves the
    // interrupted frame at current->rsp, and the boot stack pages below the
    // image are ordinary free memory for the PMM — a big allocation
    // (pthread stacks) would clobber a saved idle frame and resurrect a
    // random user context with kernel cr3
    __asm__ volatile("mov %0, %%rsp" :: "r"(current->kstack_top) : "memory");
    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}
