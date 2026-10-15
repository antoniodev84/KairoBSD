#include <kernel.h>
static volatile uint32_t *lapic;
static uint64_t lapic_phys;
static volatile uint32_t *ioapics[8];
static unsigned io_count;
static uint32_t io_gsi[8];
struct override {
    uint8_t irq;
    uint32_t gsi;
    uint16_t flags;
};
static struct override overrides[32];
static unsigned override_count;
static spinlock_t io_lock = SPINLOCK_INIT;
static uint32_t io_read(volatile uint32_t *p, uint32_t r)
{
    p[0] = r;
    return p[4];
}
static void io_write(volatile uint32_t *p, uint32_t r, uint32_t v)
{
    p[0] = r;
    p[4] = v;
}
uint32_t lapic_read(unsigned r)
{
    return lapic[r / 4];
}
void lapic_write(unsigned r, uint32_t v)
{
    lapic[r / 4] = v;
    (void)lapic[0x20 / 4];
}
void apic_init(void)
{
    if (!lapic_phys) {
        uint8_t *m = acpi_find("APIC");
        if (!m)
            panic("MADT missing");
        uint32_t length;
        memcpy(&length, m + 4, 4);
        if (length < 44)
            panic("MADT too short");
        uint32_t base;
        memcpy(&base, m + 36, 4);
        lapic_phys = base;
        for (unsigned off = 44; off + 2 <= length;) {
            unsigned type = m[off], n = m[off + 1];
            if (n < 2 || off + n > length)
                panic("Malformed MADT");
            if (type == 1 && n >= 12 && io_count < 8) {
                uint32_t p;
                memcpy(&p, m + off + 4, 4);
                memcpy(&io_gsi[io_count], m + off + 8, 4);
                ioapics[io_count++] = mmio_map(p, 4096);
            }
            if (type == 2 && n >= 10 && override_count < 32) {
                struct override *o = &overrides[override_count++];
                o->irq = m[off + 3];
                memcpy(&o->gsi, m + off + 4, 4);
                memcpy(&o->flags, m + off + 8, 2);
            }
            if (type == 5 && n >= 12)
                memcpy(&lapic_phys, m + off + 4, 8);
            off += n;
        }
        lapic = mmio_map(lapic_phys, 4096);
        for (unsigned i = 0; i < io_count; i++) {
            unsigned max = (io_read(ioapics[i], 1) >> 16) & 255;
            for (unsigned j = 0; j <= max; j++) {
                io_write(ioapics[i], 0x10 + 2 * j, 1U << 16);
                io_write(ioapics[i], 0x11 + 2 * j, 0);
            }
        }
        kprintf("APIC: LAPIC=%lx, %u I/O APIC controllers\n", lapic_phys, io_count);
    }
    uint64_t msr = rdmsr(0x1b);
    if (msr & (1U << 10))
        panic("Unexpected x2APIC mode");
    wrmsr(0x1b, (msr & ~PTE_ADDR) | lapic_phys | (1U << 11));
    lapic_write(0xf0, 0x100 | 255);
    lapic_write(0x80, 0);
    lapic_write(0x350, 1U << 16);
    lapic_write(0x360, 1U << 16);
    lapic_write(0x370, 1U << 16);
    apic_eoi();
}
void apic_eoi(void)
{
    if (lapic)
        lapic_write(0xb0, 0);
}

unsigned acpi_irq_gsi(unsigned irq, bool *low, bool *level)
{
    *low = *level = false;
    for (unsigned i = 0; i < override_count; i++)
        if (overrides[i].irq == irq) {
            *low = (overrides[i].flags & 3) == 3;
            *level = ((overrides[i].flags >> 2) & 3) == 3;
            return overrides[i].gsi;
        }
    return irq;
}
int ioapic_route(unsigned gsi, unsigned vector, unsigned destination, bool low, bool level)
{
    if (vector < 32 || vector >= 255 || destination > 255)
        return -EINVAL;
    uint64_t flags = irq_save();
    spin_lock(&io_lock);
    for (unsigned i = 0; i < io_count; i++) {
        volatile uint32_t *p = ioapics[i];
        unsigned max = (io_read(p, 1) >> 16) & 255;
        if (gsi >= io_gsi[i] && gsi - io_gsi[i] <= max) {
            unsigned index = gsi - io_gsi[i];
            uint32_t value = vector | (low ? 1U << 13 : 0) | (level ? 1U << 15 : 0);
            io_write(p, 0x10 + 2 * index, value | (1U << 16));
            io_write(p, 0x11 + 2 * index, destination << 24);
            io_write(p, 0x10 + 2 * index, value);
            spin_unlock(&io_lock);
            irq_restore(flags);
            return 0;
        }
    }
    spin_unlock(&io_lock);
    irq_restore(flags);
    return -ENODEV;
}

void apic_stop_others(void)
{
    if (!lapic)
        return;
    unsigned tries = 1000000;
    while ((lapic_read(0x300) & (1U << 12)) && tries--)
        pause_cpu();
    if (tries)
        lapic_write(0x300, 0xc0000 | 254);
}
