#include <kernel.h>
uint64_t hhdm, kernel_root;
static uint64_t pages, total_pages, free_pages, used_pages, hint;
static uint8_t *bitmap;
static uint16_t *refs;
static spinlock_t lock = SPINLOCK_INIT;
static bool bit(uint64_t i)
{
    return bitmap[i >> 3] & (1U << (i & 7));
}
static void mark(uint64_t i, bool v)
{
    if (v)
        bitmap[i >> 3] |= 1U << (i & 7);
    else
        bitmap[i >> 3] &= ~(1U << (i & 7));
}
void pmm_init(struct limine_memmap_response *m)
{
    uint64_t limit = 0;
    for (uint64_t i = 0; i < m->entry_count; i++) {
        struct limine_memmap_entry *e = m->entries[i];
        if (!e || e->length > UINT64_MAX - e->base)
            panic("Invalid memory map region");
        if (e->type == LIMINE_MEMMAP_USABLE || e->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE ||
            e->type == LIMINE_MEMMAP_ACPI_NVS || e->type == LIMINE_MEMMAP_BAD_MEMORY ||
            e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            e->type == LIMINE_MEMMAP_KERNEL_AND_MODULES) {
            uint64_t first = ALIGN_UP(e->base, PAGE_SIZE),
                     last = ALIGN_DOWN(e->base + e->length, PAGE_SIZE);
            if (last > first)
                total_pages += (last - first) / PAGE_SIZE;
        }
        if (e->type == LIMINE_MEMMAP_USABLE && e->base + e->length > limit)
            limit = e->base + e->length;
    }
    pages = ALIGN_UP(limit, PAGE_SIZE) / PAGE_SIZE;
    if (!pages || pages > UINT64_MAX / 3)
        panic("Invalid physical memory map");
    uint64_t bs = ALIGN_UP((pages + 7) / 8, 16), bytes = ALIGN_UP(bs + pages * 2, PAGE_SIZE),
             storage = 0;
    for (uint64_t i = 0; i < m->entry_count; i++) {
        struct limine_memmap_entry *e = m->entries[i];
        uint64_t b = ALIGN_UP(e->base, PAGE_SIZE);
        if (!b)
            b = PAGE_SIZE;
        if (e->type == LIMINE_MEMMAP_USABLE && b >= PAGE_SIZE && e->length > bytes + PAGE_SIZE) {
            storage = b;
            break;
        }
    }
    if (!storage)
        panic("No RAM for frame bitmap");
    bitmap = phys_ptr(storage);
    refs = (void *)(bitmap + bs);
    memset(bitmap, 255, bs);
    memset(refs, 0, pages * 2);
    for (uint64_t i = 0; i < m->entry_count; i++) {
        struct limine_memmap_entry *e = m->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        uint64_t first = ALIGN_UP(e->base, PAGE_SIZE) / PAGE_SIZE,
                 last = ALIGN_DOWN(e->base + e->length, PAGE_SIZE) / PAGE_SIZE;
        for (uint64_t j = first; j < last; j++)
            if (j && !(j >= storage / PAGE_SIZE && j < (storage + bytes) / PAGE_SIZE)) {
                mark(j, false);
                free_pages++;
            }
    }
    used_pages = total_pages - free_pages;
    hint = 1;
    kprintf("PMM: %lu pages, %lu KiB available\n", pages, free_pages * 4);
}
uint64_t frame_alloc(void)
{
    uint64_t f = irq_save();
    spin_lock(&lock);
    if (!free_pages) {
        spin_unlock(&lock);
        irq_restore(f);
        return 0;
    }
    uint64_t start = hint;
    do {
        if (!bit(hint)) {
            uint64_t i = hint;
            mark(i, true);
            refs[i] = 1;
            free_pages--;
            hint = (i + 1) % pages;
            spin_unlock(&lock);
            irq_restore(f);
            memset(phys_ptr(i * PAGE_SIZE), 0, PAGE_SIZE);
            return i * PAGE_SIZE;
        }
        hint = (hint + 1) % pages;
    } while (hint != start);
    panic("Physical memory accounting mismatch");
}
void frame_get(uint64_t p)
{
    uint64_t f = irq_save();
    spin_lock(&lock);
    uint64_t i = p / PAGE_SIZE;
    if (p % PAGE_SIZE || i >= pages || !refs[i] || refs[i] == UINT16_MAX)
        panic("Invalid frame retain %lx", p);
    refs[i]++;
    spin_unlock(&lock);
    irq_restore(f);
}
void frame_put(uint64_t p)
{
    uint64_t f = irq_save();
    spin_lock(&lock);
    uint64_t i = p / PAGE_SIZE;
    if (p % PAGE_SIZE || i >= pages || !bit(i) || !refs[i])
        panic("Invalid/double frame free %lx", p);
    if (!--refs[i]) {
        mark(i, false);
        free_pages++;
        if (i < hint)
            hint = i;
    }
    spin_unlock(&lock);
    irq_restore(f);
}
unsigned frame_refs(uint64_t p)
{
    uint64_t f = irq_save();
    spin_lock(&lock);
    unsigned n = p / PAGE_SIZE < pages ? refs[p / PAGE_SIZE] : 0;
    spin_unlock(&lock);
    irq_restore(f);
    return n;
}
void memory_info(struct ksysinfo *s)
{
    uint64_t f = irq_save();
    spin_lock(&lock);
    s->total = total_pages * PAGE_SIZE;
    s->free = free_pages * PAGE_SIZE;
    s->reserved = used_pages * PAGE_SIZE;
    spin_unlock(&lock);
    irq_restore(f);
}
