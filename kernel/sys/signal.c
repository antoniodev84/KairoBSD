#include <kernel.h>
#define SIGNAL_MAGIC 0x4b4152494f534947UL
atomic_int terminal_pgid;
static int terminal_sid;
static uint64_t bit(int s)
{
    return 1UL << (s - 1);
}
static bool ignored(struct task *t, int s)
{
    return t->actions[s].handler == 1 ||
           (!t->actions[s].handler &&
            (s == SIGCHLD || s == SIGURG || s == SIGWINCH || s == SIGCONT));
}
void signal_queue_locked(struct task *t, int s, const struct ksiginfo *info)
{
    if (!t->pid || t->kernel || t->constructing || t->state == TERMINATED || s <= 0 || s >= NSIG)
        return;
    if (s == SIGCONT) {
        t->pending &= ~(bit(SIGSTOP) | bit(SIGTSTP) | bit(SIGTTIN) | bit(SIGTTOU));
        if (t->state == STOPPED) {
            t->state = READY;
            t->stop_event = false;
            if (t->sleeping && ignored(t, SIGCONT) && t->sleep_deadline > monotonic_ns()) {
                t->state = BLOCKED;
                t->wake = t->sleep_deadline;
            }
            if (t->suspended && ignored(t, SIGCONT)) {
                t->state = BLOCKED;
                t->wake = 0;
            }
            t->continue_event = true;
            signal_child_locked(t, SIGCHLD, 0xffff);
        }
    }
    if (s == SIGSTOP || s == SIGTSTP || s == SIGTTIN || s == SIGTTOU)
        t->pending &= ~bit(SIGCONT);
    if (ignored(t, s))
        return;
    if (!(t->pending & bit(s)))
        t->signal_info[s] = *info;
    t->pending |= bit(s);
    if ((!(t->signal_mask & bit(s)) || s == SIGKILL || s == SIGSTOP) &&
        (t->state == BLOCKED || (s == SIGKILL && t->state == STOPPED))) {
        t->state = READY;
        t->wake = 0;
    }
}
void signal_child_locked(struct task *child, int s, int status)
{
    for (unsigned i = 0; i < MAX_TASKS; i++)
        if (tasks[i].pid == child->ppid) {
            struct task *parent = &tasks[i];
            if ((status == 0xffff || (status & 255) == 127) &&
                (parent->actions[SIGCHLD].flags & SA_NOCLDSTOP))
                return;
            struct ksiginfo info = {.signo = s,
                                    .code = status == 0xffff        ? 6
                                            : (status & 255) == 127 ? 5
                                            : (status & 127)        ? 2
                                                                    : 1,
                                    .pid = child->pid,
                                    .uid = child->uid,
                                    .status = status == 0xffff        ? SIGCONT
                                              : (status & 255) == 127 ? status >> 8
                                              : (status & 127)        ? status & 127
                                                                      : status >> 8};
            signal_queue_locked(parent, s, &info);
            return;
        }
}
void signal_queue_self(int s)
{
    struct task *t = this_cpu()->current;
    struct ksiginfo info = {.signo = s, .pid = t->pid, .uid = t->uid};
    spin_lock(&sched_lock);
    signal_queue_locked(t, s, &info);
    spin_unlock(&sched_lock);
}
int signal_kill(int pid, int s)
{
    if (s < 0 || s >= NSIG || pid == INT32_MIN)
        return -EINVAL;
    struct task *sender = this_cpu()->current;
    bool found = false, allowed = false;
    spin_lock(&sched_lock);
    for (unsigned i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (!t->pid || t->kernel || t->constructing || t->state == TERMINATED)
            continue;
        bool match = pid > 0     ? t->pid == pid
                     : pid == 0  ? t->pgid == sender->pgid
                     : pid == -1 ? t->pid != 1 && t != sender
                                 : t->pgid == -pid;
        if (!match)
            continue;
        found = true;
        if (sender->uid && sender->uid != t->uid && !(s == SIGCONT && sender->sid == t->sid))
            continue;
        allowed = true;
        if (s) {
            struct ksiginfo info = {.signo = s, .pid = sender->pid, .uid = sender->uid};
            signal_queue_locked(t, s, &info);
        }
    }
    spin_unlock(&sched_lock);
    return allowed ? 0 : found ? -EPERM : -ESRCH;
}
int signal_action(int s, uint64_t input, uint64_t output)
{
    if (s <= 0 || s >= NSIG || s == SIGKILL || s == SIGSTOP)
        return -EINVAL;
    struct ksigaction action, old;
    struct task *t = this_cpu()->current;
    if (input) {
        int r = copy_in(&action, input, sizeof(action));
        if (r)
            return r;
        if (action.reserved || action.flags & ~(SA_RESTART | SA_NODEFER | SA_RESETHAND |
                                                SA_SIGINFO | SA_NOCLDSTOP | SA_NOCLDWAIT))
            return -EINVAL;
        if (action.handler > 1 && (action.handler < PAGE_SIZE || action.handler >= USER_TOP ||
                                   action.restorer < PAGE_SIZE || action.restorer >= USER_TOP))
            return -EFAULT;
        action.mask &= SIGNAL_BITS & ~SIGNAL_UNBLOCKABLE;
    }
    if (output && !user_range(t->root, output, sizeof(old), true))
        return -EFAULT;
    spin_lock(&sched_lock);
    old = t->actions[s];
    if (input) {
        t->actions[s] = action;
        if (ignored(t, s))
            t->pending &= ~bit(s);
    }
    spin_unlock(&sched_lock);
    return output ? copy_out(output, &old, sizeof(old)) : 0;
}
int signal_mask(int how, uint64_t input, uint64_t output)
{
    uint64_t mask = 0, old;
    struct task *t = this_cpu()->current;
    if (input && (how < SIG_BLOCK || how > SIG_SETMASK))
        return -EINVAL;
    if (input && copy_in(&mask, input, 8))
        return -EFAULT;
    if (output && !user_range(t->root, output, 8, true))
        return -EFAULT;
    mask &= SIGNAL_BITS & ~SIGNAL_UNBLOCKABLE;
    spin_lock(&sched_lock);
    old = t->signal_mask;
    if (input) {
        if (how == SIG_BLOCK)
            t->signal_mask |= mask;
        else if (how == SIG_UNBLOCK)
            t->signal_mask &= ~mask;
        else
            t->signal_mask = mask;
    }
    spin_unlock(&sched_lock);
    return output ? copy_out(output, &old, 8) : 0;
}
int signal_pending(uint64_t output)
{
    struct task *t = this_cpu()->current;
    spin_lock(&sched_lock);
    uint64_t pending = t->pending;
    spin_unlock(&sched_lock);
    return copy_out(output, &pending, 8);
}
int signal_suspend(uint64_t input)
{
    uint64_t mask;
    if (copy_in(&mask, input, 8))
        return -EFAULT;
    struct task *t = this_cpu()->current;
    spin_lock(&sched_lock);
    t->suspend_mask = t->signal_mask;
    t->suspended = true;
    t->signal_mask = mask & SIGNAL_BITS & ~SIGNAL_UNBLOCKABLE;
    t->state = BLOCKED;
    t->wake = 0;
    if (t->pending & ~t->signal_mask)
        t->state = READY;
    spin_unlock(&sched_lock);
    return 0;
}
static bool executable(struct task *t, uint64_t p)
{
    if (p < PAGE_SIZE || p >= USER_TOP)
        return false;
    uint64_t *entry = page_entry(t->root, p, false);
    return entry && (*entry & (PTE_P | PTE_U)) == (PTE_P | PTE_U) && !(*entry & PTE_NX);
}
bool signal_deliver(struct frame *f)
{
    struct task *t = this_cpu()->current;
    if (!t || t->kernel || (f->cs & 3) != 3)
        return true;
    for (;;) {
        spin_lock(&sched_lock);
        uint64_t pending = t->pending & (~t->signal_mask | SIGNAL_UNBLOCKABLE);
        if (!pending) {
            t->retrying = false;
            t->sleeping = false;
            spin_unlock(&sched_lock);
            return true;
        }
        int s = (pending & bit(SIGKILL)) ? SIGKILL : (int)__builtin_ctzll(pending) + 1;
        t->pending &= ~bit(s);
        struct ksigaction action = t->actions[s];
        struct ksiginfo info = t->signal_info[s];
        if (ignored(t, s)) {
            spin_unlock(&sched_lock);
            continue;
        }
        if (s == SIGSTOP || (!action.handler && (s == SIGTSTP || s == SIGTTIN || s == SIGTTOU))) {
            t->state = STOPPED;
            t->stop_event = true;
            t->continue_event = false;
            t->wake = 0;
            signal_child_locked(t, SIGCHLD, (s << 8) | 127);
            t->waiting = s;
            spin_unlock(&sched_lock);
            return false;
        }
        if (!action.handler || s == SIGKILL) {
            spin_unlock(&sched_lock);
            task_signal_exit(s);
            return false;
        }
        uint64_t oldmask = t->suspended ? t->suspend_mask : t->signal_mask;
        if (action.flags & SA_RESETHAND)
            memset(&t->actions[s], 0, sizeof(action));
        t->signal_mask |= action.mask;
        if (!(action.flags & SA_NODEFER))
            t->signal_mask |= bit(s);
        t->signal_mask &= ~SIGNAL_UNBLOCKABLE;
        spin_unlock(&sched_lock);
        if (t->retrying && !(action.flags & SA_RESTART)) {
            f->rip += 2;
            f->rax = (uint64_t)-EINTR;
        }
        if (t->sleeping) {
            uint64_t now = monotonic_ns(),
                     left = t->sleep_deadline > now ? t->sleep_deadline - now : 0;
            struct ktimespec remainder = {left / 1000000000UL, left % 1000000000UL};
            if (t->sleep_remaining && copy_out(t->sleep_remaining, &remainder, sizeof(remainder))) {
                task_signal_exit(SIGSEGV);
                return false;
            }
            f->rax = (uint64_t)-EINTR;
        }
        if (t->suspended)
            f->rax = (uint64_t)-EINTR;
        spin_lock(&sched_lock);
        t->retrying = t->sleeping = t->suspended = false;
        spin_unlock(&sched_lock);
        if (t->signal_depth >= 16 || f->rsp < sizeof(struct ksignal_frame) + 256 ||
            !executable(t, action.handler) || !executable(t, action.restorer)) {
            task_signal_exit(SIGSEGV);
            return false;
        }
        uint64_t base = ALIGN_DOWN(f->rsp - 128 - sizeof(struct ksignal_frame), 16);
        struct ksignal_frame frame = {
            .magic = SIGNAL_MAGIC, .previous = t->signal_frame, .mask = oldmask, .info = info};
        memcpy(&frame.context, f, sizeof(*f));
        __asm__ volatile("fxsave64 %0" : "=m"(frame.fpstate)::"memory");
        if (copy_out(base, &frame, sizeof(frame)) || copy_out(base - 8, &action.restorer, 8)) {
            task_signal_exit(SIGSEGV);
            return false;
        }
        t->signal_frame = base;
        t->signal_depth++;
        f->rip = action.handler;
        f->rsp = base - 8;
        f->rdi = s;
        f->rsi = base + offsetof(struct ksignal_frame, info);
        f->rdx = base + offsetof(struct ksignal_frame, context);
        f->vector = 0;
        f->flags = (f->flags & 0x240cd5) | 0x202;
        return true;
    }
}
int signal_restore(uint64_t base, struct frame *f)
{
    struct task *t = this_cpu()->current;
    struct ksignal_frame frame;
    if (!t->signal_depth || base != t->signal_frame || copy_in(&frame, base, sizeof(frame)) ||
        frame.magic != SIGNAL_MAGIC)
        return -EFAULT;
    if (!executable(t, frame.context.rip) || frame.context.rsp < PAGE_SIZE ||
        frame.context.rsp >= USER_TOP)
        return -EFAULT;
    uint32_t mxcsr;
    memcpy(&mxcsr, frame.fpstate + 24, 4);
    if (mxcsr & ~0xffbfU)
        return -EINVAL;
    memcpy(f, &frame.context, sizeof(*f));
    f->cs = 35;
    f->ss = 27;
    f->vector = f->error = 0;
    f->flags = (f->flags & 0x240dd5) | 0x202;
    spin_lock(&sched_lock);
    t->signal_mask = frame.mask & SIGNAL_BITS & ~SIGNAL_UNBLOCKABLE;
    spin_unlock(&sched_lock);
    t->signal_frame = frame.previous;
    t->signal_depth--;
    memcpy(t->fpstate, frame.fpstate, 512);
    __asm__ volatile("fxrstor64 %0" ::"m"(t->fpstate) : "memory");
    return 0;
}
int process_group(int pid, int group, bool session)
{
    struct task *self = this_cpu()->current;
    spin_lock(&sched_lock);
    if (session) {
        for (unsigned i = 0; i < MAX_TASKS; i++)
            if (tasks[i].pid && tasks[i].pgid == self->pid) {
                spin_unlock(&sched_lock);
                return -EPERM;
            }
        self->sid = self->pgid = self->pid;
        spin_unlock(&sched_lock);
        return self->sid;
    }
    if (pid < 0 || group < 0) {
        spin_unlock(&sched_lock);
        return -EINVAL;
    }
    struct task *target = NULL;
    for (unsigned i = 0; i < MAX_TASKS; i++)
        if (tasks[i].pid == (pid ? pid : self->pid))
            target = &tasks[i];
    if (!target || (target != self && target->ppid != self->pid)) {
        spin_unlock(&sched_lock);
        return -ESRCH;
    }
    if (target->sid != self->sid || target->sid == target->pid ||
        (target != self && target->execed)) {
        spin_unlock(&sched_lock);
        return -EPERM;
    }
    if (!group)
        group = target->pid;
    bool found = group == target->pid;
    for (unsigned i = 0; i < MAX_TASKS; i++)
        if (tasks[i].pid && tasks[i].pgid == group && tasks[i].sid == self->sid)
            found = true;
    if (!found) {
        spin_unlock(&sched_lock);
        return -EPERM;
    }
    target->pgid = group;
    spin_unlock(&sched_lock);
    return 0;
}
int terminal_group(int group, bool set)
{
    if (!set)
        return atomic_load(&terminal_pgid);
    struct task *self = this_cpu()->current;
    spin_lock(&sched_lock);
    if (terminal_sid && self->sid != terminal_sid) {
        spin_unlock(&sched_lock);
        return -EPERM;
    }
    bool found = false;
    for (unsigned i = 0; i < MAX_TASKS; i++)
        if (tasks[i].pid && tasks[i].pgid == group && tasks[i].sid == self->sid)
            found = true;
    if (found) {
        terminal_sid = self->sid;
        atomic_store(&terminal_pgid, group);
    }
    spin_unlock(&sched_lock);
    return found ? 0 : -EPERM;
}
void terminal_signal(int s)
{
    int group = atomic_load(&terminal_pgid);
    if (!group)
        return;
    spin_lock(&sched_lock);
    for (unsigned i = 0; i < MAX_TASKS; i++)
        if (tasks[i].pid && tasks[i].pgid == group) {
            struct ksiginfo info = {.signo = s, .code = 2};
            signal_queue_locked(&tasks[i], s, &info);
        }
    spin_unlock(&sched_lock);
}
