#include <kernel.h>
void *memset(void *p, int c, size_t n)
{
    unsigned char *q = p;
    while (n--)
        *q++ = (unsigned char)c;
    return p;
}
void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n--)
        *a++ = *b++;
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    if (a > b && a < b + n) {
        while (n) {
            n--;
            a[n] = b[n];
        }
    } else
        memcpy(d, s, n);
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y)
            return *x - *y;
        x++;
        y++;
    }
    return 0;
}
size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) {
    }
    return r;
}
