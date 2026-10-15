#include <libkario.h>
#include <ctype.h>
#include <sys/stat.h>
int isspace(int c)
{
    return c == ' ' || (c >= 9 && c <= 13);
}
int isdigit(int c)
{
    return c >= '0' && c <= '9';
}
int islower(int c)
{
    return c >= 'a' && c <= 'z';
}
int isupper(int c)
{
    return c >= 'A' && c <= 'Z';
}
int isalpha(int c)
{
    return islower(c) || isupper(c);
}
int isalnum(int c)
{
    return isalpha(c) || isdigit(c);
}
int isxdigit(int c)
{
    return isdigit(c) || (tolower(c) >= 'a' && tolower(c) <= 'f');
}
int iscntrl(int c)
{
    return (c >= 0 && c < 32) || c == 127;
}
int isprint(int c)
{
    return c >= 32 && c < 127;
}
int isgraph(int c)
{
    return c > 32 && c < 127;
}
int ispunct(int c)
{
    return isgraph(c) && !isalnum(c);
}
int tolower(int c)
{
    return isupper(c) ? c + 32 : c;
}
int toupper(int c)
{
    return islower(c) ? c - 32 : c;
}
void *memchr(const void *p, int c, size_t n)
{
    const unsigned char *s = p;
    while (n--) {
        if (*s == (unsigned char)c)
            return (void *)s;
        s++;
    }
    return NULL;
}
char *strndup(const char *s, size_t n)
{
    size_t len = 0;
    while (len < n && s[len])
        len++;
    char *p = malloc(len + 1);
    if (p) {
        memcpy(p, s, len);
        p[len] = 0;
    }
    return p;
}
char *strdup(const char *s)
{
    return strndup(s, strlen(s));
}
char *strncpy(char *d, const char *s, size_t n)
{
    char *r = d;
    while (n && *s) {
        *d++ = *s++;
        n--;
    }
    while (n--)
        *d++ = 0;
    return r;
}
char *strcat(char *d, const char *s)
{
    strcpy(d + strlen(d), s);
    return d;
}
char *strstr(const char *s, const char *n)
{
    size_t len = strlen(n);
    do {
        if (!strncmp(s, n, len))
            return (char *)s;
    } while (*s++);
    return NULL;
}
size_t strspn(const char *s, const char *set)
{
    size_t n = 0;
    while (s[n] && strchr(set, s[n]))
        n++;
    return n;
}
size_t strcspn(const char *s, const char *set)
{
    size_t n = 0;
    while (s[n] && !strchr(set, s[n]))
        n++;
    return n;
}
char *strpbrk(const char *s, const char *set)
{
    s += strcspn(s, set);
    return *s ? (char *)s : NULL;
}
char *strtok(char *s, const char *set)
{
    static char *next;
    if (!s)
        s = next;
    if (!s)
        return NULL;
    s += strspn(s, set);
    if (!*s) {
        next = NULL;
        return NULL;
    }
    char *end = s + strcspn(s, set);
    if (*end) {
        *end = 0;
        next = end + 1;
    } else
        next = NULL;
    return s;
}
int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n--) {
        int x = tolower((unsigned char)*a++), y = tolower((unsigned char)*b++);
        if (x != y || !x)
            return x - y;
    }
    return 0;
}
int strcasecmp(const char *a, const char *b)
{
    return strncasecmp(a, b, SIZE_MAX);
}
unsigned long strtoul(const char *s, char **end, int base)
{
    const char *orig = s;
    while (isspace((unsigned char)*s))
        s++;
    bool neg = false;
    if (*s == '+' || *s == '-')
        neg = *s++ == '-';
    if (base && (base < 2 || base > 36)) {
        errno = EINVAL;
        if (end)
            *end = (char *)orig;
        return 0;
    }
    if ((!base || base == 16) && s[0] == '0' && tolower(s[1]) == 'x' && isxdigit(s[2])) {
        base = 16;
        s += 2;
    }
    if (!base)
        base = *s == '0' ? 8 : 10;
    unsigned long v = 0;
    bool any = false, overflow = false;
    for (;; s++) {
        unsigned d = isdigit((unsigned char)*s)   ? *s - '0'
                     : isalpha((unsigned char)*s) ? tolower((unsigned char)*s) - 'a' + 10
                                                  : 36;
        if (d >= (unsigned)base)
            break;
        any = true;
        if (v > (UINT64_MAX - d) / (unsigned)base)
            overflow = true;
        else
            v = v * base + d;
    }
    if (end)
        *end = (char *)(any ? s : orig);
    if (overflow) {
        errno = ERANGE;
        return UINT64_MAX;
    }
    return neg ? 0 - v : v;
}
unsigned long long strtoull(const char *s, char **e, int b)
{
    return strtoul(s, e, b);
}
long long strtoll(const char *s, char **e, int b)
{
    return strtol(s, e, b);
}
long double ldexpl(long double v, int n)
{
    if (n > 32768)
        n = 32768;
    if (n < -32768)
        n = -32768;
    while (n > 0) {
        v *= 2;
        n--;
    }
    while (n < 0) {
        v *= 0.5L;
        n++;
    }
    return v;
}
double ldexp(double v, int n)
{
    return ldexpl(v, n);
}
long double strtold(const char *s, char **end)
{
    const char *orig = s;
    while (isspace((unsigned char)*s))
        s++;
    bool neg = false;
    if (*s == '+' || *s == '-')
        neg = *s++ == '-';
    if (!strncasecmp(s, "inf", 3)) {
        s += 3;
        if (!strncasecmp(s, "inity", 5))
            s += 5;
        if (end)
            *end = (char *)s;
        return neg ? -__builtin_huge_vall() : __builtin_huge_vall();
    }
    if (!strncasecmp(s, "nan", 3)) {
        s += 3;
        if (end)
            *end = (char *)s;
        return __builtin_nanl("");
    }
    bool hex = s[0] == '0' && tolower(s[1]) == 'x';
    if (hex)
        s += 2;
    int base = hex ? 16 : 10, exponent = 0;
    long double v = 0;
    bool any = false, dot = false;
    for (;; s++) {
        if (*s == '.' && !dot) {
            dot = true;
            continue;
        }
        int d = isdigit((unsigned char)*s)                        ? *s - '0'
                : hex && tolower(*s) >= 'a' && tolower(*s) <= 'f' ? tolower(*s) - 'a' + 10
                                                                  : base;
        if (d >= base)
            break;
        any = true;
        v = v * base + d;
        if (dot)
            exponent -= hex ? 4 : 1;
    }
    if (!any) {
        if (end)
            *end = (char *)orig;
        return 0;
    }
    if (tolower(*s) == (hex ? 'p' : 'e')) {
        const char *mark = s++;
        bool eneg = false;
        if (*s == '+' || *s == '-')
            eneg = *s++ == '-';
        if (!isdigit(*s))
            s = mark;
        else {
            int e = 0;
            while (isdigit(*s)) {
                if (e < 40000)
                    e = e * 10 + *s - '0';
                s++;
            }
            exponent += eneg ? -e : e;
        }
    }
    if (hex)
        v = ldexpl(v, exponent);
    else {
        bool down = exponent < 0;
        unsigned e = down ? -(long)exponent : exponent;
        long double factor = 10.L, scale = 1;
        while (e) {
            if (e & 1)
                scale *= factor;
            factor *= factor;
            e >>= 1;
        }
        if (down)
            v /= scale;
        else if (v)
            v *= scale;
    }
    if (__builtin_isinf(v) || (v == 0 && exponent))
        errno = ERANGE;
    if (end)
        *end = (char *)s;
    return neg ? -v : v;
}
double strtod(const char *s, char **e)
{
    return strtold(s, e);
}
float strtof(const char *s, char **e)
{
    return strtold(s, e);
}
static void swap(unsigned char *a, unsigned char *b, size_t n)
{
    while (n--) {
        unsigned char c = *a;
        *a++ = *b;
        *b++ = c;
    }
}
static void sift(unsigned char *a, size_t root, size_t n, size_t size,
                 int (*cmp)(const void *, const void *))
{
    while (root < n / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < n && cmp(a + child * size, a + (child + 1) * size) < 0)
            child++;
        if (cmp(a + root * size, a + child * size) >= 0)
            break;
        swap(a + root * size, a + child * size, size);
        root = child;
    }
}
void qsort(void *p, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    if (!size || n < 2 || n > SIZE_MAX / size)
        return;
    unsigned char *a = p;
    for (size_t i = n / 2; i; i--)
        sift(a, i - 1, n, size, cmp);
    for (size_t i = n - 1; i; i--) {
        swap(a, a + i * size, size);
        sift(a, 0, i, size, cmp);
    }
}
int stat(const char *path, struct stat *out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    struct kstat s;
    int r = fstat(fd, &s);
    int e = errno;
    close(fd);
    errno = e;
    if (r < 0)
        return r;
    *out = (struct stat){s.ino, s.size, s.blocks, s.mode,  s.nlink,
                         s.uid, s.gid,  s.atime,  s.mtime, s.ctime};
    return 0;
}
int access(const char *path, int mode)
{
    struct stat s;
    if (mode & ~7) {
        errno = EINVAL;
        return -1;
    }
    if (stat(path, &s) < 0)
        return -1;
    unsigned bits = getuid() == s.st_uid   ? (s.st_mode >> 6)
                    : getgid() == s.st_gid ? (s.st_mode >> 3)
                                           : s.st_mode;
    if (getuid() == 0)
        bits = 6 | ((s.st_mode & 0111) ? 1 : 0);
    if ((bits & mode) != (unsigned)mode) {
        errno = EACCES;
        return -1;
    }
    return 0;
}
int remove(const char *p)
{
    if (unlink(p) == 0)
        return 0;
    if (errno == EISDIR)
        return rmdir(p);
    return -1;
}
pid_t wait(int *s)
{
    return waitpid(-1, s, 0);
}
int mkstemp(char *s)
{
    static unsigned counter;
    size_t n = strlen(s);
    if (n < 6 || strcmp(s + n - 6, "XXXXXX")) {
        errno = EINVAL;
        return -1;
    }
    for (unsigned tries = 0; tries < 4096; tries++) {
        unsigned long v = ((unsigned long)getpid() << 24) | ++counter;
        for (unsigned i = 0; i < 6; i++) {
            s[n - 1 - i] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36];
            v /= 36;
        }
        int fd = open(s, O_RDWR | O_CREAT | O_EXCL, 0600U);
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    errno = EEXIST;
    return -1;
}
char *basename(char *p)
{
    if (!p || !*p)
        return ".";
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/')
        p[--n] = 0;
    char *s = strrchr(p, '/');
    return s && s[1] ? s + 1 : p;
}
char *dirname(char *p)
{
    if (!p || !*p)
        return ".";
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/')
        p[--n] = 0;
    char *s = strrchr(p, '/');
    if (!s)
        return ".";
    while (s > p && s[-1] == '/')
        s--;
    if (s == p) {
        p[1] = 0;
        return p;
    }
    *s = 0;
    return p;
}
_Noreturn void __assert_fail(const char *s, const char *f, int l)
{
    fprintf(stderr, "%s:%d: assertion failed: %s\n", f, l, s);
    abort();
}
_Noreturn void abort(void)
{
    raise(SIGABRT);
    _exit(128 + SIGABRT);
}
char *realpath(const char *path, char *out)
{
    if (!path || !*path) {
        errno = ENOENT;
        return NULL;
    }
    char pending[PATH_MAX], resolved[PATH_MAX];
    if (strlen(path) >= sizeof(pending)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    strcpy(pending, path);
    if (*path == '/')
        strcpy(resolved, "/");
    else if (!getcwd(resolved, sizeof(resolved)))
        return NULL;
    unsigned links = 0;
    char *cursor = pending;
    while (*cursor) {
        while (*cursor == '/')
            cursor++;
        if (!*cursor)
            break;
        char part[PATH_MAX];
        size_t n = 0;
        while (*cursor && *cursor != '/')
            part[n++] = *cursor++;
        part[n] = 0;
        if (!strcmp(part, "."))
            continue;
        if (!strcmp(part, "..")) {
            char *slash = strrchr(resolved, '/');
            if (slash == resolved)
                slash[1] = 0;
            else
                *slash = 0;
            continue;
        }
        size_t old = strlen(resolved), slash = old > 1;
        if (old + slash + n >= sizeof(resolved)) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        if (slash)
            resolved[old] = '/';
        strcpy(resolved + old + slash, part);
        char target[PATH_MAX];
        ssize_t count = readlink(resolved, target, sizeof(target) - 1);
        if (count >= 0) {
            if (++links > 40) {
                errno = ELOOP;
                return NULL;
            }
            target[count] = 0;
            if ((size_t)count + strlen(cursor) >= sizeof(pending)) {
                errno = ENAMETOOLONG;
                return NULL;
            }
            char rest[PATH_MAX];
            strcpy(rest, cursor);
            strcpy(pending, target);
            strcat(pending, rest);
            cursor = pending;
            if (target[0] == '/')
                strcpy(resolved, "/");
            else
                resolved[old] = 0;
        } else {
            if (errno != EINVAL)
                return NULL;
            struct stat st;
            if (stat(resolved, &st) < 0)
                return NULL;
            if (*cursor && !S_ISDIR(st.st_mode)) {
                errno = ENOTDIR;
                return NULL;
            }
        }
    }
    if (!out)
        return strdup(resolved);
    strcpy(out, resolved);
    return out;
}
