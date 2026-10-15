#include <kernel.h>
extern uint32_t lapic_read(unsigned);
extern void lapic_write(unsigned, uint32_t);
struct clocksource {
    const char *name;
    uint64_t (*read)(void);
};
static volatile uint64_t *hpet;
static volatile uint32_t *pm_memory;
static uint16_t pm_port;
static uint32_t pm_mask, pm_last;
static uint64_t pm_ticks;
static spinlock_t pm_lock = SPINLOCK_INIT;
static uint64_t period, tsc_hz, tsc_base, hpet_base;
static uint32_t event_ticks;
static struct clocksource reference, active;
static uint64_t hpet_ns(void)
{
    uint64_t c = hpet[0xf0 / 8] - hpet_base;
    return (c / 1000000) * period + (c % 1000000) * period / 1000000;
}
static uint64_t tsc_ns(void)
{
    uint64_t c = rdtsc() - tsc_base;
    return (c / tsc_hz) * 1000000000UL + (c % tsc_hz) * 1000000000UL / tsc_hz;
}
static uint64_t pm_ns(void)
{
    uint64_t flags = irq_save();
    spin_lock(&pm_lock);
    uint32_t current = (pm_memory ? *pm_memory : inl(pm_port)) & pm_mask;
    pm_ticks += (current - pm_last) & pm_mask;
    pm_last = current;
    uint64_t ticks = pm_ticks;
    spin_unlock(&pm_lock);
    irq_restore(flags);
    return (ticks / 3579545) * 1000000000UL + (ticks % 3579545) * 1000000000UL / 3579545;
}
uint64_t monotonic_ns(void)
{
    return active.read();
}
void delay_ns(uint64_t n)
{
    uint64_t start = monotonic_ns();
    while (monotonic_ns() - start < n)
        pause_cpu();
}
static bool invariant_tsc(void)
{
    uint32_t a, b, c, d;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a < 0x80000007)
        return false;
    cpuid(0x80000007, 0, &a, &b, &c, &d);
    return d & (1U << 8);
}
static bool tsc_frequency(void)
{
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    if (a < 0x15 || !invariant_tsc())
        return false;
    cpuid(0x15, 0, &a, &b, &c, &d);
    if (!a || !b || !c)
        return false;
    tsc_hz = (uint64_t)c * b / a;
    return tsc_hz >= 1000000 && tsc_hz <= 10000000000UL;
}
static void reference_init(void)
{
    uint8_t *table = acpi_find("HPET");
    if (table) {
        uint32_t length;
        memcpy(&length, table + 4, 4);
        uint64_t address;
        if (length < 56 || table[40] != 0)
            panic("Invalid memory-mapped HPET table");
        memcpy(&address, table + 44, 8);
        hpet = mmio_map(address, 1024);
        uint64_t caps = hpet[0];
        period = caps >> 32;
        if (!period || period > 100000000 || !(caps & (1U << 13)))
            panic("Invalid HPET capabilities");
        hpet[0x10 / 8] = 0;
        hpet[0xf0 / 8] = 0;
        hpet[0x10 / 8] = 1;
        hpet_base = hpet[0xf0 / 8];
        reference = (struct clocksource){"HPET", hpet_ns};
        return;
    }
    table = acpi_find("FACP");
    if (table) {
        uint32_t length, flags, address;
        memcpy(&length, table + 4, 4);
        if (length >= 116) {
            memcpy(&flags, table + 112, 4);
            memcpy(&address, table + 76, 4);
            pm_mask = flags & (1U << 8) ? UINT32_MAX : 0xffffff;
            if (length >= 220 && table[209] >= 24 && table[209] <= 32 &&
                (table[211] == 0 || table[211] == 3)) {
                uint64_t extended;
                memcpy(&extended, table + 212, 8);
                if (extended && table[208] == 0)
                    pm_memory = mmio_map(extended, 4);
                else if (extended && table[208] == 1 && extended <= UINT16_MAX)
                    pm_port = extended;
            }
            if (!pm_memory && !pm_port && address && address <= UINT16_MAX && table[91] == 4)
                pm_port = address;
            if (pm_memory || pm_port) {
                pm_last = (pm_memory ? *pm_memory : inl(pm_port)) & pm_mask;
                reference = (struct clocksource){"ACPI PM timer", pm_ns};
                return;
            }
        }
    }
    if (tsc_frequency()) {
        tsc_base = rdtsc();
        reference = (struct clocksource){"invariant TSC", tsc_ns};
        return;
    }
    panic("No reliable modern clocksource available");
}
void timer_init(void)
{
    reference_init();
    active = reference;
    lapic_write(0x3e0, 3);
    lapic_write(0x320, 1U << 16);
    lapic_write(0x380, 0xffffffff);
    uint64_t start = reference.read(), ts = rdtsc();
    while (reference.read() - start < 20000000)
        pause_cpu();
    uint64_t elapsed = reference.read() - start, dt = rdtsc() - ts;
    uint64_t ticks = 0xffffffffUL - lapic_read(0x390);
    event_ticks = (uint32_t)(ticks * 10000000 / elapsed);
    if (!event_ticks)
        panic("LAPIC calibration failed");
    if (reference.read != tsc_ns) {
        tsc_hz = dt * 1000000000UL / elapsed;
        if (invariant_tsc() && tsc_hz > 1000000 && tsc_hz < 10000000000UL) {
            tsc_base = rdtsc();
            active = (struct clocksource){"invariant TSC", tsc_ns};
        }
    }
    kprintf("Timer: reference %s, LAPIC %u ticks/10ms, clocksource %s\n", reference.name,
            event_ticks, active.name);
}
void timer_cpu_init(void)
{
    lapic_write(0x3e0, 3);
    lapic_write(0x320, (1U << 17) | 32);
    lapic_write(0x380, event_ticks);
}
