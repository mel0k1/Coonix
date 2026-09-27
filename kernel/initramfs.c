#include "initramfs.h"
#include "kernel.h"
#include "limine.h"
#include "tmpfs.h"
#include "string.h"
#include "console.h"

extern volatile struct limine_module_request module_request; // kmain.c

#define TAR_BLOCK 512

// parse octal, space/zero terminated
static uint64_t tar_size(const char *s, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n && s[i]; i++)
        if (s[i] >= '0' && s[i] <= '7')
            v = v * 8 + (s[i] - '0');
    return v;
}

static const char *skip_dotslash(const char *p) {
    while (p[0] == '.' && p[1] == '/')
        p += 2;
    return p;
}

// normalize tar name into "/bin/foo": leading slash, no trailing slash
static void norm_path(const char *name, char *out, int max) {
    int o = 0;
    out[o++] = '/';
    const char *p = skip_dotslash(name);
    while (*p && o < max - 1)
        out[o++] = *p++;
    out[o] = 0;
    while (o > 1 && out[o - 1] == '/')
        out[--o] = 0; // strip trailing slash (dirs)
}

void initramfs_load(void) {
    volatile struct limine_module_response *resp = module_request.response;
    if (!resp || resp->module_count < 1)
        panic("initramfs: no module in limine.conf");

    char path[128];
    for (uint64_t m = 0; m < resp->module_count; m++) {
        struct limine_file *lf = resp->modules[m];
        console_puts("initramfs: module at 0x");
        // size print (hex) for sanity
        uint64_t sz = lf->size;
        char hb[17];
        static const char hex[] = "0123456789abcdef";
        for (int i = 15; i >= 0; i--) { hb[i] = hex[sz & 0xf]; sz >>= 4; }
        hb[16] = 0;
        console_puts(hb);
        console_puts(" bytes\n");

        const uint8_t *p = lf->address;
        uint64_t left = lf->size;

        while (left >= TAR_BLOCK) {
            // header: name[100] mode[8] uid[8] gid[8] size[12] ... typeflag@156 magic@257
            const char *name = (const char *)p;
            uint64_t fsize = tar_size((const char *)p + 124, 12);
            char typeflag = ((const char *)p)[156];
            const char *magic = (const char *)p + 257;

            if (strncmp(magic, "ustar", 5) != 0)
                break; // end of archive or junk

            norm_path(name, path, sizeof(path));
            if (strlen(path) > 1) {
                if (typeflag == '5') {
                    tmpfs_mkdir_p(path);
                } else if (typeflag == '0' || typeflag == 0) {
                    if (fsize && p + TAR_BLOCK + fsize > (const uint8_t *)lf->address + lf->size)
                        break; // truncated
                    tmpfs_put_file(path, p + TAR_BLOCK, fsize);
                }
            }

            uint64_t adv = TAR_BLOCK + ((fsize + TAR_BLOCK - 1) & ~(uint64_t)(TAR_BLOCK - 1));
            if (adv > left)
                break; // malformed tail
            p += adv;
            left -= adv;
        }
    }

    console_puts("initramfs: tmpfs loaded, ");
    char buf[16];
    int v = tmpfs_node_count(), i = 15;
    buf[i] = 0;
    if (!v) buf[--i] = '0';
    while (v) { buf[--i] = '0' + v % 10; v /= 10; }
    console_puts(&buf[i]);
    console_puts(" nodes\n");
}
