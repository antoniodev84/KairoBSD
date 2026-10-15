#include <kernel.h>
static void ap_entry(struct limine_smp_info *info)
{
    struct cpu *c = (void *)info->extra_argument;
    __asm__ volatile("cli; mov %0,%%cr3" ::"r"(kernel_root) : "memory");
    arch_init(c);
    apic_init();
    timer_cpu_init();
    atomic_store_explicit(&c->online, true, memory_order_release);
    kprintf("SMP: CPU %u APIC %u online\n", c->id, c->apic_id);
    struct frame boot = {0};
    arch_enter(schedule(&boot, false));
}
void smp_start(struct limine_smp_response *s)
{
    if (!s || !s->cpu_count || s->cpu_count > MAX_CPUS)
        panic("Unsupported boot CPU count");
    cpu_count = s->cpu_count;
    cpus[0].apic_id = s->bsp_lapic_id;
    atomic_store(&cpus[0].online, true);
    for (unsigned i = 0; i < cpu_count; i++) {
        cpus[i].id = i;
        cpus[i].idle = task_kernel(idle_loop, i);
        if (!cpus[i].idle)
            panic("Idle allocation");
    }
    unsigned n = 1;
    for (uint64_t i = 0; i < s->cpu_count; i++)
        if (s->cpus[i]->lapic_id != s->bsp_lapic_id) {
            struct cpu *c = &cpus[n++];
            c->apic_id = s->cpus[i]->lapic_id;
            s->cpus[i]->extra_argument = (uint64_t)c;
            __atomic_store_n(&s->cpus[i]->goto_address, ap_entry, __ATOMIC_RELEASE);
            uint64_t start = monotonic_ns();
            while (!atomic_load_explicit(&c->online, memory_order_acquire)) {
                if (monotonic_ns() - start > 2000000000UL)
                    panic("AP startup timed out");
                pause_cpu();
            }
        }
}
