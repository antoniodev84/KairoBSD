#include <kernel.h>
struct sdt {
    char sig[4];
    uint32_t length;
    uint8_t revision, checksum;
    char oem[6], table[8];
    uint32_t oemrev, creator, creatorrev;
} PACKED;
struct rsdp {
    char sig[8];
    uint8_t checksum;
    char oem[6];
    uint8_t revision;
    uint32_t rsdt, length;
    uint64_t xsdt;
    uint8_t extchecksum, reserved[3];
} PACKED;
static struct sdt *root;
static bool xsdt;
static bool checksum(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint8_t s = 0;
    while (n--)
        s += *b++;
    return s == 0;
}
static struct sdt *table(uint64_t p)
{
    struct sdt *t = mmio_map(p, 4096);
    if (t->length < sizeof(*t) || t->length > 1024 * 1024)
        return NULL;
    mmio_map(p, t->length);
    return checksum(t, t->length) ? t : NULL;
}
void acpi_init(uint64_t address)
{
    if (!address)
        panic("UEFI ACPI RSDP missing");
    uint64_t p = address >= hhdm ? address - hhdm : address;
    struct rsdp *r = mmio_map(p, sizeof(*r));
    if (memcmp(r->sig, "RSD PTR ", 8) || !checksum(r, 20))
        panic("Invalid ACPI RSDP");
    xsdt = r->revision >= 2 && r->xsdt;
    if (xsdt && (r->length < 36 || r->length > 4096 || !checksum(r, r->length)))
        panic("Invalid extended RSDP");
    root = table(xsdt ? r->xsdt : r->rsdt);
    if (!root || memcmp(root->sig, xsdt ? "XSDT" : "RSDT", 4))
        panic("Invalid ACPI root table");
    kprintf("ACPI: %s verified\n", xsdt ? "XSDT" : "RSDT");
}
void *acpi_find(const char *sig)
{
    unsigned step = xsdt ? 8 : 4;
    size_t count = (root->length - sizeof(*root)) / step;
    uint8_t *p = (void *)(root + 1);
    for (size_t i = 0; i < count; i++) {
        uint64_t a = 0;
        memcpy(&a, p + i * step, step);
        struct sdt *t = table(a);
        if (t && !memcmp(t->sig, sig, 4))
            return t;
    }
    return NULL;
}
