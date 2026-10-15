#include <kernel.h>
#define HEAP_BASE 0xffffc00000000000UL
#define HEAP_LIMIT 0xffffc10000000000UL
#define HEAP_MAGIC 0x6b6172696f686561UL
struct allocation {
    uint64_t magic;
    size_t capacity, size;
    struct allocation *next;
    bool free;
};
static struct allocation *allocations;
static uint64_t cursor = HEAP_BASE;
static spinlock_t lock = SPINLOCK_INIT;
void *kmemalign(size_t a, size_t n)
{
    if (a < 16)
        a = 16;
    if ((a & (a - 1)) || !n || n > SIZE_MAX - a - sizeof(struct allocation) - sizeof(void *))
        return NULL;
    size_t need = n + a + sizeof(struct allocation) + sizeof(void *);
    if (need > SIZE_MAX - 4095)
        return NULL;
    uint64_t f = irq_save();
    spin_lock(&lock);
    struct allocation *h = allocations;
    while (h && (!h->free || h->capacity < need))
        h = h->next;
    if (!h) {
        size_t bytes = ALIGN_UP(need, PAGE_SIZE);
        if (bytes > HEAP_LIMIT - cursor) {
            spin_unlock(&lock);
            irq_restore(f);
            return NULL;
        }
        uint64_t base = cursor;
        size_t mapped = 0;
        for (; mapped < bytes; mapped += PAGE_SIZE) {
            uint64_t p = frame_alloc();
            if (!p)
                break;
            if (page_map(kernel_root, base + mapped, p, PTE_W | PTE_NX)) {
                frame_put(p);
                break;
            }
        }
        if (mapped < bytes) {
            while (mapped) {
                mapped -= 4096;
                page_unmap(kernel_root, base + mapped, true);
            }
            spin_unlock(&lock);
            irq_restore(f);
            return NULL;
        }
        cursor += bytes;
        h = (void *)base;
        h->magic = HEAP_MAGIC;
        h->capacity = bytes;
        h->next = allocations;
        allocations = h;
    }
    h->free = false;
    h->size = n;
    void *p = (void *)ALIGN_UP((uintptr_t)(h + 1) + sizeof(void *), a);
    ((struct allocation **)p)[-1] = h;
    spin_unlock(&lock);
    irq_restore(f);
    return p;
}
void *kmalloc(size_t n)
{
    return kmemalign(16, n);
}
void *kzalloc(size_t n)
{
    void *p = kmalloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}
void kfree(void *p)
{
    if (!p)
        return;
    uint64_t f = irq_save();
    spin_lock(&lock);
    if ((uint64_t)p < HEAP_BASE || (uint64_t)p >= cursor)
        panic("Invalid heap pointer");
    struct allocation *h = ((struct allocation **)p)[-1];
    if ((uint64_t)h < HEAP_BASE || (uint64_t)h >= cursor || h->magic != HEAP_MAGIC || h->free)
        panic("Invalid/double heap free");
    h->free = true;
    spin_unlock(&lock);
    irq_restore(f);
}
void *krealloc(void *p, size_t n)
{
    if (!p)
        return kmalloc(n);
    if (!n) {
        kfree(p);
        return NULL;
    }
    struct allocation *h = ((struct allocation **)p)[-1];
    if (h->magic != HEAP_MAGIC || h->free)
        panic("Invalid realloc");
    if (n <= h->size) {
        h->size = n;
        return p;
    }
    void *q = kmalloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, h->size);
    kfree(p);
    return q;
}
