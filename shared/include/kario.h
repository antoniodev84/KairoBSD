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
#define O_NOSYMLINKS 1048576
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
#define AF_UNSPEC 0
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_NONBLOCK O_NONBLOCK
#define SOCK_CLOEXEC O_CLOEXEC
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define S_IFSOCK 0140000
#define SOL_SOCKET 1
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_ACCEPTCONN 30
#define MSG_NOSIGNAL 16384
#define MSG_PEEK 2
#define MSG_DONTWAIT 64
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENETDOWN 100
#define ENETUNREACH 101
#define ECONNABORTED 103
#define ECONNRESET 104
#define ENOBUFS 105
#define EISCONN 106
#define ENOTCONN 107
#define ECONNREFUSED 111
#define EALREADY 114
#define EINPROGRESS 115
#define EMSGSIZE 90
#define ENOTSOCK 88
#define EDESTADDRREQ 89
#define EPROTONOSUPPORT 93
#define EAFNOSUPPORT 97
struct in_addr {
    uint32_t s_addr;
};
struct sockaddr {
    uint16_t sa_family;
    char sa_data[14];
};
struct sockaddr_in {
    uint16_t sin_family, sin_port;
    struct in_addr sin_addr;
    uint8_t sin_zero[8];
};
#define NET_DHCP_DISABLED 0
#define NET_DHCP_SELECTING 1
#define NET_DHCP_REQUESTING 2
#define NET_DHCP_PROBING 3
#define NET_DHCP_BOUND 4
#define NET_DHCP_RENEWING 5
#define NET_DHCP_REBINDING 6
struct knetconfig {
    uint32_t index, address, netmask, gateway, dns[2], dhcp, dhcp_state;
    uint64_t lease_remaining;
    uint8_t mac[6];
    uint16_t up;
};
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
    SYS_socket,
    SYS_bind,
    SYS_connect,
    SYS_listen,
    SYS_accept,
    SYS_sendto,
    SYS_recvfrom,
    SYS_shutdown,
    SYS_getsockname,
    SYS_getpeername,
    SYS_netconfig,
    SYS_getsockopt,
    SYS_chroot,
    SYS_mountpoint,
    SYS_COUNT
};
#endif
