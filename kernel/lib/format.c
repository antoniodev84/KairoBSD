#include <kernel.h>
static void emit(char *b, size_t n, size_t *p, char c)
{
    if (*p + 1 < n)
        b[*p] = c;
    (*p)++;
}
static int format(char *b, size_t n, const char *f, va_list ap)
{
    size_t p = 0;
    while (*f) {
        if (*f != '%') {
            emit(b, n, &p, *f++);
            continue;
        }
        f++;
        unsigned width = 0;
        char pad = ' ';
        if (*f == '0') {
            pad = '0';
            f++;
        }
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (*f++ - '0');
        unsigned l = 0;
        while (*f == 'l' || *f == 'z') {
            l++;
            f++;
        }
        char t = *f++;
        uint64_t v = 0;
        unsigned base = 10;
        bool neg = false;
        if (t == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (*s)
                emit(b, n, &p, *s++);
            continue;
        }
        if (t == 'c') {
            emit(b, n, &p, va_arg(ap, int));
            continue;
        }
        if (t == '%') {
            emit(b, n, &p, '%');
            continue;
        }
        if (t == 'd' || t == 'i') {
            int64_t x = l ? va_arg(ap, int64_t) : va_arg(ap, int);
            neg = x < 0;
            v = neg ? 0 - (uint64_t)x : (uint64_t)x;
        } else if (t == 'u' || t == 'x' || t == 'p') {
            v = (l || t == 'p') ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            if (t != 'u')
                base = 16;
        } else {
            emit(b, n, &p, '?');
            continue;
        }
        char tmp[32];
        unsigned z = 0;
        do {
            tmp[z++] = "0123456789abcdef"[v % base];
            v /= base;
        } while (v);
        if (neg)
            emit(b, n, &p, '-');
        while (width > z + (unsigned)neg) {
            emit(b, n, &p, pad);
            width--;
        }
        while (z)
            emit(b, n, &p, tmp[--z]);
    }
    if (n)
        b[p < n ? p : n - 1] = 0;
    return (int)p;
}
int snprintf(char *b, size_t n, const char *f, ...)
{
    va_list a;
    va_start(a, f);
    int r = format(b, n, f, a);
    va_end(a);
    return r;
}
void kprintf(const char *f, ...)
{
    char b[1024];
    va_list a;
    va_start(a, f);
    int n = format(b, sizeof(b), f, a);
    va_end(a);
    console_write(b, n < (int)sizeof(b) ? (size_t)n : sizeof(b) - 1);
}
NORETURN void panic(const char *f, ...)
{
    __asm__ volatile("cli");
    apic_stop_others();
    char b[1024];
    va_list a;
    va_start(a, f);
    int n = format(b, sizeof(b), f, a);
    va_end(a);
    console_emergency("\nPANIC: ", 8);
    console_emergency(b, n < (int)sizeof(b) ? (size_t)n : sizeof(b) - 1);
    console_emergency("\n", 1);
    for (;;)
        __asm__ volatile("hlt");
}
