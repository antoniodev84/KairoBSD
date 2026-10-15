#include <libkario.h>
static FILE streams[3] = {{.fd = 0}, {.fd = 1}, {.fd = 2}};
FILE *stdin = &streams[0], *stdout = &streams[1], *stderr = &streams[2];
FILE *fopen(const char *path, const char *mode)
{
    if (!mode || !*mode) {
        errno = EINVAL;
        return NULL;
    }
    int flags;
    if (*mode == 'r')
        flags = O_RDONLY;
    else if (*mode == 'w')
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (*mode == 'a')
        flags = O_WRONLY | O_CREAT | O_APPEND;
    else {
        errno = EINVAL;
        return NULL;
    }
    bool plus = false;
    for (const char *p = mode + 1; *p; p++) {
        if (*p == '+') {
            if (plus) {
                errno = EINVAL;
                return NULL;
            }
            plus = true;
        } else if (*p != 'b') {
            errno = EINVAL;
            return NULL;
        }
    }
    if (plus)
        flags = (flags & ~3) | O_RDWR;
    int fd = open(path, flags, 0666U);
    if (fd < 0)
        return NULL;
    FILE *f = malloc(sizeof(*f));
    if (!f) {
        close(fd);
        return NULL;
    }
    *f = (FILE){.fd = fd, .owned = 1};
    return f;
}
size_t fread(void *p, size_t size, size_t n, FILE *f)
{
    if (!size || !n)
        return 0;
    if (n > SIZE_MAX / size) {
        errno = EOVERFLOW;
        f->error = 1;
        return 0;
    }
    size_t len = n * size, done = 0;
    if (f->memory_result) {
        size_t available = f->position < f->length ? f->length - f->position : 0;
        if (len > available) {
            len = available;
            f->eof = 1;
        }
        memcpy(p, f->memory + f->position, len);
        f->position += len;
        return len / size;
    }
    while (done < len) {
        ssize_t r = read(f->fd, (char *)p + done, len - done);
        if (r < 0) {
            f->error = 1;
            break;
        }
        if (!r) {
            f->eof = 1;
            break;
        }
        done += r;
    }
    return done / size;
}
size_t fwrite(const void *p, size_t size, size_t n, FILE *f)
{
    if (!size || !n)
        return 0;
    if (n > SIZE_MAX / size) {
        errno = EOVERFLOW;
        f->error = 1;
        return 0;
    }
    size_t len = n * size, done = 0;
    if (f->memory_result) {
        if (len > SIZE_MAX - f->position - 1) {
            errno = EOVERFLOW;
            f->error = 1;
            return 0;
        }
        size_t end = f->position + len;
        if (end + 1 > f->capacity) {
            size_t cap = end < SIZE_MAX / 2 ? (end + 1) * 2 : end + 1;
            char *mem = realloc(f->memory, cap);
            if (!mem) {
                f->error = 1;
                return 0;
            }
            f->memory = mem;
            f->capacity = cap;
        }
        if (f->position > f->length)
            memset(f->memory + f->length, 0, f->position - f->length);
        memcpy(f->memory + f->position, p, len);
        f->position = end;
        if (end > f->length)
            f->length = end;
        f->memory[f->length] = 0;
        return n;
    }
    while (done < len) {
        ssize_t r = write(f->fd, (const char *)p + done, len - done);
        if (r <= 0) {
            f->error = 1;
            if (!r)
                errno = EIO;
            break;
        }
        done += r;
    }
    return done / size;
}
int fclose(FILE *f)
{
    int r = f->memory_result ? fflush(f) : close(f->fd);
    if (f->owned)
        free(f);
    return r;
}
int fflush(FILE *f)
{
    if (f && f->memory_result) {
        *f->memory_result = f->memory;
        *f->length_result = f->length;
    }
    return f && f->error ? EOF : 0;
}
int fseek(FILE *f, long off, int whence)
{
    if (f->memory_result) {
        long base = whence == SEEK_SET   ? 0
                    : whence == SEEK_CUR ? (long)f->position
                    : whence == SEEK_END ? (long)f->length
                                         : -1;
        long pos;
        if (base < 0 || __builtin_add_overflow(base, off, &pos) || pos < 0) {
            errno = EINVAL;
            return -1;
        }
        f->position = pos;
        f->eof = 0;
        return 0;
    }
    if (lseek(f->fd, off, whence) < 0) {
        f->error = 1;
        return -1;
    }
    f->eof = 0;
    return 0;
}
long ftell(FILE *f)
{
    return f->memory_result ? (long)f->position : lseek(f->fd, 0, SEEK_CUR);
}
int feof(FILE *f)
{
    return f->eof;
}
int ferror(FILE *f)
{
    return f->error;
}
void clearerr(FILE *f)
{
    f->error = f->eof = 0;
}
int fgetc(FILE *f)
{
    unsigned char c;
    return fread(&c, 1, 1, f) == 1 ? c : EOF;
}
int fputc(int c, FILE *f)
{
    unsigned char b = c;
    return fwrite(&b, 1, 1, f) == 1 ? b : EOF;
}
char *fgets(char *s, int size, FILE *f)
{
    if (size <= 0) {
        errno = EINVAL;
        return NULL;
    }
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        s[i++] = c;
        if (c == '\n')
            break;
    }
    s[i] = 0;
    return i ? s : NULL;
}
int fputs(const char *s, FILE *f)
{
    size_t n = strlen(s);
    return fwrite(s, 1, n, f) == n ? 0 : EOF;
}
int puts(const char *s)
{
    if (fputs(s, stdout) == EOF)
        return EOF;
    return fputc('\n', stdout);
}
int putchar(int c)
{
    return fputc(c, stdout);
}
int getchar(void)
{
    return fgetc(stdin);
}
struct output {
    FILE *file;
    char *string;
    size_t capacity, count, used;
    char buffer[512];
    int error;
};
static void emit(struct output *o, char c)
{
    if (o->file) {
        o->buffer[o->used++] = c;
        if (o->used == sizeof(o->buffer)) {
            if (fwrite(o->buffer, 1, o->used, o->file) != o->used)
                o->error = 1;
            o->used = 0;
        }
    } else if (o->count + 1 < o->capacity)
        o->string[o->count] = c;
    o->count++;
}
static int format(struct output *o, const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            emit(o, *fmt++);
            continue;
        }
        fmt++;
        char pad = ' ';
        bool plus = false;
        if (*fmt == '+') {
            plus = true;
            fmt++;
        }
        bool left = false;
        if (*fmt == '-') {
            left = true;
            fmt++;
        }
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        int precision = -1;
        unsigned width = 0;
        if (*fmt == '*') {
            int w = va_arg(ap, int);
            if (w < 0) {
                left = true;
                width = -(long)w;
            } else
                width = w;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            if (width < 10000)
                width = width * 10 + *fmt - '0';
            fmt++;
        }
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
            } else
                while (*fmt >= '0' && *fmt <= '9') {
                    if (precision < 100000)
                        precision = precision * 10 + *fmt - '0';
                    fmt++;
                }
        }
        unsigned l = 0;
        while (*fmt == 'l' || *fmt == 'z') {
            l++;
            fmt++;
        }
        char t = *fmt++;
        char digits[64];
        const char *s = digits;
        size_t len = 0;
        bool neg = false;
        if (t == 's') {
            s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (s[len] && (precision < 0 || len < (size_t)precision))
                len++;
        } else if (t == 'c') {
            digits[0] = va_arg(ap, int);
            len = 1;
        } else if (t == '%') {
            digits[0] = '%';
            len = 1;
        } else {
            uint64_t v;
            unsigned base = t == 'x' || t == 'p' ? 16 : t == 'o' ? 8 : 10;
            if (t == 'd' || t == 'i') {
                int64_t x = l ? va_arg(ap, int64_t) : va_arg(ap, int);
                neg = x < 0;
                v = neg ? 0 - (uint64_t)x : x;
            } else if (t == 'u' || t == 'x' || t == 'o' || t == 'p')
                v = (l || t == 'p') ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            else {
                o->error = 1;
                break;
            }
            char reverse[32];
            unsigned n = 0;
            do {
                reverse[n++] = "0123456789abcdef"[v % base];
                v /= base;
            } while (v);
            if (neg || (plus && (t == 'd' || t == 'i')))
                digits[len++] = neg ? '-' : '+';
            while (n)
                digits[len++] = reverse[--n];
        }
        if (!left && neg && pad == '0') {
            emit(o, '-');
            s++;
            len--;
            if (width)
                width--;
        }
        if (!left)
            while (width > len) {
                emit(o, pad);
                width--;
            }
        for (size_t i = 0; i < len; i++)
            emit(o, s[i]);
        if (left)
            while (width > len) {
                emit(o, ' ');
                width--;
            }
    }
    if (o->file && o->used && fwrite(o->buffer, 1, o->used, o->file) != o->used)
        o->error = 1;
    if (!o->file && o->capacity)
        o->string[o->count < o->capacity ? o->count : o->capacity - 1] = 0;
    return o->error ? -1 : (int)o->count;
}
int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    struct output o = {.file = f};
    return format(&o, fmt, ap);
}
int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}
int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}
int vsnprintf(char *p, size_t n, const char *fmt, va_list ap)
{
    struct output o = {.string = p, .capacity = n};
    return format(&o, fmt, ap);
}
int snprintf(char *p, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(p, n, fmt, ap);
    va_end(ap);
    return r;
}

FILE *fdopen(int fd, const char *mode)
{
    struct kstat s;
    if (fstat(fd, &s) < 0)
        return NULL;
    if (!mode || !strchr("rwa", *mode)) {
        errno = EINVAL;
        return NULL;
    }
    FILE *f = calloc(1, sizeof(*f));
    if (f) {
        f->fd = fd;
        f->owned = 1;
    }
    return f;
}
FILE *open_memstream(char **p, size_t *n)
{
    if (!p || !n) {
        errno = EINVAL;
        return NULL;
    }
    FILE *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->memory = calloc(1, 1);
    if (!f->memory) {
        free(f);
        return NULL;
    }
    f->fd = -1;
    f->owned = 1;
    f->capacity = 1;
    f->memory_result = p;
    f->length_result = n;
    *p = f->memory;
    *n = 0;
    return f;
}
int fileno(FILE *f)
{
    if (f->fd < 0)
        errno = EBADF;
    return f->fd;
}
int vsprintf(char *p, const char *fmt, va_list ap)
{
    return vsnprintf(p, SIZE_MAX, fmt, ap);
}
int sprintf(char *p, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(p, fmt, ap);
    va_end(ap);
    return n;
}
