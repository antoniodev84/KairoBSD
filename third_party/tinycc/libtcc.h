#ifndef LIBTCC_H
#define LIBTCC_H

#ifndef LIBTCCAPI
#define LIBTCCAPI
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void *TCCReallocFunc(void *ptr, unsigned long size);
LIBTCCAPI void tcc_set_realloc(TCCReallocFunc *my_realloc);

typedef struct TCCState TCCState;

LIBTCCAPI TCCState *tcc_new(void);

LIBTCCAPI void tcc_delete(TCCState *s);

LIBTCCAPI void tcc_set_lib_path(TCCState *s, const char *path);

typedef void TCCErrorFunc(void *opaque, const char *msg);
LIBTCCAPI void tcc_set_error_func(TCCState *s, void *error_opaque, TCCErrorFunc *error_func);

LIBTCCAPI int tcc_set_options(TCCState *s, const char *str);

LIBTCCAPI int tcc_add_include_path(TCCState *s, const char *pathname);

LIBTCCAPI int tcc_add_sysinclude_path(TCCState *s, const char *pathname);

LIBTCCAPI void tcc_define_symbol(TCCState *s, const char *sym, const char *value);

LIBTCCAPI void tcc_undefine_symbol(TCCState *s, const char *sym);

LIBTCCAPI int tcc_add_file(TCCState *s, const char *filename);

LIBTCCAPI int tcc_compile_string(TCCState *s, const char *buf);

LIBTCCAPI int tcc_set_output_type(TCCState *s, int output_type);
#define TCC_OUTPUT_MEMORY 1
#define TCC_OUTPUT_EXE 2
#define TCC_OUTPUT_DLL 4
#define TCC_OUTPUT_OBJ 3
#define TCC_OUTPUT_PREPROCESS 5

LIBTCCAPI int tcc_add_library_path(TCCState *s, const char *pathname);

LIBTCCAPI int tcc_add_library(TCCState *s, const char *libraryname);

LIBTCCAPI int tcc_add_symbol(TCCState *s, const char *name, const void *val);

LIBTCCAPI int tcc_output_file(TCCState *s, const char *filename);

LIBTCCAPI int tcc_run(TCCState *s, int argc, char **argv);

LIBTCCAPI int tcc_relocate(TCCState *s1);

LIBTCCAPI void *tcc_get_symbol(TCCState *s, const char *name);

LIBTCCAPI void tcc_list_symbols(TCCState *s, void *ctx,
                                void (*symbol_cb)(void *ctx, const char *name, const void *val));

LIBTCCAPI void *_tcc_setjmp(TCCState *s1, void *jmp_buf, void *top_func, void *longjmp);
#define tcc_setjmp(s1, jb, f) setjmp(_tcc_setjmp(s1, jb, f, longjmp))

typedef int TCCBtFunc(void *udata, void *pc, const char *file, int line, const char *func,
                      const char *msg);
LIBTCCAPI void tcc_set_backtrace_func(TCCState *s1, void *userdata, TCCBtFunc *);

#ifdef __cplusplus
}
#endif

#endif
