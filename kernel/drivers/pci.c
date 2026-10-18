#include <kernel.h>
struct ecam_range {
    uint64_t base;
    uint16_t segment;
    uint8_t first, last;
    uint32_t reserved;
} PACKED;
static struct ecam_range ranges[32];
static unsigned range_count;
static volatile uint8_t *config(struct pci_device *d)
{
    for (unsigned i = 0; i < range_count; i++) {
        struct ecam_range *r = &ranges[i];
        if (d->segment == r->segment && d->bus >= r->first && d->bus <= r->last)
            return mmio_map(r->base + ((uint64_t)(d->bus - r->first) << 20) +
                                ((uint64_t)d->slot << 15) + ((uint64_t)d->func << 12),
                            4096);
    }
    return NULL;
}
uint32_t pci_read(struct pci_device *d, unsigned off, unsigned size)
{
    if ((size != 1 && size != 2 && size != 4) || off + size > 4096 || off % size)
        return UINT32_MAX;
    volatile uint8_t *p = config(d);
    if (p) {
        if (size == 1)
            return p[off];
        if (size == 2)
            return *(volatile uint16_t *)(p + off);
        return *(volatile uint32_t *)(p + off);
    }
    if (d->segment || off >= 256)
        return UINT32_MAX;
    outl(0xcf8, 0x80000000U | ((uint32_t)d->bus << 16) | ((uint32_t)d->slot << 11) |
                    ((uint32_t)d->func << 8) | (off & 252));
    uint32_t v = inl(0xcfc);
    return (v >> ((off & 3) * 8)) & (size == 4 ? UINT32_MAX : ((1U << (size * 8)) - 1));
}
void pci_write(struct pci_device *d, unsigned off, unsigned size, uint32_t v)
{
    if ((size != 1 && size != 2 && size != 4) || off + size > 4096 || off % size)
        return;
    volatile uint8_t *p = config(d);
    if (p) {
        if (size == 1)
            p[off] = v;
        else if (size == 2)
            *(volatile uint16_t *)(p + off) = v;
        else
            *(volatile uint32_t *)(p + off) = v;
        return;
    }
    if (d->segment || off >= 256)
        return;
    outl(0xcf8, 0x80000000U | ((uint32_t)d->bus << 16) | ((uint32_t)d->slot << 11) |
                    ((uint32_t)d->func << 8) | (off & 252));
    if (size == 4)
        outl(0xcfc, v);
    else if (size == 1)
        outb(0xcfc + (off & 3), v);
    else
        __asm__ volatile("outw %0,%1" ::"a"((uint16_t)v), "Nd"((uint16_t)(0xcfc + (off & 3))));
}
static void bars(struct pci_device *d)
{
    unsigned count = (d->header & 127) == 0 ? 6 : (d->header & 127) == 1 ? 2 : 0;
    uint16_t cmd = pci_read(d, 4, 2);
    pci_write(d, 4, 2, cmd & ~3U);
    for (unsigned i = 0; i < count; i++) {
        unsigned off = 16 + i * 4;
        uint32_t lo = pci_read(d, off, 4);
        if (lo & 1) {
            pci_write(d, off, 4, UINT32_MAX);
            uint32_t mask = pci_read(d, off, 4) & ~3U;
            pci_write(d, off, 4, lo);
            d->bar[i] = lo & ~3U;
            d->bar_size[i] = mask ? (~mask + 1) : 0;
            continue;
        }
        bool wide = (lo & 6) == 4;
        if (wide && i + 1 >= count)
            continue;
        uint32_t hi = wide ? pci_read(d, off + 4, 4) : 0;
        pci_write(d, off, 4, UINT32_MAX);
        if (wide)
            pci_write(d, off + 4, 4, UINT32_MAX);
        uint32_t mask = pci_read(d, off, 4) & ~15U,
                 hm = wide ? pci_read(d, off + 4, 4) : UINT32_MAX;
        pci_write(d, off, 4, lo);
        if (wide)
            pci_write(d, off + 4, 4, hi);
        uint64_t address = ((uint64_t)hi << 32) | (lo & ~15U), bits = ((uint64_t)hm << 32) | mask;
        d->bar[i] = address;
        uint64_t size = ~bits + 1;
        d->bar_size[i] = size <= UINT32_MAX ? (uint32_t)size : 0;
        if (wide)
            i++;
    }
    pci_write(d, 4, 2, cmd);
}
static void scan(uint16_t segment, unsigned first, unsigned last)
{
    for (unsigned b = first; b <= last; b++)
        for (unsigned s = 0; s < 32; s++) {
            struct pci_device d = {.segment = segment, .bus = b, .slot = s};
            if (pci_read(&d, 0, 2) == 0xffff)
                continue;
            unsigned funcs = pci_read(&d, 14, 1) & 128 ? 8 : 1;
            for (unsigned f = 0; f < funcs; f++) {
                d.func = f;
                d.vendor = pci_read(&d, 0, 2);
                if (d.vendor == 0xffff)
                    continue;
                d.device = pci_read(&d, 2, 2);
                d.class = pci_read(&d, 11, 1);
                d.subclass = pci_read(&d, 10, 1);
                d.progif = pci_read(&d, 9, 1);
                d.header = pci_read(&d, 14, 1);
                memset(d.bar, 0, sizeof(d.bar));
                memset(d.bar_size, 0, sizeof(d.bar_size));
                bars(&d);
                kprintf("PCI %u:%02x:%02x.%u %04x:%04x class %02x:%02x\n", segment, b, s, f,
                        d.vendor, d.device, d.class, d.subclass);
                int r = 0;
                if (d.vendor == 0x10ec && d.device == 0x8139)
                    r = rtl8139_probe(&d);
                else if (d.vendor == 0x8086 &&
                         (d.device == 0x10d3 || d.device == 0x100e || d.device == 0x100f))
                    r = e1000_probe(&d);
                else if (d.vendor == 0x1af4 && (d.device == 0x1042 || d.device == 0x1001))
                    r = virtio_probe(&d);
                else if (d.class == 1 && d.subclass == 6 && d.progif == 1)
                    r = ahci_probe(&d);
                if (r)
                    kprintf("PCI probe error %d\n", r);
            }
        }
}
void pci_init(void)
{
    uint8_t *m = acpi_find("MCFG");
    if (m) {
        uint32_t len;
        memcpy(&len, m + 4, 4);
        if (len < 44 || (len - 44) % 16 || (len - 44) / 16 > 32)
            panic("Malformed MCFG");
        range_count = (len - 44) / 16;
        memcpy(ranges, m + 44, range_count * 16);
        for (unsigned i = 0; i < range_count; i++) {
            if (ranges[i].first > ranges[i].last || ranges[i].base & 0xfffff)
                panic("Invalid ECAM range");
            scan(ranges[i].segment, ranges[i].first, ranges[i].last);
        }
    } else
        scan(0, 0, 255);
}
