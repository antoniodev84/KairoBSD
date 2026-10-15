#ifndef KARIO_KERNEL_H
#define KARIO_KERNEL_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <kario.h>
#include <limine.h>
#define PACKED __attribute__((packed))
#define NORETURN __attribute__((noreturn))
#define MAX_CPUS 32
#define MAX_TASKS 128
#define MAX_FDS 64
#define PTE_P 1UL
#define PTE_W 2UL
#define PTE_U 4UL
#define PTE_COW 512UL
#define PTE_NX (1UL << 63)
#define PTE_ADDR 0x000ffffffffff000UL
#define USER_TOP 0x0000800000000000UL
#define USER_STACK 0x00007ffffff00000UL
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
typedef struct {
    atomic_flag flag;
} spinlock_t;
#define SPINLOCK_INIT {ATOMIC_FLAG_INIT}
static inline void pause_cpu(void)
{
    __asm__ volatile("pause");
}
static inline uint64_t irq_save(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f)::"memory");
    return f;
}
static inline void irq_restore(uint64_t f)
{
    if (f & 512)
        __asm__ volatile("sti" ::: "memory");
}
static inline void spin_lock(spinlock_t *l)
{
    while (atomic_flag_test_and_set_explicit(&l->flag, memory_order_acquire))
        pause_cpu();
}
static inline void spin_unlock(spinlock_t *l)
{
    atomic_flag_clear_explicit(&l->flag, memory_order_release);
}
static inline void outb(uint16_t p, uint8_t v)
{
    __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p));
}
static inline uint8_t inb(uint16_t p)
{
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline uint32_t inl(uint16_t p)
{
    uint32_t v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline void outl(uint16_t p, uint32_t v)
{
    __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(p));
}
static inline uint64_t rdmsr(uint32_t n)
{
    uint32_t a, d;
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(n));
    return ((uint64_t)d << 32) | a;
}
static inline void wrmsr(uint32_t n, uint64_t v)
{
    __asm__ volatile("wrmsr" ::"c"(n), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline uint64_t rdtsc(void)
{
    uint32_t a, d;
    __asm__ volatile("lfence; rdtsc" : "=a"(a), "=d"(d)::"memory");
    return ((uint64_t)d << 32) | a;
}
static inline void cpuid(uint32_t n, uint32_t s, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(n), "c"(s));
}
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strcpy(char *, const char *);
int snprintf(char *, size_t, const char *, ...);
void serial_init(void);
int serial_get(void);
void serial_irq_init(void);
void serial_irq(void);
void console_init(struct limine_framebuffer *);
void console_write(const char *, size_t);
void console_emergency(const char *, size_t);
void kprintf(const char *, ...);
NORETURN void panic(const char *, ...);
extern uint64_t hhdm, kernel_root;
static inline void *phys_ptr(uint64_t p)
{
    return (void *)(p + hhdm);
}
static inline uint64_t ptr_phys(const void *p)
{
    return (uint64_t)p - hhdm;
}
void pmm_init(struct limine_memmap_response *);
uint64_t frame_alloc(void);
void frame_get(uint64_t);
void frame_put(uint64_t);
unsigned frame_refs(uint64_t);
void memory_info(struct ksysinfo *);
void vmm_init(void);
uint64_t space_new(void);
int page_map(uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t *page_entry(uint64_t, uint64_t, bool);
uint64_t page_translate(uint64_t, uint64_t);
void page_unmap(uint64_t, uint64_t, bool);
void space_destroy(uint64_t);
uint64_t space_clone(uint64_t);
bool cow_fault(uint64_t, uint64_t);
bool user_range(uint64_t, uint64_t, size_t, bool);
int copy_in(void *, uint64_t, size_t);
int copy_out(uint64_t, const void *, size_t);
int copy_string(char *, uint64_t, size_t);
void *kmalloc(size_t);
void *kzalloc(size_t);
void *krealloc(void *, size_t);
void *kmemalign(size_t, size_t);
void kfree(void *);
void *mmio_map(uint64_t, size_t);
struct frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rsi, rdi, rbp, rdx, rcx, rbx, rax, vector, error,
        rip, cs, flags, rsp, ss;
};
struct tss {
    uint32_t reserved0;
    uint64_t rsp[3], reserved1, ist[7], reserved2;
    uint16_t reserved3, iomap;
} PACKED;
struct task;
struct cpu {
    uint64_t kernel_sp, user_sp;
    struct cpu *self;
    uint32_t id, apic_id;
    struct task *current, *idle;
    struct task *retired[MAX_TASKS];
    unsigned retired_count;
    struct tss tss;
    uint64_t gdt[7];
    uint8_t ist[3][8192];
    atomic_bool online;
    uint64_t ticks;
};
extern struct cpu cpus[MAX_CPUS];
extern unsigned cpu_count;
static inline struct cpu *this_cpu(void)
{
    struct cpu *p;
    __asm__ volatile("movq %%gs:16,%0" : "=r"(p));
    return p;
}
void arch_init(struct cpu *);
void idt_init(void);
struct frame *interrupt_dispatch(struct frame *);
struct frame *syscall_dispatch(struct frame *);
NORETURN void arch_enter(struct frame *);
void acpi_init(uint64_t);
void *acpi_find(const char *);
void apic_init(void);
void apic_eoi(void);
void apic_stop_others(void);
int ioapic_route(unsigned, unsigned, unsigned, bool, bool);
unsigned acpi_irq_gsi(unsigned, bool *, bool *);
void timer_init(void);
void timer_cpu_init(void);
uint64_t monotonic_ns(void);
extern uint64_t boot_epoch;
uint64_t realtime_seconds(void);
void delay_ns(uint64_t);
void smp_start(struct limine_smp_response *);
struct vnode;
struct file {
    struct vnode *node;
    uint64_t offset;
    atomic_uint flags;
    atomic_uint refs;
    spinlock_t lock;
};
enum task_state { READY, RUNNING, BLOCKED, STOPPED, TERMINATED };
struct task {
    int pid, ppid, status;
    atomic_int pgid, sid;
    bool stop_event, continue_event, execed, retirement_queued;
    uint64_t pending, signal_mask, alarm_deadline, signal_frame, signal_depth;
    struct ksigaction actions[NSIG];
    struct ksiginfo signal_info[NSIG];
    bool retrying, sleeping, suspended;
    uint64_t sleep_deadline, sleep_remaining, suspend_mask;
    unsigned cpu;
    enum task_state state;
    bool kernel, retired, constructing;
    uint64_t root, wake, mmap_next;
    uint32_t uid, gid, umask;
    struct frame *frame;
    void *stack;
    _Alignas(16) uint8_t fpstate[512];
    struct file *fds[MAX_FDS];
    uint8_t fd_flags[MAX_FDS];
    struct vnode *cwd;
    char cwd_path[PATH_MAX];
    int waiting;
};
extern struct task tasks[MAX_TASKS];
extern spinlock_t sched_lock;
void sched_init(void);
struct task *task_kernel(void (*)(void), unsigned);
int task_spawn(const char *, struct task *, const char *const *, const char *const *);
int task_exec(struct task *, const char *, const char *const *, const char *const *);
int task_fork(struct frame *);
void task_exit(int);
void task_signal_exit(int);
void task_reap(struct task *);
void sched_enter(void);
int pipe_create(struct task *, unsigned, int *);
int signal_kill(int, int);
void signal_queue_locked(struct task *, int, const struct ksiginfo *);
void signal_queue_self(int);
void signal_child_locked(struct task *, int, int);
bool signal_deliver(struct frame *);
int signal_action(int, uint64_t, uint64_t);
int signal_mask(int, uint64_t, uint64_t);
int signal_pending(uint64_t);
int signal_suspend(uint64_t);
int signal_restore(uint64_t, struct frame *);
int process_group(int, int, bool);
int terminal_group(int, bool);
void terminal_signal(int);
extern atomic_int terminal_pgid;
struct frame *schedule(struct frame *, bool);
int elf_load(struct task *, struct vnode *, const char *const *, const char *const *,
             struct frame *);
void idle_loop(void);
struct vnode_ops {
    int (*read)(struct vnode *, uint64_t, void *, size_t);
    int (*write)(struct vnode *, uint64_t, const void *, size_t);
    int (*lookup)(struct vnode *, const char *, struct vnode **);
    int (*create)(struct vnode *, const char *, uint32_t, const char *, struct vnode **);
    int (*remove)(struct vnode *, const char *, bool);
    int (*link)(struct vnode *, const char *, struct vnode *);
    int (*readdir)(struct vnode *, uint64_t, struct kdirent *);
    int (*truncate)(struct vnode *, uint64_t);
    int (*chmod)(struct vnode *, uint32_t);
    int (*chown)(struct vnode *, uint32_t, uint32_t);
    void (*destroy)(struct vnode *);
};
struct vnode {
    uint64_t ino;
    atomic_uint_fast64_t size, blocks, atime, mtime, ctime;
    atomic_uint mode, nlink, uid, gid;
    atomic_uint refs;
    struct vnode_ops *ops;
    void *data, *fs;
    struct vnode *parent, *mounted;
    spinlock_t lock;
};
extern struct vnode *vfs_root;
int vfs_access(struct vnode *, unsigned);
void vnode_get(struct vnode *);
void vnode_put(struct vnode *);
int vfs_lookup(const char *, struct vnode *, bool, struct vnode **);
int vfs_parent(const char *, struct vnode *, struct vnode **, char *);
int vfs_create(const char *, struct vnode *, uint32_t, const char *, struct vnode **);
int vfs_remove(const char *, struct vnode *, bool);
int vfs_mount(const char *, struct vnode *, struct vnode *);
int vfs_open(const char *, struct task *, unsigned, unsigned, struct file **);
void file_put(struct file *);
struct vnode *tmpfs_new(uint32_t);
void vfs_init(const void *, size_t);
struct vnode *devfs_new(void);
int devfs_register_block(unsigned);
struct pci_device {
    uint16_t segment;
    uint8_t bus, slot, func;
    uint16_t vendor, device;
    uint8_t class, subclass, progif, header;
    uint64_t bar[6];
    uint32_t bar_size[6];
};
uint32_t pci_read(struct pci_device *, unsigned, unsigned);
void pci_write(struct pci_device *, unsigned, unsigned, uint32_t);
void pci_init(void);
int virtio_probe(struct pci_device *);
int ahci_probe(struct pci_device *);
struct blockdev {
    char name[32];
    uint64_t sectors, start;
    uint32_t sector_size;
    bool readonly;
    void *data;
    struct blockdev *parent;
    int (*transfer)(struct blockdev *, uint64_t, uint32_t, void *, bool);
    int (*flush)(struct blockdev *);
};
extern struct blockdev blocks[32];
extern atomic_uint block_count;
int block_register(struct blockdev *);
int block_io(struct blockdev *, uint64_t, uint32_t, void *, bool);
int partitions_scan(struct blockdev *);
int ext2_mount(struct blockdev *, struct vnode **);
#endif
