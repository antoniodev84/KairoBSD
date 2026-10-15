#include <kernel.h>
struct task tasks[MAX_TASKS];
spinlock_t sched_lock = SPINLOCK_INIT;
static int next_pid = 1;
static struct task *allocate(unsigned cpu)
{
    uint64_t f = irq_save();
    spin_lock(&sched_lock);
    struct task *t = NULL;
    for (unsigned i = 0; i < MAX_TASKS; i++)
        if (!tasks[i].pid) {
            t = &tasks[i];
            memset(t, 0, sizeof(*t));
            t->pid = next_pid++;
            t->constructing = true;
            t->state = BLOCKED;
            t->cpu = cpu;
            break;
        }
    spin_unlock(&sched_lock);
    irq_restore(f);
    if (!t)
        return NULL;
    t->stack = kmalloc(32768);
    if (!t->stack) {
        spin_lock(&sched_lock);
        t->pid = 0;
        spin_unlock(&sched_lock);
        return NULL;
    }
    t->frame = (void *)((uint8_t *)t->stack + 32768 - sizeof(struct frame));
    t->fpstate[0] = 0x7f;
    t->fpstate[1] = 3;
    t->fpstate[24] = 0x80;
    t->fpstate[25] = 0x1f;
    t->umask = 0022;
    t->mmap_next = 0x100000000UL;
    strcpy(t->cwd_path, "/");
    return t;
}
void sched_init(void)
{
    memset(tasks, 0, sizeof(tasks));
    kprintf("Scheduler: preemptive, per-CPU run eligibility\n");
}
struct task *task_kernel(void (*entry)(void), unsigned cpu)
{
    struct task *t = allocate(cpu);
    if (!t)
        return NULL;
    t->kernel = true;
    t->root = kernel_root;
    memset(t->frame, 0, sizeof(*t->frame));
    t->frame->rip = (uint64_t)entry;
    t->frame->cs = 8;
    t->frame->ss = 16;
    t->frame->flags = 0x202;
    t->frame->rsp = ALIGN_DOWN((uint64_t)t->frame - 16, 16) + 8;
    t->constructing = false;
    t->state = READY;
    return t;
}
void idle_loop(void)
{
    for (;;)
        __asm__ volatile("sti; hlt" ::: "memory");
}
void sched_enter(void)
{
    struct cpu *c = this_cpu();
    spin_lock(&sched_lock);
    for (unsigned i = 0; i < c->retired_count; i++)
        c->retired[i]->retired = true;
    c->retired_count = 0;
    spin_unlock(&sched_lock);
    for (unsigned i = 0; i < MAX_TASKS; i++) {
        spin_lock(&sched_lock);
        struct task *t = &tasks[i];
        bool orphan = t->pid && !t->ppid && t->state == TERMINATED && t->retired;
        if (orphan)
            t->state = BLOCKED;
        spin_unlock(&sched_lock);
        if (orphan)
            task_reap(t);
    }
}
static struct frame *schedule_next(struct frame *f, bool tick)
{
    struct cpu *c = this_cpu();
    uint64_t now = monotonic_ns();
    spin_lock(&sched_lock);
    struct task *old = c->current;
    if (old) {
        old->frame = f;
        if (old->state == RUNNING)
            old->state = READY;
    }
    for (unsigned i = 0; i < MAX_TASKS; i++)
        if (tasks[i].pid && tasks[i].alarm_deadline && tasks[i].alarm_deadline <= now) {
            tasks[i].alarm_deadline = 0;
            struct ksiginfo info = {.signo = SIGALRM};
            signal_queue_locked(&tasks[i], SIGALRM, &info);
        }
    for (unsigned i = 0; i < MAX_TASKS; i++)
        if (tasks[i].pid && tasks[i].state == BLOCKED && tasks[i].wake && tasks[i].wake <= now) {
            tasks[i].wake = 0;
            tasks[i].state = READY;
        }
    unsigned start = old ? (unsigned)(old - tasks) + 1 : 0;
    struct task *next = NULL;
    for (unsigned i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[(start + i) % MAX_TASKS];
        if (t->pid && t->cpu == c->id && t->state == READY && t != c->idle) {
            next = t;
            break;
        }
    }
    if (!next)
        next = c->idle;
    if (!next)
        panic("CPU %u has no idle task", c->id);
    if (next != old) {
        if (old)
            __asm__ volatile("fxsave64 %0" : "=m"(old->fpstate)::"memory");
        __asm__ volatile("fxrstor64 %0" ::"m"(next->fpstate) : "memory");
    }
    next->state = RUNNING;
    c->current = next;
    c->kernel_sp = (uint64_t)next->stack + 32768;
    c->tss.rsp[0] = c->kernel_sp;
    if (!old || old->root != next->root)
        __asm__ volatile("mov %0,%%cr3" ::"r"(next->root) : "memory");
    if (old && old->state == TERMINATED && !old->retirement_queued) {
        old->retirement_queued = true;
        if (c->retired_count == MAX_TASKS)
            panic("Retirement queue overflow");
        c->retired[c->retired_count++] = old;
    }
    spin_unlock(&sched_lock);
    (void)tick;
    return next->frame;
}
struct frame *schedule(struct frame *f, bool tick)
{
    for (;;) {
        f = schedule_next(f, tick);
        if (signal_deliver(f))
            return f;
        tick = false;
    }
}
int task_exec(struct task *t, const char *path, const char *const *argv, const char *const *envp)
{
    struct vnode *n;
    int r = vfs_lookup(path, t->cwd, true, &n);
    if (r)
        return r;
    if ((n->mode & S_IFMT) != S_IFREG || vfs_access(n, 5)) {
        vnode_put(n);
        return -EACCES;
    }
    struct task image = {0};
    image.root = space_new();
    if (!image.root) {
        vnode_put(n);
        return -ENOMEM;
    }
    struct frame fresh;
    r = elf_load(&image, n, argv, envp, &fresh);
    vnode_put(n);
    if (r) {
        space_destroy(image.root);
        return r;
    }
    for (unsigned i = 0; i < MAX_FDS; i++)
        if (t->fd_flags[i] & FD_CLOEXEC) {
            file_put(t->fds[i]);
            t->fds[i] = NULL;
            t->fd_flags[i] = 0;
        }
    spin_lock(&sched_lock);
    for (unsigned i = 1; i < NSIG; i++)
        if (t->actions[i].handler != 1)
            memset(&t->actions[i], 0, sizeof(t->actions[i]));
    t->signal_frame = t->signal_depth = 0;
    t->retrying = t->sleeping = t->suspended = false;
    t->execed = true;
    spin_unlock(&sched_lock);
    uint64_t old = t->root;
    t->root = image.root;
    t->mmap_next = 0x100000000UL;
    *t->frame = fresh;
    memset(t->fpstate, 0, sizeof(t->fpstate));
    t->fpstate[0] = 0x7f;
    t->fpstate[1] = 3;
    t->fpstate[24] = 0x80;
    t->fpstate[25] = 0x1f;
    if (this_cpu()->current == t)
        __asm__ volatile("fxrstor64 %0" ::"m"(t->fpstate) : "memory");
    if (this_cpu()->current == t)
        __asm__ volatile("mov %0,%%cr3" ::"r"(t->root) : "memory");
    if (old && old != kernel_root)
        space_destroy(old);
    return 0;
}
int task_spawn(const char *path, struct task *parent, const char *const *argv,
               const char *const *envp)
{
    struct task *t = allocate(0);
    if (!t)
        return -EAGAIN;
    t->ppid = parent ? parent->pid : 0;
    t->pgid = parent ? parent->pgid : t->pid;
    t->sid = parent ? parent->sid : t->pid;
    t->cwd = parent ? parent->cwd : vfs_root;
    vnode_get(t->cwd);
    if (parent) {
        t->uid = parent->uid;
        t->gid = parent->gid;
        t->umask = parent->umask;
        memcpy(t->fd_flags, parent->fd_flags, sizeof(t->fd_flags));
        strcpy(t->cwd_path, parent->cwd_path);
        for (unsigned i = 0; i < MAX_FDS; i++)
            if (parent->fds[i]) {
                t->fds[i] = parent->fds[i];
                atomic_fetch_add(&t->fds[i]->refs, 1);
            }
    } else {
        for (unsigned i = 0; i < 3; i++) {
            int r = vfs_open("/dev/console", t, i ? O_WRONLY : O_RDONLY, 0, &t->fds[i]);
            if (r) {
                task_reap(t);
                return r;
            }
        }
    }
    int r = task_exec(t, path, argv, envp);
    if (r) {
        task_reap(t);
        return r;
    }
    spin_lock(&sched_lock);
    t->constructing = false;
    t->state = READY;
    spin_unlock(&sched_lock);
    return t->pid;
}
int task_fork(struct frame *f)
{
    struct task *p = this_cpu()->current;
    struct task *t = allocate((p->cpu + 1) % cpu_count);
    if (!t)
        return -EAGAIN;
    t->root = space_clone(p->root);
    if (!t->root) {
        task_reap(t);
        return -ENOMEM;
    }
    t->ppid = p->pid;
    t->pgid = p->pgid;
    t->sid = p->sid;
    t->signal_mask = p->signal_mask;
    t->signal_frame = p->signal_frame;
    t->signal_depth = p->signal_depth;
    memcpy(t->actions, p->actions, sizeof(t->actions));
    memcpy(t->fd_flags, p->fd_flags, sizeof(t->fd_flags));
    t->uid = p->uid;
    t->gid = p->gid;
    t->umask = p->umask;
    t->cwd = p->cwd;
    vnode_get(t->cwd);
    strcpy(t->cwd_path, p->cwd_path);
    t->mmap_next = p->mmap_next;
    for (unsigned i = 0; i < MAX_FDS; i++)
        if (p->fds[i]) {
            t->fds[i] = p->fds[i];
            atomic_fetch_add(&t->fds[i]->refs, 1);
        }
    __asm__ volatile("fxsave64 %0" : "=m"(p->fpstate)::"memory");
    memcpy(t->fpstate, p->fpstate, sizeof(t->fpstate));
    *t->frame = *f;
    t->frame->rax = 0;
    spin_lock(&sched_lock);
    t->constructing = false;
    t->state = READY;
    spin_unlock(&sched_lock);
    return t->pid;
}
static void finish_exit(int status)
{
    struct task *t = this_cpu()->current;
    if (t->kernel)
        panic("Kernel task exit");
    for (unsigned i = 0; i < MAX_FDS; i++) {
        file_put(t->fds[i]);
        t->fds[i] = NULL;
    }
    vnode_put(t->cwd);
    t->cwd = NULL;
    spin_lock(&sched_lock);
    t->status = status;
    signal_child_locked(t, SIGCHLD, status);
    for (unsigned i = 0; i < MAX_TASKS; i++)
        if (tasks[i].pid == t->ppid && (tasks[i].actions[SIGCHLD].handler == 1 ||
                                        (tasks[i].actions[SIGCHLD].flags & SA_NOCLDWAIT)))
            t->ppid = 0;
    t->state = TERMINATED;
    for (unsigned i = 0; i < MAX_TASKS; i++)
        if (tasks[i].pid && tasks[i].ppid == t->pid)
            tasks[i].ppid = 0;
    spin_unlock(&sched_lock);
}
void task_exit(int status)
{
    finish_exit((status & 255) << 8);
}
void task_signal_exit(int s)
{
    finish_exit(s & 127);
}
void task_reap(struct task *t)
{
    for (unsigned i = 0; i < MAX_FDS; i++)
        file_put(t->fds[i]);
    vnode_put(t->cwd);
    if (t->root && t->root != kernel_root)
        space_destroy(t->root);
    kfree(t->stack);
    spin_lock(&sched_lock);
    memset(t, 0, sizeof(*t));
    spin_unlock(&sched_lock);
}
