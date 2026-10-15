#ifndef _MATH_H
#define _MATH_H
#define HUGE_VAL (__builtin_huge_val())
#define HUGE_VALL (__builtin_huge_vall())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
double ldexp(double, int);
long double ldexpl(long double, int);
#endif
