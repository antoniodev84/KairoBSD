#include <libkario.h>
int errno;
char **environ;
int64_t __kario_call(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
                     uint64_t f)
{
    register uint64_t r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f;
    int64_t r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory", "cc");
    if (r < 0 && r >= -4095) {
        errno = -r;
        return -1;
    }
    return r;
}
#define C(n, a, b, c, d, e, f)                                                                     \
    __kario_call(SYS_##n, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d),              \
                 (uint64_t)(e), (uint64_t)(f))
ssize_t read(int f, void *p, size_t n)
{
    return C(read, f, p, n, 0, 0, 0);
}
ssize_t write(int f, const void *p, size_t n)
{
    return C(write, f, p, n, 0, 0, 0);
}
int open(const char *p, int flags, ...)
{
    unsigned mode = 0;
    if (flags & O_CREAT) {
        va_list a;
        va_start(a, flags);
        mode = va_arg(a, unsigned);
        va_end(a);
    }
    return C(open, p, flags, mode, 0, 0, 0);
}
int close(int f)
{
    return C(close, f, 0, 0, 0, 0, 0);
}
off_t lseek(int f, off_t o, int w)
{
    return C(lseek, f, o, w, 0, 0, 0);
}
int fstat(int f, struct kstat *s)
{
    return C(fstat, f, s, 0, 0, 0, 0);
}
pid_t getpid(void)
{
    return C(getpid, 0, 0, 0, 0, 0, 0);
}
_Noreturn void _exit(int s)
{
    C(exit, s, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}
int yield(void)
{
    return C(yield, 0, 0, 0, 0, 0, 0);
}
int nanosleep(const struct ktimespec *s, struct ktimespec *r)
{
    return C(nanosleep, s, r, 0, 0, 0, 0);
}
void *mmap(void *p, size_t n, int prot, int flags, int fd, off_t off)
{
    return (void *)C(mmap, p, n, prot, flags, fd, off);
}
int munmap(void *p, size_t n)
{
    return C(munmap, p, n, 0, 0, 0, 0);
}
int execve(const char *p, char *const *a, char *const *e)
{
    return C(execve, p, a, e, 0, 0, 0);
}
pid_t fork(void)
{
    return C(fork, 0, 0, 0, 0, 0, 0);
}
pid_t waitpid(pid_t p, int *s, int o)
{
    return C(waitpid, p, s, o, 0, 0, 0);
}
int chdir(const char *p)
{
    return C(chdir, p, 0, 0, 0, 0, 0);
}
char *getcwd(char *p, size_t n)
{
    int64_t r = C(getcwd, p, n, 0, 0, 0, 0);
    return r == -1 ? NULL : (char *)r;
}
int readdir(int f, struct kdirent *d)
{
    return C(readdir, f, d, 0, 0, 0, 0);
}
int mkdir(const char *p, unsigned m)
{
    return C(mkdir, p, m, 0, 0, 0, 0);
}
int unlink(const char *p)
{
    return C(unlink, p, 0, 0, 0, 0, 0);
}
int rmdir(const char *p)
{
    return C(rmdir, p, 0, 0, 0, 0, 0);
}
int link(const char *a, const char *b)
{
    return C(link, a, b, 0, 0, 0, 0);
}
int symlink(const char *a, const char *b)
{
    return C(symlink, a, b, 0, 0, 0, 0);
}
ssize_t readlink(const char *p, char *b, size_t n)
{
    return C(readlink, p, b, n, 0, 0, 0);
}
int chmod(const char *p, unsigned m)
{
    return C(chmod, p, m, 0, 0, 0, 0);
}
int sysinfo(struct ksysinfo *s)
{
    return C(sysinfo, s, 0, 0, 0, 0, 0);
}
int mount(const char *d, const char *p)
{
    return C(mount, d, p, 0, 0, 0, 0);
}
int dup2(int a, int b)
{
    return C(dup2, a, b, 0, 0, 0, 0);
}
int ftruncate(int fd, off_t n)
{
    return C(ftruncate, fd, n, 0, 0, 0, 0);
}
unsigned getuid(void)
{
    return C(getuid, 0, 0, 0, 0, 0, 0);
}
unsigned getgid(void)
{
    return C(getgid, 0, 0, 0, 0, 0, 0);
}
int setuid(unsigned uid)
{
    return C(setuid, uid, 0, 0, 0, 0, 0);
}
int setgid(unsigned gid)
{
    return C(setgid, gid, 0, 0, 0, 0, 0);
}
int chown(const char *p, unsigned uid, unsigned gid)
{
    return C(chown, p, uid, gid, 0, 0, 0);
}
unsigned umask(unsigned mask)
{
    return C(umask, mask, 0, 0, 0, 0, 0);
}
int execvp(const char *p, char *const *argv)
{
    if (strchr(p, '/'))
        return execve(p, argv, environ);
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "/bin/%s", p);
    if (n < 0 || n >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return execve(path, argv, environ);
}

int partscan(const char *p)
{
    return C(partscan, p, 0, 0, 0, 0, 0);
}
