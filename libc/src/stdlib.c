#include <libkario.h>
struct header {
    _Alignas(16) uint64_t magic;
    size_t mapped, size;
};
static struct header *bins[7];
#define MAGIC 0x4b4152494f4d454dUL
void *malloc(size_t n)
{
    if (!n)
        n = 1;
    if (n > SIZE_MAX - 4095 - sizeof(struct header)) {
        errno = ENOMEM;
        return NULL;
    }
    size_t need = n + sizeof(struct header);
    unsigned bin = 0;
    size_t unit = 64;
    while (unit < need && bin < 6) {
        unit <<= 1;
        bin++;
    }
    if (unit >= need) {
        struct header *h = bins[bin];
        if (!h) {
            void *region =
                mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (region == MAP_FAILED)
                return NULL;
            for (size_t off = 0; off < 65536; off += unit) {
                struct header *block = (void *)((char *)region + off);
                block->magic = (uintptr_t)bins[bin];
                block->mapped = unit | 1;
                bins[bin] = block;
            }
            h = bins[bin];
        }
        bins[bin] = (void *)(uintptr_t)h->magic;
        h->magic = MAGIC;
        h->size = n;
        return h + 1;
    }
    size_t bytes = (need + 4095) & ~4095UL;
    struct header *h =
        mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (h == MAP_FAILED)
        return NULL;
    h->magic = MAGIC;
    h->mapped = bytes;
    h->size = n;
    return h + 1;
}
void free(void *p)
{
    if (!p)
        return;
    struct header *h = (struct header *)p - 1;
    if (h->magic != MAGIC) {
        errno = EINVAL;
        return;
    }
    size_t n = h->mapped;
    h->magic = 0;
    if (n & 1) {
        unsigned bin = 0;
        for (size_t unit = 64; unit < (n & ~1UL); unit <<= 1)
            bin++;
        h->magic = (uintptr_t)bins[bin];
        bins[bin] = h;
    } else
        munmap(h, n);
}
void *calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    size_t n = count * size;
    void *p = malloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}
void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
    if (!n) {
        free(p);
        return NULL;
    }
    struct header *h = (struct header *)p - 1;
    if (h->magic != MAGIC) {
        errno = EINVAL;
        return NULL;
    }
    if (n <= h->size) {
        h->size = n;
        return p;
    }
    void *q = malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, h->size);
    free(p);
    return q;
}
long strtol(const char *s, char **end, int base)
{
    const char *original = s;
    while (*s == ' ' || *s == '\t' || *s == '\n')
        s++;
    bool neg = false;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    if (base && (base < 2 || base > 36)) {
        errno = EINVAL;
        if (end)
            *end = (char *)original;
        return 0;
    }
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    }
    if (!base)
        base = *s == '0' ? 8 : 10;
    uint64_t v = 0, limit = neg ? (1UL << 63) : INT64_MAX;
    bool seen = false, overflow = false;
    for (;; s++) {
        unsigned d = *s >= '0' && *s <= '9'   ? *s - '0'
                     : *s >= 'a' && *s <= 'z' ? *s - 'a' + 10
                     : *s >= 'A' && *s <= 'Z' ? *s - 'A' + 10
                                              : 36;
        if (d >= (unsigned)base)
            break;
        seen = true;
        if (v > (limit - d) / (unsigned)base)
            overflow = true;
        else
            v = v * base + d;
    }
    if (end)
        *end = (char *)(seen ? s : original);
    if (overflow) {
        errno = ERANGE;
        return neg ? INT64_MIN : INT64_MAX;
    }
    return neg ? (long)(0 - v) : (long)v;
}
int atoi(const char *s)
{
    return strtol(s, NULL, 10);
}
char *getenv(const char *name)
{
    size_t n = strlen(name);
    if (environ)
        for (char **e = environ; *e; e++)
            if (!strncmp(*e, name, n) && (*e)[n] == '=')
                return *e + n + 1;
    return NULL;
}
const char *strerror(int e)
{
    switch (e) {
        case EINTR:
            return "Interrupted system call";
        case EPIPE:
            return "Broken pipe";
        case ENOTTY:
            return "Not a terminal";
        case EPERM:
            return "Operation not permitted";
        case ENOENT:
            return "No such file or directory";
        case EIO:
            return "Input/output error";
        case ENOEXEC:
            return "Invalid executable";
        case EBADF:
            return "Bad file descriptor";
        case ECHILD:
            return "No child process";
        case EAGAIN:
            return "Try again";
        case ENOMEM:
            return "Out of memory";
        case EACCES:
            return "Permission denied";
        case EFAULT:
            return "Invalid address";
        case EBUSY:
            return "Resource busy";
        case EEXIST:
            return "File exists";
        case EXDEV:
            return "Cross-device link";
        case ENODEV:
            return "No such device";
        case ENOTDIR:
            return "Not a directory";
        case EISDIR:
            return "Is a directory";
        case EINVAL:
            return "Invalid argument";
        case EMFILE:
            return "Too many open files";
        case ENOSPC:
            return "No space left";
        case EROFS:
            return "Read-only filesystem";
        case ERANGE:
            return "Out of range";
        case ENOSYS:
            return "Unknown syscall";
        case ENOTEMPTY:
            return "Directory not empty";
        case ELOOP:
            return "Too many symbolic links";
        case ENAMETOOLONG:
            return "Path too long";
        case ENOTSUP:
            return "Operation not supported";
        case EADDRINUSE:
            return "Address already in use";
        case EADDRNOTAVAIL:
            return "Address not available";
        case ENETDOWN:
            return "Network is down";
        case ENETUNREACH:
            return "Network unreachable";
        case ECONNABORTED:
            return "Connection aborted";
        case ECONNRESET:
            return "Connection reset";
        case ENOBUFS:
            return "Network buffers exhausted";
        case EISCONN:
            return "Socket already connected";
        case ENOTCONN:
            return "Socket not connected";
        case ECONNREFUSED:
            return "Connection refused";
        case EALREADY:
            return "Connection already in progress";
        case EINPROGRESS:
            return "Connection in progress";
        case EMSGSIZE:
            return "Message too large";
        case ENOTSOCK:
            return "Not a socket";
        case EDESTADDRREQ:
            return "Destination required";
        case EPROTONOSUPPORT:
            return "Protocol not supported";
        case EAFNOSUPPORT:
            return "Address family not supported";
        case ETIMEDOUT:
            return "Device timed out";
        default:
            return "Error";
    }
}
void perror(const char *s)
{
    int e = errno;
    fprintf(stderr, "%s: %s (%d)\n", s, strerror(e), e);
}

static void (*exit_handlers[64])(void);
static unsigned exit_count;
int atexit(void (*fn)(void))
{
    if (exit_count == 64) {
        errno = ENOMEM;
        return -1;
    }
    exit_handlers[exit_count++] = fn;
    return 0;
}
_Noreturn void exit(int status)
{
    while (exit_count)
        exit_handlers[--exit_count]();
    fflush(NULL);
    _exit(status);
}
