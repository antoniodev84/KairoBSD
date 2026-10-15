#include <kernel.h>
static bool nx;
static inline void invlpg(uint64_t v)
{
    __asm__ volatile("invlpg (%0)" ::"r"(v) : "memory");
}
void vmm_init(void)
{
    uint32_t a, b, c, d;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        nx = d & (1U << 20);
    }
    if (!nx)
        panic("NX is required for process isolation");
    wrmsr(0xc0000080, rdmsr(0xc0000080) | (1U << 11));
    __asm__ volatile("mov %%cr3,%0" : "=r"(kernel_root));
    kernel_root &= PTE_ADDR;
    uint64_t cr0;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    cr0 |= 1U << 16;
    __asm__ volatile("mov %0,%%cr0" ::"r"(cr0) : "memory");
    uint64_t *r = phys_ptr(kernel_root);
    for (unsigned i = 256; i < 512; i++) {
        r[i] &= ~PTE_U;
        if (!(r[i] & PTE_P)) {
            uint64_t p = frame_alloc();
            if (!p)
                panic("PML4 allocation");
            r[i] = p | PTE_P | PTE_W;
        }
    }
    kprintf("VMM: four-level paging, NX and CR0.WP enabled\n");
}
uint64_t *page_entry(uint64_t root, uint64_t v, bool create)
{
    if (v >= USER_TOP && v < 0xffff800000000000UL)
        return NULL;
    uint64_t *t = phys_ptr(root);
    for (int level = 3; level > 0; level--) {
        unsigned i = (v >> (12 + level * 9)) & 511;
        if (t[i] & 128) {
            if (!create)
                return NULL;
            uint64_t p = frame_alloc();
            if (!p)
                return NULL;
            uint64_t old = t[i], span = 1UL << (12 + (level - 1) * 9),
                     base = old & PTE_ADDR & ~((span * 512) - 1), flags = old & ~PTE_ADDR;
            if (level == 1) {
                flags &= ~128UL;
                if (old & (1UL << 12))
                    flags |= 128UL;
            }
            uint64_t *child = phys_ptr(p);
            for (unsigned j = 0; j < 512; j++)
                child[j] = (base + j * span) | flags;
            t[i] = p | PTE_P | PTE_W | (old & PTE_U);
            invlpg(v);
        }
        if (!(t[i] & PTE_P)) {
            if (!create)
                return NULL;
            uint64_t p = frame_alloc();
            if (!p)
                return NULL;
            t[i] = p | PTE_P | PTE_W | (v < USER_TOP ? PTE_U : 0);
        }
        t = phys_ptr(t[i] & PTE_ADDR);
    }
    return &t[(v >> 12) & 511];
}
int page_map(uint64_t root, uint64_t v, uint64_t p, uint64_t flags)
{
    if ((v | p) & 4095)
        return -EINVAL;
    uint64_t *e = page_entry(root, v, true);
    if (!e)
        return -ENOMEM;
    if (*e & PTE_P)
        return -EEXIST;
    *e = p | flags | PTE_P;
    invlpg(v);
    return 0;
}
uint64_t page_translate(uint64_t root, uint64_t v)
{
    uint64_t *t = phys_ptr(root);
    for (int level = 3; level >= 0; level--) {
        uint64_t e = t[(v >> (12 + level * 9)) & 511];
        if (!(e & PTE_P))
            return 0;
        if (level && (e & 128)) {
            uint64_t mask = (1UL << (12 + level * 9)) - 1;
            return (e & PTE_ADDR & ~mask) | (v & mask);
        }
        if (!level)
            return (e & PTE_ADDR) | (v & 4095);
        t = phys_ptr(e & PTE_ADDR);
    }
    return 0;
}
void page_unmap(uint64_t root, uint64_t v, bool release)
{
    uint64_t *e = page_entry(root, v, false);
    if (e && (*e & PTE_P)) {
        uint64_t p = *e & PTE_ADDR;
        *e = 0;
        invlpg(v);
        if (release)
            frame_put(p);
    }
}
uint64_t space_new(void)
{
    uint64_t p = frame_alloc();
    if (!p)
        return 0;
    memcpy((uint64_t *)phys_ptr(p) + 256, (uint64_t *)phys_ptr(kernel_root) + 256, 2048);
    return p;
}
static void destroy_table(uint64_t p, unsigned level)
{
    uint64_t *t = phys_ptr(p);
    for (unsigned i = 0; i < 512; i++)
        if (t[i] & PTE_P) {
            if (level)
                destroy_table(t[i] & PTE_ADDR, level - 1);
            else
                frame_put(t[i] & PTE_ADDR);
        }
    frame_put(p);
}
void space_destroy(uint64_t r)
{
    if (!r || r == kernel_root)
        return;
    uint64_t *t = phys_ptr(r);
    for (unsigned i = 0; i < 256; i++)
        if (t[i] & PTE_P)
            destroy_table(t[i] & PTE_ADDR, 2);
    frame_put(r);
}
static int clone_table(uint64_t src, uint64_t dst, unsigned level, uint64_t prefix)
{
    uint64_t *s = phys_ptr(src), *d = phys_ptr(dst);
    for (unsigned i = 0; i < 512; i++)
        if (s[i] & PTE_P) {
            uint64_t v = prefix | ((uint64_t)i << (12 + level * 9));
            if (level) {
                uint64_t p = frame_alloc();
                if (!p)
                    return -ENOMEM;
                d[i] = p | PTE_P | PTE_W | PTE_U;
                if (clone_table(s[i] & PTE_ADDR, p, level - 1, v))
                    return -ENOMEM;
            } else {
                if (s[i] & PTE_W) {
                    s[i] = (s[i] & ~PTE_W) | PTE_COW;
                    invlpg(v);
                }
                frame_get(s[i] & PTE_ADDR);
                d[i] = s[i];
            }
        }
    return 0;
}
uint64_t space_clone(uint64_t src)
{
    uint64_t dst = space_new();
    if (!dst)
        return 0;
    uint64_t *s = phys_ptr(src), *d = phys_ptr(dst);
    for (unsigned i = 0; i < 256; i++)
        if (s[i] & PTE_P) {
            uint64_t p = frame_alloc();
            if (!p) {
                space_destroy(dst);
                return 0;
            }
            d[i] = p | PTE_P | PTE_W | PTE_U;
            if (clone_table(s[i] & PTE_ADDR, p, 2, (uint64_t)i << 39)) {
                space_destroy(dst);
                return 0;
            }
        }
    return dst;
}
bool cow_fault(uint64_t root, uint64_t v)
{
    uint64_t *e = page_entry(root, v, false);
    if (!e || (*e & (PTE_P | PTE_U | PTE_COW)) != (PTE_P | PTE_U | PTE_COW))
        return false;
    uint64_t old = *e & PTE_ADDR;
    if (frame_refs(old) == 1)
        *e = (*e | PTE_W) & ~PTE_COW;
    else {
        uint64_t p = frame_alloc();
        if (!p)
            return false;
        memcpy(phys_ptr(p), phys_ptr(old), 4096);
        *e = p | ((*e & ~PTE_ADDR & ~PTE_COW) | PTE_W);
        frame_put(old);
    }
    invlpg(v);
    return true;
}
bool user_range(uint64_t root, uint64_t p, size_t n, bool write)
{
    if (p < PAGE_SIZE || p >= USER_TOP || n > USER_TOP - p)
        return false;
    if (!n)
        return true;
    uint64_t end = p + n - 1;
    for (uint64_t v = ALIGN_DOWN(p, PAGE_SIZE); v <= end; v += PAGE_SIZE) {
        uint64_t *e = page_entry(root, v, false);
        if (!e || (*e & (PTE_P | PTE_U)) != (PTE_P | PTE_U))
            return false;
        if (write && !(*e & PTE_W) && !cow_fault(root, v))
            return false;
    }
    return true;
}
int copy_in(void *d, uint64_t s, size_t n)
{
    struct task *t = this_cpu()->current;
    if (!user_range(t->root, s, n, false))
        return -EFAULT;
    memcpy(d, (void *)s, n);
    return 0;
}
int copy_out(uint64_t d, const void *s, size_t n)
{
    struct task *t = this_cpu()->current;
    if (!user_range(t->root, d, n, true))
        return -EFAULT;
    memcpy((void *)d, s, n);
    return 0;
}
int copy_string(char *d, uint64_t s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (copy_in(d + i, s + i, 1))
            return -EFAULT;
        if (!d[i])
            return 0;
    }
    return -ENAMETOOLONG;
}
void *mmio_map(uint64_t p, size_t n)
{
    if (!n || p > UINT64_MAX - n)
        return NULL;
    uint64_t end = ALIGN_UP(p + n, PAGE_SIZE);
    for (uint64_t a = ALIGN_DOWN(p, PAGE_SIZE); a < end; a += PAGE_SIZE) {
        uint64_t *e = page_entry(kernel_root, hhdm + a, true);
        if (!e)
            panic("MMIO page table allocation");
        *e = a | PTE_P | PTE_W | PTE_NX | 24;
        invlpg(hhdm + a);
    }
    return phys_ptr(p);
}
