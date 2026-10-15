#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX(x, y) ((x) < (y) ? (y) : (x))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#ifndef __GNUC__
#define __attribute__(x)
#endif

typedef struct Type Type;
typedef struct Node Node;
typedef struct Member Member;
typedef struct Relocation Relocation;
typedef struct Hideset Hideset;

typedef struct {
    char **data;
    int capacity;
    int len;
} StringArray;

void strarray_push(StringArray *arr, char *s);
char *format(char *fmt, ...) __attribute__((format(printf, 1, 2)));

typedef enum {
    TK_IDENT,
    TK_PUNCT,
    TK_KEYWORD,
    TK_STR,
    TK_NUM,
    TK_PP_NUM,
    TK_EOF,
} TokenKind;

typedef struct {
    char *name;
    int file_no;
    char *contents;

    char *display_name;
    int line_delta;
} File;

typedef struct Token Token;
struct Token {
    TokenKind kind;
    Token *next;
    int64_t val;
    long double fval;
    char *loc;
    int len;
    Type *ty;
    char *str;

    File *file;
    char *filename;
    int line_no;
    int line_delta;
    bool at_bol;
    bool has_space;
    Hideset *hideset;
    Token *origin;
};

noreturn void error(char *fmt, ...) __attribute__((format(printf, 1, 2)));
noreturn void error_at(char *loc, char *fmt, ...) __attribute__((format(printf, 2, 3)));
noreturn void error_tok(Token *tok, char *fmt, ...) __attribute__((format(printf, 2, 3)));
void warn_tok(Token *tok, char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool equal(Token *tok, char *op);
Token *skip(Token *tok, char *op);
bool consume(Token **rest, Token *tok, char *str);
void convert_pp_tokens(Token *tok);
File **get_input_files(void);
File *new_file(char *name, int file_no, char *contents);
Token *tokenize_string_literal(Token *tok, Type *basety);
Token *tokenize(File *file);
Token *tokenize_file(char *filename);

#define unreachable() error("internal error at %s:%d", __FILE__, __LINE__)

char *search_include_paths(char *filename);
void init_macros(void);
void define_macro(char *name, char *buf);
void undef_macro(char *name);
Token *preprocess(Token *tok);

typedef struct Obj Obj;
struct Obj {
    Obj *next;
    char *name;
    Type *ty;
    Token *tok;
    bool is_local;
    int align;

    int offset;

    bool is_function;
    bool is_definition;
    bool is_static;

    bool is_tentative;
    bool is_tls;
    char *init_data;
    Relocation *rel;

    bool is_inline;
    Obj *params;
    Node *body;
    Obj *locals;
    Obj *va_area;
    Obj *alloca_bottom;
    int stack_size;

    bool is_live;
    bool is_root;
    StringArray refs;
};

typedef struct Relocation Relocation;
struct Relocation {
    Relocation *next;
    int offset;
    char **label;
    long addend;
};

typedef enum {
    ND_NULL_EXPR,
    ND_ADD,
    ND_SUB,
    ND_MUL,
    ND_DIV,
    ND_NEG,
    ND_MOD,
    ND_BITAND,
    ND_BITOR,
    ND_BITXOR,
    ND_SHL,
    ND_SHR,
    ND_EQ,
    ND_NE,
    ND_LT,
    ND_LE,
    ND_ASSIGN,
    ND_COND,
    ND_COMMA,
    ND_MEMBER,
    ND_ADDR,
    ND_DEREF,
    ND_NOT,
    ND_BITNOT,
    ND_LOGAND,
    ND_LOGOR,
    ND_RETURN,
    ND_IF,
    ND_FOR,
    ND_DO,
    ND_SWITCH,
    ND_CASE,
    ND_BLOCK,
    ND_GOTO,
    ND_GOTO_EXPR,
    ND_LABEL,
    ND_LABEL_VAL,
    ND_FUNCALL,
    ND_EXPR_STMT,
    ND_STMT_EXPR,
    ND_VAR,
    ND_VLA_PTR,
    ND_NUM,
    ND_CAST,
    ND_MEMZERO,
    ND_ASM,
    ND_CAS,
    ND_EXCH,
} NodeKind;

struct Node {
    NodeKind kind;
    Node *next;
    Type *ty;
    Token *tok;

    Node *lhs;
    Node *rhs;

    Node *cond;
    Node *then;
    Node *els;
    Node *init;
    Node *inc;

    char *brk_label;
    char *cont_label;

    Node *body;

    Member *member;

    Type *func_ty;
    Node *args;
    bool pass_by_stack;
    Obj *ret_buffer;

    char *label;
    char *unique_label;
    Node *goto_next;

    Node *case_next;
    Node *default_case;

    long begin;
    long end;

    char *asm_str;

    Node *cas_addr;
    Node *cas_old;
    Node *cas_new;

    Obj *atomic_addr;
    Node *atomic_expr;

    Obj *var;

    int64_t val;
    long double fval;
};

Node *new_cast(Node *expr, Type *ty);
int64_t const_expr(Token **rest, Token *tok);
Obj *parse(Token *tok);

typedef enum {
    TY_VOID,
    TY_BOOL,
    TY_CHAR,
    TY_SHORT,
    TY_INT,
    TY_LONG,
    TY_FLOAT,
    TY_DOUBLE,
    TY_LDOUBLE,
    TY_ENUM,
    TY_PTR,
    TY_FUNC,
    TY_ARRAY,
    TY_VLA,
    TY_STRUCT,
    TY_UNION,
} TypeKind;

struct Type {
    TypeKind kind;
    int size;
    int align;
    bool is_unsigned;
    bool is_atomic;
    Type *origin;

    Type *base;

    Token *name;
    Token *name_pos;

    int array_len;

    Node *vla_len;
    Obj *vla_size;

    Member *members;
    bool is_flexible;
    bool is_packed;

    Type *return_ty;
    Type *params;
    bool is_variadic;
    Type *next;
};

struct Member {
    Member *next;
    Type *ty;
    Token *tok;
    Token *name;
    int idx;
    int align;
    int offset;

    bool is_bitfield;
    int bit_offset;
    int bit_width;
};

extern Type *ty_void;
extern Type *ty_bool;

extern Type *ty_char;
extern Type *ty_short;
extern Type *ty_int;
extern Type *ty_long;

extern Type *ty_uchar;
extern Type *ty_ushort;
extern Type *ty_uint;
extern Type *ty_ulong;

extern Type *ty_float;
extern Type *ty_double;
extern Type *ty_ldouble;

bool is_integer(Type *ty);
bool is_flonum(Type *ty);
bool is_numeric(Type *ty);
bool is_compatible(Type *t1, Type *t2);
Type *copy_type(Type *ty);
Type *pointer_to(Type *base);
Type *func_type(Type *return_ty);
Type *array_of(Type *base, int size);
Type *vla_of(Type *base, Node *expr);
Type *enum_type(void);
Type *struct_type(void);
void add_type(Node *node);

void codegen(Obj *prog, FILE *out);
int align_to(int n, int align);

int encode_utf8(char *buf, uint32_t c);
uint32_t decode_utf8(char **new_pos, char *p);
bool is_ident1(uint32_t c);
bool is_ident2(uint32_t c);
int display_width(char *p, int len);

typedef struct {
    char *key;
    int keylen;
    void *val;
} HashEntry;

typedef struct {
    HashEntry *buckets;
    int capacity;
    int used;
} HashMap;

void *hashmap_get(HashMap *map, char *key);
void *hashmap_get2(HashMap *map, char *key, int keylen);
void hashmap_put(HashMap *map, char *key, void *val);
void hashmap_put2(HashMap *map, char *key, int keylen, void *val);
void hashmap_delete(HashMap *map, char *key);
void hashmap_delete2(HashMap *map, char *key, int keylen);

bool file_exists(char *path);

extern StringArray include_paths;
extern bool opt_fpic;
extern bool opt_fcommon;
extern char *base_file;
