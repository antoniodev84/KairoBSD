#include <kernel.h>
struct cpu cpus[MAX_CPUS];
unsigned cpu_count = 1;
struct idt_entry {
    uint16_t lo, sel;
    uint8_t ist, type;
    uint16_t mid;
    uint32_t hi, zero;
} PACKED;
static struct idt_entry idt[256];
extern void *isr_table[256];
extern void syscall_entry(void);
extern void gdt_load(void *);
void idt_init(void)
{
    for (unsigned i = 0; i < 256; i++) {
        uint64_t p = (uint64_t)isr_table[i];
        idt[i] = (struct idt_entry){
            p, 8, i == 8 ? 1 : (i == 2 ? 2 : (i == 18 ? 3 : 0)), 0x8e, p >> 16, p >> 32, 0};
    }
}
void arch_init(struct cpu *c)
{
    c->self = c;
    c->gdt[0] = 0;
    c->gdt[1] = 0x00af9a000000ffff;
    c->gdt[2] = 0x00cf92000000ffff;
    c->gdt[3] = 0x00cff2000000ffff;
    c->gdt[4] = 0x00affa000000ffff;
    memset(&c->tss, 0, sizeof(c->tss));
    c->tss.iomap = sizeof(c->tss);
    for (unsigned i = 0; i < 3; i++)
        c->tss.ist[i] = (uint64_t)&c->ist[i][8192];
    uint64_t p = (uint64_t)&c->tss;
    c->gdt[5] =
        (sizeof(c->tss) - 1) | ((p & 0xffffff) << 16) | (0x89UL << 40) | ((p & 0xff000000) << 32);
    c->gdt[6] = p >> 32;
    struct {
        uint16_t size;
        uint64_t ptr;
    } PACKED gd = {sizeof(c->gdt) - 1, (uint64_t)c->gdt}, id = {sizeof(idt) - 1, (uint64_t)idt};
    gdt_load(&gd);
    __asm__ volatile("lidt %0; mov $40,%%ax; ltr %%ax" ::"m"(id) : "rax", "memory");
    wrmsr(0xc0000101, (uint64_t)c);
    wrmsr(0xc0000102, 0);
    wrmsr(0xc0000080, rdmsr(0xc0000080) | 1 | (1U << 11));
    wrmsr(0xc0000081, (0x13UL << 48) | (8UL << 32));
    wrmsr(0xc0000082, (uint64_t)syscall_entry);
    wrmsr(0xc0000084, 0x44700);
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0,%0; mov %%cr4,%1" : "=r"(cr0), "=r"(cr4));
    cr0 = (cr0 | 2 | (1U << 5) | (1U << 16)) & ~12UL;
    cr4 = (cr4 | (3UL << 9)) & ~((1UL << 16) | (1UL << 18));
    __asm__ volatile("mov %0,%%cr0; mov %1,%%cr4" ::"r"(cr0), "r"(cr4) : "memory");
    __asm__ volatile("fninit");
}
struct frame *interrupt_dispatch(struct frame *f)
{
    uint64_t vector = f->vector & ~(1UL << 63);
    if (vector == 254) {
        __asm__ volatile("cli");
        for (;;)
            __asm__ volatile("hlt");
    }
    if (vector == 2 || vector == 8 || vector == 18)
        panic("Critical exception %lu RIP=%lx error=%lx", vector, f->rip, f->error);
    sched_enter();
    struct cpu *c = this_cpu();
    if (vector == 32) {
        c->ticks++;
        apic_eoi();
        return schedule(f, true);
    }
    if (vector == 36) {
        serial_irq();
        apic_eoi();
        if (!signal_deliver(f))
            return schedule(f, false);
        return f;
    }
    if (vector == 255)
        return f;
    if (vector >= 32) {
        apic_eoi();
        return f;
    }
    uint64_t cr2 = 0;
    if (vector == 14)
        __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    if (vector == 14 && (f->error & 7) == 7 && c->current && cow_fault(c->current->root, cr2))
        return f;
    if ((f->cs & 3) == 3 && c->current) {
        kprintf("PID %d exception %lu RIP=%lx address=%lx error=%lx\n", c->current->pid, vector,
                f->rip, cr2, f->error);
        int signo = vector == 6                                   ? SIGILL
                    : vector == 0 || vector == 16 || vector == 19 ? SIGFPE
                    : vector == 3 || vector == 1                  ? SIGTRAP
                    : vector == 17                                ? SIGBUS
                                                                  : SIGSEGV;
        struct ksiginfo info = {.signo = signo, .code = 3, .address = cr2};
        spin_lock(&sched_lock);
        c->current->signal_mask &= ~(1UL << (signo - 1));
        if (c->current->actions[signo].handler == 1)
            c->current->actions[signo].handler = 0;
        signal_queue_locked(c->current, signo, &info);
        spin_unlock(&sched_lock);
        if (!signal_deliver(f))
            return schedule(f, false);
        return f;
    }
    panic("CPU %u exception %lu RIP=%lx CR2=%lx error=%lx", c->id, vector, f->rip, cr2, f->error);
}
