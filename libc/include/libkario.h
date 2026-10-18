#ifndef LIBKARIO_H
#define LIBKARIO_H
#include <kario.h>
#include <stdarg.h>
#include <stdbool.h>
typedef int64_t ssize_t;
typedef int64_t off_t;
typedef int pid_t;
extern int errno;
extern char **environ;
#define EOF (-1)
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define MAP_FAILED ((void *)-1)
typedef uint64_t sigset_t;
typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);
struct sigaction {
    union {
        sighandler_t sa_handler;
        void (*sa_sigaction)(int, struct ksiginfo *, void *);
    };
    sigset_t sa_mask;
    int sa_flags;
};
#define WIFSIGNALED(s) (((s) & 127) != 0 && ((s) & 127) != 127)
#define WTERMSIG(s) ((s) & 127)
#define WIFSTOPPED(s) (((s) & 255) == 127)
#define WSTOPSIG(s) (((s) >> 8) & 255)
#define WIFCONTINUED(s) ((s) == 0xffff)
int pipe(int[2]);
int pipe2(int[2], int);
int fcntl(int, int, ...);
int kill(pid_t, int);
int raise(int);
int sigaction(int, const struct sigaction *, struct sigaction *);
sighandler_t signal(int, sighandler_t);
int sigprocmask(int, const sigset_t *, sigset_t *);
int sigpending(sigset_t *);
int sigsuspend(const sigset_t *);
int pause(void);
int sigemptyset(sigset_t *);
int sigfillset(sigset_t *);
int sigaddset(sigset_t *, int);
int sigdelset(sigset_t *, int);
int sigismember(const sigset_t *, int);
unsigned alarm(unsigned);
pid_t getppid(void);
int setpgid(pid_t, pid_t);
pid_t getpgid(pid_t);
pid_t getpgrp(void);
pid_t setsid(void);
pid_t tcgetpgrp(int);
int tcsetpgrp(int, pid_t);
#define WIFEXITED(s) (((s) & 255) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 255)
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
int open(const char *, int, ...);
int close(int);
off_t lseek(int, off_t, int);
int fstat(int, struct kstat *);
pid_t getpid(void);
_Noreturn void exit(int);
int yield(void);
int nanosleep(const struct ktimespec *, struct ktimespec *);
void *mmap(void *, size_t, int, int, int, off_t);
int munmap(void *, size_t);
int execve(const char *, char *const *, char *const *);
int execvp(const char *, char *const *);
pid_t fork(void);
pid_t waitpid(pid_t, int *, int);
int chdir(const char *);
int chroot(const char *);
char *getcwd(char *, size_t);
int readdir(int, struct kdirent *);
int mkdir(const char *, unsigned);
int unlink(const char *);
int rmdir(const char *);
int link(const char *, const char *);
int symlink(const char *, const char *);
ssize_t readlink(const char *, char *, size_t);
int chmod(const char *, unsigned);
int sysinfo(struct ksysinfo *);
int mount(const char *, const char *);
int mountpoint(const char *, char *, size_t);
int partscan(const char *);
int dup2(int, int);
int ftruncate(int, off_t);
unsigned getuid(void);
unsigned getgid(void);
int setuid(unsigned);
int setgid(unsigned);
int chown(const char *, unsigned, unsigned);
unsigned umask(unsigned);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strcpy(char *, const char *);
char *strchr(const char *, int);
char *strrchr(const char *, int);
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *malloc(size_t);
void free(void *);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
long strtol(const char *, char **, int);
int atoi(const char *);
char *getenv(const char *);
const char *strerror(int);
void perror(const char *);
typedef struct {
    int fd, error, eof, owned;
    char *memory;
    size_t position, length, capacity;
    char **memory_result;
    size_t *length_result;
} FILE;
extern FILE *stdin, *stdout, *stderr;
FILE *fopen(const char *, const char *);
size_t fread(void *, size_t, size_t, FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);
int fclose(FILE *);
int fflush(FILE *);
int fseek(FILE *, long, int);
long ftell(FILE *);
int feof(FILE *);
int ferror(FILE *);
void clearerr(FILE *);
int fgetc(FILE *);
int fputc(int, FILE *);
char *fgets(char *, int, FILE *);
int fputs(const char *, FILE *);
int vfprintf(FILE *, const char *, va_list);
int fprintf(FILE *, const char *, ...);
int printf(const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int puts(const char *);
int putchar(int);
int getchar(void);
char *strdup(const char *);
char *strndup(const char *, size_t);
char *strncpy(char *, const char *, size_t);
char *strcat(char *, const char *);
char *strstr(const char *, const char *);
char *strpbrk(const char *, const char *);
size_t strspn(const char *, const char *);
size_t strcspn(const char *, const char *);
char *strtok(char *, const char *);
int strcasecmp(const char *, const char *);
int strncasecmp(const char *, const char *, size_t);
void *memchr(const void *, int, size_t);
unsigned long strtoul(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);
long long strtoll(const char *, char **, int);
long double strtold(const char *, char **);
double strtod(const char *, char **);
float strtof(const char *, char **);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
int atexit(void (*)(void));
_Noreturn void _exit(int);
_Noreturn void abort(void);
int mkstemp(char *);
FILE *fdopen(int, const char *);
FILE *open_memstream(char **, size_t *);
int fileno(FILE *);
int vsnprintf(char *, size_t, const char *, va_list);
int sprintf(char *, const char *, ...);
int vsprintf(char *, const char *, va_list);
int remove(const char *);
int access(const char *, int);
char *realpath(const char *, char *);
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
#endif
