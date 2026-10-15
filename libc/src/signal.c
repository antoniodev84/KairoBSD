#include <libkario.h>
_Static_assert(SYS_sigreturn == 43, "sigreturn assembly ABI mismatch");
extern void __kario_sigreturn(void);
extern int64_t __kario_call(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int pipe(int fds[2])
{
    return pipe2(fds, 0);
}
int pipe2(int fds[2], int flags)
{
    return __kario_call(SYS_pipe2, (uint64_t)fds, flags, 0, 0, 0, 0);
}
int fcntl(int fd, int command, ...)
{
    uint64_t argument = 0;
    if (command == F_SETFL || command == F_SETFD) {
        va_list ap;
        va_start(ap, command);
        argument = va_arg(ap, int);
        va_end(ap);
    }
    return __kario_call(SYS_fcntl, fd, command, argument, 0, 0, 0);
}
int kill(pid_t pid, int signo)
{
    return __kario_call(SYS_kill, pid, signo, 0, 0, 0, 0);
}
int raise(int signo)
{
    return kill(getpid(), signo);
}
int sigaction(int signo, const struct sigaction *action, struct sigaction *old)
{
    struct ksigaction input = {0}, output;
    if (action) {
        input.handler = (uint64_t)action->sa_handler;
        input.mask = action->sa_mask;
        input.flags = action->sa_flags;
        input.restorer = (uint64_t)__kario_sigreturn;
    }
    int result = __kario_call(SYS_sigaction, signo, action ? (uint64_t)&input : 0,
                              old ? (uint64_t)&output : 0, 0, 0, 0);
    if (!result && old) {
        old->sa_handler = (sighandler_t)output.handler;
        old->sa_mask = output.mask;
        old->sa_flags = output.flags;
    }
    return result;
}
sighandler_t signal(int signo, sighandler_t handler)
{
    struct sigaction action = {.sa_handler = handler, .sa_flags = SA_RESTART}, old;
    if (sigaction(signo, &action, &old))
        return SIG_ERR;
    return old.sa_handler;
}
int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    return __kario_call(SYS_sigprocmask, how, (uint64_t)set, (uint64_t)old, 0, 0, 0);
}
int sigpending(sigset_t *set)
{
    return __kario_call(SYS_sigpending, (uint64_t)set, 0, 0, 0, 0, 0);
}
int sigsuspend(const sigset_t *set)
{
    return __kario_call(SYS_sigsuspend, (uint64_t)set, 0, 0, 0, 0, 0);
}
int pause(void)
{
    sigset_t mask;
    if (sigprocmask(SIG_BLOCK, NULL, &mask))
        return -1;
    return sigsuspend(&mask);
}
int sigemptyset(sigset_t *set)
{
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    *set = 0;
    return 0;
}
int sigfillset(sigset_t *set)
{
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    *set = SIGNAL_BITS;
    return 0;
}
int sigaddset(sigset_t *set, int s)
{
    if (!set || s <= 0 || s >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    *set |= 1UL << (s - 1);
    return 0;
}
int sigdelset(sigset_t *set, int s)
{
    if (!set || s <= 0 || s >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    *set &= ~(1UL << (s - 1));
    return 0;
}
int sigismember(const sigset_t *set, int s)
{
    if (!set || s <= 0 || s >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    return (*set >> (s - 1)) & 1;
}
unsigned alarm(unsigned seconds)
{
    return __kario_call(SYS_alarm, seconds, 0, 0, 0, 0, 0);
}
pid_t getppid(void)
{
    return __kario_call(SYS_getppid, 0, 0, 0, 0, 0, 0);
}
int setpgid(pid_t pid, pid_t group)
{
    return __kario_call(SYS_setpgid, pid, group, 0, 0, 0, 0);
}
pid_t getpgid(pid_t pid)
{
    return __kario_call(SYS_getpgid, pid, 0, 0, 0, 0, 0);
}
pid_t getpgrp(void)
{
    return getpgid(0);
}
pid_t setsid(void)
{
    return __kario_call(SYS_setsid, 0, 0, 0, 0, 0, 0);
}
pid_t tcgetpgrp(int fd)
{
    return __kario_call(SYS_tcgetpgrp, fd, 0, 0, 0, 0, 0);
}
int tcsetpgrp(int fd, pid_t group)
{
    return __kario_call(SYS_tcsetpgrp, fd, group, 0, 0, 0, 0);
}
