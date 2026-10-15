#include <kernel.h>
struct ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstr;
};
struct phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
static int read_exact(struct vnode *n, uint64_t off, void *p, size_t len)
{
    int r = n->ops->read(n, off, p, len);
    return r == (int)len ? 0 : (r < 0 ? r : -ENOEXEC);
}
static int image_copy(uint64_t root, uint64_t v, const void *p, size_t len)
{
    const uint8_t *s = p;
    while (len) {
        uint64_t a = page_translate(root, v);
        if (!a)
            return -EFAULT;
        size_t n = 4096 - (v & 4095);
        if (n > len)
            n = len;
        memcpy(phys_ptr(a), s, n);
        s += n;
        v += n;
        len -= n;
    }
    return 0;
}
int elf_load(struct task *t, struct vnode *node, const char *const *argv, const char *const *envp,
             struct frame *f)
{
    struct ehdr h;
    if (read_exact(node, 0, &h, sizeof(h)))
        return -ENOEXEC;
    if (memcmp(h.ident, "\177ELF", 4) || h.ident[4] != 2 || h.ident[5] != 1 || h.ident[6] != 1 ||
        h.type != 2 || h.machine != 62 || h.version != 1 || h.ehsize != sizeof(h) ||
        h.phentsize != sizeof(struct phdr) || !h.phnum || h.phnum > 128 || h.phoff > node->size ||
        (uint64_t)h.phnum * sizeof(struct phdr) > node->size - h.phoff || h.entry < PAGE_SIZE ||
        h.entry >= USER_STACK - 65536)
        return -ENOEXEC;
    bool executable = false;
    uint8_t buf[4096];
    for (unsigned i = 0; i < h.phnum; i++) {
        struct phdr p;
        if (read_exact(node, h.phoff + i * sizeof(p), &p, sizeof(p)))
            return -ENOEXEC;
        if (p.type == 3 || p.type == 2)
            return -ENOEXEC;
        if (p.type != 1)
            continue;
        if (!p.memsz) {
            if (p.filesz)
                return -ENOEXEC;
            continue;
        }
        if (p.filesz > p.memsz || p.offset > node->size || p.filesz > node->size - p.offset ||
            p.vaddr < PAGE_SIZE || p.vaddr >= USER_STACK - 65536 ||
            p.memsz > USER_STACK - 65536 - p.vaddr || ((p.vaddr - p.offset) & 4095) ||
            (p.align > 1 &&
             ((p.align & (p.align - 1)) || (p.vaddr % p.align != p.offset % p.align))) ||
            (p.flags & 3) == 3)
            return -ENOEXEC;
        uint64_t flags = PTE_U | ((p.flags & 2) ? PTE_W : 0) | ((p.flags & 1) ? 0 : PTE_NX);
        for (uint64_t v = ALIGN_DOWN(p.vaddr, 4096); v < ALIGN_UP(p.vaddr + p.memsz, 4096);
             v += 4096) {
            uint64_t phys = frame_alloc();
            if (!phys)
                return -ENOMEM;
            int r = page_map(t->root, v, phys, flags);
            if (r) {
                frame_put(phys);
                return r == -EEXIST ? -ENOEXEC : r;
            }
        }
        for (uint64_t off = 0; off < p.filesz;) {
            size_t n = p.filesz - off;
            if (n > sizeof(buf))
                n = sizeof(buf);
            if (read_exact(node, p.offset + off, buf, n) ||
                image_copy(t->root, p.vaddr + off, buf, n))
                return -ENOEXEC;
            off += n;
        }
        if ((p.flags & 1) && h.entry >= p.vaddr && h.entry < p.vaddr + p.memsz)
            executable = true;
    }
    if (!executable)
        return -ENOEXEC;
    for (uint64_t v = USER_STACK - 65536; v < USER_STACK; v += 4096) {
        uint64_t p = frame_alloc();
        if (!p)
            return -ENOMEM;
        int r = page_map(t->root, v, p, PTE_U | PTE_W | PTE_NX);
        if (r) {
            frame_put(p);
            return r;
        }
    }
    uint64_t av[33], ev[33], sp = USER_STACK;
    unsigned ac = 0, ec = 0;
    for (unsigned kind = 0; kind < 2; kind++) {
        const char *const *list = kind ? envp : argv;
        unsigned *count = kind ? &ec : &ac;
        uint64_t *ptrs = kind ? ev : av;
        if (list)
            while (list[*count]) {
                if (*count == 32)
                    return -E2BIG;
                size_t len = strlen(list[*count]) + 1;
                if (len > 4096 || sp - len < USER_STACK - 60000)
                    return -E2BIG;
                sp -= len;
                image_copy(t->root, sp, list[*count], len);
                ptrs[(*count)++] = sp;
            }
        ptrs[*count] = 0;
    }
    uint64_t words[70];
    unsigned n = 0;
    words[n++] = ac;
    for (unsigned i = 0; i <= ac; i++)
        words[n++] = av[i];
    for (unsigned i = 0; i <= ec; i++)
        words[n++] = ev[i];
    words[n++] = 0;
    words[n++] = 0;
    sp = ALIGN_DOWN(sp - n * 8, 16);
    image_copy(t->root, sp, words, n * 8);
    memset(f, 0, sizeof(*f));
    f->rip = h.entry;
    f->rsp = sp;
    f->cs = 35;
    f->ss = 27;
    f->flags = 0x202;
    f->rdi = ac;
    f->rsi = sp + 8;
    f->rdx = sp + 8 * (ac + 2);
    return 0;
}
