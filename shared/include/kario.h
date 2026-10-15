#ifndef KARIO_ABI_H
#define KARIO_ABI_H
#include <stdint.h>
#include <stddef.h>
#define KARIO_VERSION "v0.1.0"
#define PAGE_SIZE 4096UL
#define PATH_MAX 256
#define NAME_MAX 255
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 64
#define O_EXCL 128
#define O_TRUNC 512
#define O_APPEND 1024
#define O_DIRECTORY 65536
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 32
#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 8
#define O_NONBLOCK 2048
#define O_CLOEXEC 524288
#define FD_CLOEXEC 1
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define PIPE_BUF 4096
#define S_IFIFO 0010000
#define EINTR 4
#define EPIPE 32
#define NSIG 32
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGWINCH 28
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int)) - 1)
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define SA_RESTART 1
#define SA_NODEFER 2
#define SA_RESETHAND 4
#define SA_SIGINFO 8
#define SA_NOCLDSTOP 16
#define SA_NOCLDWAIT 32
#define SIGNAL_UNBLOCKABLE ((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)))
#define SIGNAL_BITS 0x7fffffffUL
struct ksigaction {
    uint64_t handler, mask, restorer;
    uint32_t flags, reserved;
};
struct ksiginfo {
    int signo, code, pid;
    unsigned uid;
    int status;
    uint64_t address;
};
struct ksigcontext {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rsi, rdi, rbp, rdx, rcx, rbx, rax, vector, error,
        rip, cs, flags, rsp, ss;
};
struct ksignal_frame {
    uint64_t magic, previous, mask;
    struct ksiginfo info;
    struct ksigcontext context;
    _Alignas(16) uint8_t fpstate[512];
};
#define S_IFMT 0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFLNK 0120000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EIO 5
#define ENXIO 6
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENOTTY 25
#define ENFILE 23
#define EMFILE 24
#define EFBIG 27
#define ESPIPE 29
#define ENOSPC 28
#define EROFS 30
#define EMLINK 31
#define ERANGE 34
#define ENOSYS 38
#define ENOTEMPTY 39
#define ELOOP 40
#define ENAMETOOLONG 36
#define EOVERFLOW 75
#define ENOTSUP 95
#define ETIMEDOUT 110
struct kstat {
    uint64_t ino, size, blocks;
    uint32_t mode, nlink, uid, gid;
    uint64_t atime, mtime, ctime;
};
struct kdirent {
    uint64_t ino;
    uint32_t mode;
    char name[NAME_MAX + 1];
};
struct ktimespec {
    int64_t tv_sec, tv_nsec;
};
struct ksysinfo {
    uint64_t total, free, reserved, uptime_ns;
    uint32_t cpus;
    char release[32];
    uint64_t realtime_sec;
};
enum {
    SYS_read,
    SYS_write,
    SYS_open,
    SYS_close,
    SYS_lseek,
    SYS_fstat,
    SYS_getpid,
    SYS_exit,
    SYS_yield,
    SYS_nanosleep,
    SYS_mmap,
    SYS_munmap,
    SYS_execve,
    SYS_waitpid,
    SYS_chdir,
    SYS_getcwd,
    SYS_readdir,
    SYS_fork,
    SYS_mkdir,
    SYS_unlink,
    SYS_rmdir,
    SYS_link,
    SYS_symlink,
    SYS_readlink,
    SYS_chmod,
    SYS_sysinfo,
    SYS_mount,
    SYS_dup2,
    SYS_ftruncate,
    SYS_getuid,
    SYS_getgid,
    SYS_setuid,
    SYS_setgid,
    SYS_chown,
    SYS_umask,
    SYS_partscan,
    SYS_pipe2,
    SYS_fcntl,
    SYS_kill,
    SYS_sigaction,
    SYS_sigprocmask,
    SYS_sigpending,
    SYS_sigsuspend,
    SYS_sigreturn,
    SYS_alarm,
    SYS_getppid,
    SYS_setpgid,
    SYS_getpgid,
    SYS_setsid,
    SYS_tcgetpgrp,
    SYS_tcsetpgrp,
    SYS_COUNT
};
#endif
