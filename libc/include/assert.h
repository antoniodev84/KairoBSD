#ifndef _ASSERT_H
#define _ASSERT_H
#include <libkario.h>
#define assert(x) ((x) ? (void)0 : __assert_fail(#x, __FILE__, __LINE__))
_Noreturn void __assert_fail(const char *, const char *, int);
#endif
