/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2001-2004 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define USING_GLOBALS
#include "tcc.h"

ST_DATA int rsym, anon_sym, ind, loc;

ST_DATA Sym *global_stack;
ST_DATA Sym *local_stack;
ST_DATA Sym *define_stack;
ST_DATA Sym *global_label_stack;
ST_DATA Sym *local_label_stack;

static Sym *sym_free_first;
static void **sym_pools;
static int nb_sym_pools;

static Sym *all_cleanups, *pending_gotos;
static int local_scope;
ST_DATA char debug_modes;

ST_DATA SValue *vtop;
static SValue _vstack[1 + VSTACK_SIZE];
#define vstack (_vstack + 1)

ST_DATA int nocode_wanted;
#define NODATA_WANTED (nocode_wanted > 0)
#define DATA_ONLY_WANTED 0x80000000

#define CODE_OFF_BIT 0x20000000
#define CODE_OFF()                                                                                 \
    if (!nocode_wanted)                                                                            \
    (nocode_wanted |= CODE_OFF_BIT)
#define CODE_ON() (nocode_wanted &= ~CODE_OFF_BIT)

#define NOEVAL_MASK 0x0000FFFF
#define NOEVAL_WANTED (nocode_wanted & NOEVAL_MASK)

#define CONST_WANTED_BIT 0x00010000
#define CONST_WANTED_MASK 0x0FFF0000
#define CONST_WANTED (nocode_wanted & CONST_WANTED_MASK)

ST_DATA int global_expr;
ST_DATA CType func_vt;
ST_DATA int func_var;
ST_DATA int func_vc;
ST_DATA int func_ind;
static int func_old;
ST_DATA const char *funcname;
ST_DATA CType int_type, func_old_type, char_type, char_pointer_type;
static CString initstr;

#if PTR_SIZE == 4
#define VT_SIZE_T (VT_INT | VT_UNSIGNED)
#define VT_PTRDIFF_T VT_INT
#elif LONG_SIZE == 4
#define VT_SIZE_T (VT_LLONG | VT_UNSIGNED)
#define VT_PTRDIFF_T VT_LLONG
#else
#define VT_SIZE_T (VT_LONG | VT_LLONG | VT_UNSIGNED)
#define VT_PTRDIFF_T (VT_LONG | VT_LLONG)
#endif

static struct switch_t {
    struct case_t {
        int64_t v1, v2;
        int ind, line;
    } **p;
    int n;
    int def_sym;
    int nocode_wanted;
    int *bsym;
    struct scope *scope;
    struct switch_t *prev;
    SValue sv;
} *cur_switch;

#define MAX_TEMP_LOCAL_VARIABLE_NUMBER 8

static struct temp_local_variable {
    int location;
    short size;
    short align;
} arr_temp_local_vars[MAX_TEMP_LOCAL_VARIABLE_NUMBER];
static int nb_temp_local_vars;

static struct scope {
    struct scope *prev;
    struct {
        int loc, locorig, num;
    } vla;
    struct {
        Sym *s;
        int n;
    } cl;
    int *bsym, *csym;
    Sym *lstk, *llstk;
} *cur_scope, *loop_scope, *root_scope;

typedef struct {
    Section *sec;
    int local_offset;
    Sym *flex_array_ref;
} init_params;

#if 1
#define precedence_parser
static void init_prec(void);
#endif

static void block(int flags);
#define STMT_EXPR 1
#define STMT_COMPOUND 2

static void gen_cast(CType *type);
static void gen_cast_s(int t);
static inline CType *pointed_type(CType *type);
static void check_restrict_type(CType *type);
static int type_qualifiers(CType *type);
static void parse_btype_qualify(CType *type, int qualifiers);
static int is_compatible_types(CType *type1, CType *type2);
static int parse_btype(CType *type, AttributeDef *ad, int ignore_label);
static CType *type_decl(CType *type, AttributeDef *ad, int *v, int td);
static void parse_expr_type(CType *type);
static void init_putv(init_params *p, CType *type, unsigned long c);
static void decl_initializer(init_params *p, CType *type, unsigned long c, int flags);
static void decl_initializer_alloc(CType *type, AttributeDef *ad, int r, int has_init, int v,
                                   int scope);
static int decl(int l);
static void expr_eq(void);
static void vpush_type_size(CType *type, int *a);
static int is_compatible_unqualified_types(CType *type1, CType *type2);
static inline int64_t expr_const64(void);
static void vpush64(int ty, unsigned long long v);
static void vpush(CType *type);
static int gvtst(int inv, int t);
static void gen_inline_functions(TCCState *s);
static void free_inline_functions(TCCState *s);
static void skip_or_save_block(TokenString **str);
static void gv_dup(void);
static int get_temp_local_var(int size, int align, int *r2);
static void cast_error(CType *st, CType *dt);
static void end_switch(void);
static void do_Static_assert(void);

ST_FUNC void gsym(int t)
{
    if (t) {
        gsym_addr(t, ind);
        CODE_ON();
    }
}

static int gind()
{
    int t = ind;
    CODE_ON();
    if (debug_modes)
        tcc_tcov_block_begin(tcc_state);
    return t;
}

static void gjmp_addr_acs(int t)
{
    gjmp_addr(t);
    CODE_OFF();
}

static int gjmp_acs(int t)
{
    t = gjmp(t);
    CODE_OFF();
    return t;
}

#define gjmp_addr gjmp_addr_acs
#define gjmp gjmp_acs

ST_INLN int is_float(int t)
{
    int bt = t & VT_BTYPE;
    return bt == VT_LDOUBLE || bt == VT_DOUBLE || bt == VT_FLOAT || bt == VT_QFLOAT;
}

static inline int is_integer_btype(int bt)
{
    return bt == VT_BYTE || bt == VT_BOOL || bt == VT_SHORT || bt == VT_INT || bt == VT_LLONG;
}

static int btype_size(int bt)
{
    return bt == VT_BYTE || bt == VT_BOOL ? 1
           : bt == VT_SHORT               ? 2
           : bt == VT_INT                 ? 4
           : bt == VT_LLONG               ? 8
           : bt == VT_PTR                 ? PTR_SIZE
                                          : 0;
}

static int R_RET(int t)
{
    if (!is_float(t))
        return REG_IRET;
#ifdef TCC_TARGET_X86_64
    if ((t & VT_BTYPE) == VT_LDOUBLE)
        return TREG_ST0;
#elif defined TCC_TARGET_RISCV64
    if ((t & VT_BTYPE) == VT_LDOUBLE)
        return REG_IRET;
#endif
    return REG_FRET;
}

static int R2_RET(int t)
{
    t &= VT_BTYPE;
#if PTR_SIZE == 4
    if (t == VT_LLONG)
        return REG_IRE2;
#elif defined TCC_TARGET_X86_64
    if (t == VT_QLONG)
        return REG_IRE2;
    if (t == VT_QFLOAT)
        return REG_FRE2;
#elif defined TCC_TARGET_RISCV64
    if (t == VT_LDOUBLE)
        return REG_IRE2;
#endif
    return VT_CONST;
}

#define USING_TWO_WORDS(t) (R2_RET(t) != VT_CONST)

static void PUT_R_RET(SValue *sv, int t)
{
    sv->r = R_RET(t), sv->r2 = R2_RET(t);
}

static int RC_RET(int t)
{
    return reg_classes[R_RET(t)] & ~(RC_FLOAT | RC_INT);
}

static int RC_TYPE(int t)
{
    if (!is_float(t))
        return RC_INT;
#ifdef TCC_TARGET_X86_64
    if ((t & VT_BTYPE) == VT_LDOUBLE)
        return RC_ST0;
    if ((t & VT_BTYPE) == VT_QFLOAT)
        return RC_FRET;
#elif defined TCC_TARGET_RISCV64
    if ((t & VT_BTYPE) == VT_LDOUBLE)
        return RC_INT;
#endif
    return RC_FLOAT;
}

static int RC2_TYPE(int t, int rc)
{
    if (!USING_TWO_WORDS(t))
        return 0;
#ifdef RC_IRE2
    if (rc == RC_IRET)
        return RC_IRE2;
#endif
#ifdef RC_FRE2
    if (rc == RC_FRET)
        return RC_FRE2;
#endif
    if (rc & RC_FLOAT)
        return RC_FLOAT;
    return RC_INT;
}

ST_FUNC int ieee_finite(double d)
{
    int p[4];
    memcpy(p, &d, sizeof(double));
    return ((unsigned)((p[1] | 0x800fffff) + 1)) >> 31;
}

ST_FUNC void test_lvalue(void)
{
    if (!(vtop->r & VT_LVAL))
        expect("lvalue");
}

ST_FUNC void check_vstack(void)
{
    if (vtop != vstack - 1)
        tcc_error("internal compiler error: vstack leak (%d)", (int)(vtop - vstack + 1));
}

#if 0
 
void pv (const char *lbl, int a, int b)
{
    int i;
    for (i = a; i < a + b; ++i) {
        SValue *p = &vtop[-i];
        printf("%s vtop[-%d] : type.t:%04x  r:%04x  r2:%04x  c.i:%d\n",
            lbl, i, p->type.t, p->r, p->r2, (int)p->c.i);
    }
}

static inline void psyms(const char *msg, Sym *s, Sym *last)
{
    printf("%-8s scope         v        c        r   type.t\n", msg);
    while (s && s != last) {
        printf("      %8x  %08x %08x %08x %08x %s\n",
            s->sym_scope, s->v, s->c, s->r, s->type.t, get_tok_str(s->v, 0));
        s = s->prev;
    }
}

static void type_to_str(char *buf, int buf_size, CType *type, const char *varstr);

static void ptype(const char *msg, CType *type, int v)
{
    char buf[500];
    type_to_str(buf, sizeof(buf), type,
        (v & ~SYM_FIELD) ? get_tok_str(v, NULL) : NULL);
    printf("%s : %s;\n", msg, buf);
}
#endif

ST_FUNC void tccgen_init(TCCState *s1)
{
    vtop = vstack - 1;
    memset(vtop, 0, sizeof *vtop);

    int_type.t = VT_INT;

    char_type.t = VT_BYTE;
    if (s1->char_is_unsigned)
        char_type.t |= VT_UNSIGNED;
    char_pointer_type = char_type;
    mk_pointer(&char_pointer_type);

    func_old_type.t = VT_FUNC;
    func_old_type.ref = sym_push(SYM_FIELD, &int_type, 0, 0);
    func_old_type.ref->f.func_call = FUNC_CDECL;
    func_old_type.ref->f.func_type = FUNC_OLD;
#ifdef precedence_parser
    init_prec();
#endif
    cstr_new(&initstr);
}

ST_FUNC int tccgen_compile(TCCState *s1)
{
    funcname = "";
    func_ind = -1;
    anon_sym = SYM_FIRST_ANOM;
    nocode_wanted = DATA_ONLY_WANTED;
    debug_modes = (s1->do_debug ? 1 : 0) | s1->test_coverage << 1;
    global_expr = 0;

    tcc_debug_start(s1);
    tcc_tcov_start(s1);
#ifdef TCC_TARGET_ARM
    arm_init(s1);
#endif
#ifdef INC_DEBUG
    printf("%s: **** new file\n", file->filename);
#endif
    parse_flags = PARSE_FLAG_PREPROCESS | PARSE_FLAG_TOK_NUM | PARSE_FLAG_TOK_STR;
    next();
    decl(VT_CONST);
    gen_inline_functions(s1);
    check_vstack();

#if TCC_EH_FRAME
    tcc_eh_frame_end(s1);
#endif
    tcc_debug_end(s1);
    tcc_tcov_end(s1);
    return 0;
}

ST_FUNC void tccgen_finish(TCCState *s1)
{
    tcc_debug_end(s1);
    free_inline_functions(s1);
    sym_pop(&global_stack, NULL, 0);
    sym_pop(&local_stack, NULL, 0);

    free_defines(NULL);

    dynarray_reset(&sym_pools, &nb_sym_pools);
    cstr_free(&initstr);
    dynarray_reset(&stk_data, &nb_stk_data);
    while (cur_switch)
        end_switch();
    local_scope = 0;
    loop_scope = NULL;
    all_cleanups = NULL;
    pending_gotos = NULL;
    nb_temp_local_vars = 0;
    global_label_stack = NULL;
    local_label_stack = NULL;
    cur_text_section = NULL;
    sym_free_first = NULL;
}

ST_FUNC ElfSym *elfsym(Sym *s)
{
    if (!s || !s->c)
        return NULL;
    return &((ElfSym *)symtab_section->data)[s->c];
}

ST_FUNC void update_storage(Sym *sym)
{
    ElfSym *esym;
    int sym_bind, old_sym_bind;

    esym = elfsym(sym);
    if (!esym)
        return;

    if (sym->a.visibility)
        esym->st_other = (esym->st_other & ~ELFW(ST_VISIBILITY)(-1)) | sym->a.visibility;

    if (sym->type.t & (VT_STATIC | VT_INLINE))
        sym_bind = STB_LOCAL;
    else if (sym->a.weak)
        sym_bind = STB_WEAK;
    else
        sym_bind = STB_GLOBAL;
    old_sym_bind = ELFW(ST_BIND)(esym->st_info);
    if (sym_bind != old_sym_bind) {
        esym->st_info = ELFW(ST_INFO)(sym_bind, ELFW(ST_TYPE)(esym->st_info));
    }

#ifdef TCC_TARGET_PE
    if (sym->a.dllimport)
        esym->st_other |= ST_PE_IMPORT;
    if (sym->a.dllexport)
        esym->st_other |= ST_PE_EXPORT;
#endif

#if 0
    printf("storage %s: bind=%c vis=%d exp=%d imp=%d\n",
        get_tok_str(sym->v, NULL),
        sym_bind == STB_WEAK ? 'w' : sym_bind == STB_LOCAL ? 'l' : 'g',
        sym->a.visibility,
        sym->a.dllexport,
        sym->a.dllimport
        );
#endif
}

ST_FUNC void put_extern_sym2(Sym *sym, int sh_num, addr_t value, unsigned long size,
                             int can_add_underscore)
{
    int sym_type, sym_bind, info, other, t;
    ElfSym *esym;
    const char *name;
    char buf1[256];

    if (!sym->c) {
        name = get_tok_str(sym->v, NULL);
        t = sym->type.t;
        if ((t & VT_BTYPE) == VT_FUNC) {
            sym_type = STT_FUNC;
        } else if ((t & VT_BTYPE) == VT_VOID) {
            sym_type = STT_NOTYPE;
            if (IS_ASM_FUNC(t))
                sym_type = STT_FUNC;
        } else if (t & VT_TLS) {
            sym_type = STT_TLS;
        } else {
            sym_type = STT_OBJECT;
        }
        if (t & (VT_STATIC | VT_INLINE))
            sym_bind = STB_LOCAL;
        else
            sym_bind = STB_GLOBAL;
        other = 0;

#ifdef TCC_TARGET_PE
        if (sym_type == STT_FUNC && sym->type.ref) {
            Sym *ref = sym->type.ref;
            if (ref->a.nodecorate) {
                can_add_underscore = 0;
            }
            if (ref->f.func_call == FUNC_STDCALL && can_add_underscore) {
                sprintf(buf1, "_%s@%d", name, ref->f.func_args * PTR_SIZE);
                name = buf1;
                other |= ST_PE_STDCALL;
                can_add_underscore = 0;
            }
        }
#endif

        if (sym->asm_label) {
            name = get_tok_str(sym->asm_label, NULL);
            can_add_underscore = 0;
        }

        if (tcc_state->leading_underscore && can_add_underscore) {
            buf1[0] = '_';
            pstrcpy(buf1 + 1, sizeof(buf1) - 1, name);
            name = buf1;
        }

        info = ELFW(ST_INFO)(sym_bind, sym_type);
        sym->c = put_elf_sym(symtab_section, value, size, info, other, sh_num, name);

        if (debug_modes)
            tcc_debug_extern_sym(tcc_state, sym, sh_num, sym_bind, sym_type);

    } else {
        esym = elfsym(sym);
        esym->st_value = value;
        esym->st_size = size;
        esym->st_shndx = sh_num;
    }
    update_storage(sym);
}

ST_FUNC void put_extern_sym(Sym *sym, Section *s, addr_t value, unsigned long size)
{
    if (nocode_wanted && (NODATA_WANTED || (s && s == cur_text_section)))
        return;
    put_extern_sym2(sym, s ? s->sh_num : SHN_UNDEF, value, size, 1);
}

ST_FUNC void greloca(Section *s, Sym *sym, unsigned long offset, int type, addr_t addend)
{
    int c = 0;

    if (nocode_wanted && s == cur_text_section)
        return;

    if (sym) {
        if (0 == sym->c) {
            put_extern_sym(sym, NULL, 0, 0);
            if (sym->sym_scope &&
                (sym->type.t & (VT_STATIC | VT_EXTERN)) == (VT_STATIC | VT_EXTERN)) {

                Sym *s = sym;
                while (s->prev_tok)
                    s = s->prev_tok;
                s->c = sym->c;
            }
        }
        c = sym->c;
    }

    put_elf_reloca(symtab_section, s, offset, type, c, addend);
}

#if PTR_SIZE == 4
ST_FUNC void greloc(Section *s, Sym *sym, unsigned long offset, int type)
{
    greloca(s, sym, offset, type, 0);
}
#endif

static Sym *__sym_malloc(void)
{
    Sym *sym_pool, *sym, *last_sym;
    int i;

    sym_pool = tcc_malloc(SYM_POOL_NB * sizeof(Sym));
    dynarray_add(&sym_pools, &nb_sym_pools, sym_pool);

    last_sym = sym_free_first;
    sym = sym_pool;
    for (i = 0; i < SYM_POOL_NB; i++) {
        sym->next = last_sym;
        last_sym = sym;
        sym++;
    }
    sym_free_first = last_sym;
    return last_sym;
}

static inline Sym *sym_malloc(void)
{
    Sym *sym;
#ifndef SYM_DEBUG
    sym = sym_free_first;
    if (!sym)
        sym = __sym_malloc();
    sym_free_first = sym->next;
    return sym;
#else
    sym = tcc_malloc(sizeof(Sym));
    return sym;
#endif
}

ST_INLN void sym_free(Sym *sym)
{
#ifndef SYM_DEBUG
    sym->next = sym_free_first;
    sym_free_first = sym;
#else
    tcc_free(sym);
#endif
}

ST_FUNC Sym *sym_push2(Sym **ps, int v, int t, int c)
{
    Sym *s;

    s = sym_malloc();
    memset(s, 0, sizeof *s);
    s->v = v;
    s->type.t = t;
    s->c = c;

    s->prev = *ps;
    *ps = s;
    return s;
}

ST_FUNC Sym *sym_find2(Sym *s, int v)
{
    while (s) {
        if (s->v == v)
            return s;
        s = s->prev;
    }
    return NULL;
}

ST_INLN Sym *struct_find(int v)
{
    v -= TOK_IDENT;
    if ((unsigned)v >= (unsigned)(tok_ident - TOK_IDENT))
        return NULL;
    return table_ident[v]->sym_struct;
}

ST_INLN Sym *sym_find(int v)
{
    v -= TOK_IDENT;
    if ((unsigned)v >= (unsigned)(tok_ident - TOK_IDENT))
        return NULL;
    return table_ident[v]->sym_identifier;
}

static inline void sym_link(Sym *s, int yes)
{
    TokenSym *ts = table_ident[(s->v & ~SYM_STRUCT) - TOK_IDENT];
    Sym **ps;
    if (s->v & SYM_STRUCT)
        ps = &ts->sym_struct;
    else
        ps = &ts->sym_identifier;
    if (yes) {
        s->prev_tok = *ps, *ps = s;
        s->sym_scope = local_scope;
    } else {
        *ps = s->prev_tok;
    }
}

static inline int sym_scope_ex(Sym *s)
{

    return IS_ENUM_VAL(s->type.t) ? s->type.ref->sym_scope : s->sym_scope;
}

ST_FUNC Sym *sym_push(int v, CType *type, int r, int c)
{
    Sym *s, **ps;
    if (local_stack)
        ps = &local_stack;
    else
        ps = &global_stack;
    s = sym_push2(ps, v, type->t, c);
    s->type.ref = type->ref;
    s->r = r;

    if ((v & ~SYM_STRUCT) < SYM_FIRST_ANOM) {

        sym_link(s, 1);
        if (s->prev_tok && sym_scope_ex(s->prev_tok) == local_scope)
            tcc_error("redeclaration of '%s'", get_tok_str(s->v, NULL));
    }
    return s;
}

ST_FUNC Sym *global_identifier_push(int v, int t, int c)
{
    Sym *s, **ps;
    s = sym_push2(&global_stack, v, t, c);
    s->r = VT_CONST | VT_SYM;

    if (v < SYM_FIRST_ANOM) {
        ps = &table_ident[v - TOK_IDENT]->sym_identifier;

        while (*ps != NULL && (*ps)->sym_scope)
            ps = &(*ps)->prev_tok;
        s->prev_tok = *ps;
        *ps = s;
    }
    return s;
}

ST_FUNC void sym_pop(Sym **ptop, Sym *b, int keep)
{
    Sym *s, *ss;
    int v;

    s = *ptop;
    while (s != b) {
        ss = s->prev;
        v = s->v;

        if ((v & ~SYM_STRUCT) < SYM_FIRST_ANOM)
            sym_link(s, 0);
        if (!keep)
            sym_free(s);
        s = ss;
    }
    if (!keep)
        *ptop = b;
}

ST_FUNC Sym *label_find(int v)
{
    v -= TOK_IDENT;
    if ((unsigned)v >= (unsigned)(tok_ident - TOK_IDENT))
        return NULL;
    return table_ident[v]->sym_label;
}

ST_FUNC Sym *label_push(Sym **ptop, int v, int flags)
{
    Sym *s, **ps;
    s = sym_push2(ptop, v, VT_STATIC, 0);
    s->r = flags;
    ps = &table_ident[v - TOK_IDENT]->sym_label;
    if (ptop == &global_label_stack) {

        while (*ps != NULL)
            ps = &(*ps)->prev_tok;
    }
    s->prev_tok = *ps;
    *ps = s;
    return s;
}

ST_FUNC void label_pop(Sym **ptop, Sym *slast, int keep)
{
    Sym *s, *s1;
    for (s = *ptop; s != slast; s = s1) {
        s1 = s->prev;
        if (s->r == LABEL_DECLARED) {
            tcc_warning_c(warn_all)("label '%s' declared but not used", get_tok_str(s->v, NULL));
        } else if (s->r == LABEL_FORWARD) {
            tcc_error("label '%s' used but not defined", get_tok_str(s->v, NULL));
        } else {
            if (s->c) {

                put_extern_sym(s, cur_text_section, s->jind, 1);
            }
        }

        if (s->r != LABEL_GONE)
            table_ident[s->v - TOK_IDENT]->sym_label = s->prev_tok;
        if (!keep)
            sym_free(s);
        else
            s->r = LABEL_GONE;
    }
    if (!keep)
        *ptop = slast;
}

static void vcheck_cmp(void)
{

    if (vtop->r == VT_CMP && 0 == (nocode_wanted & ~CODE_OFF_BIT))
        gv(RC_INT);
}

static void vsetc(CType *type, int r, CValue *vc)
{
    if (vtop >= vstack + (VSTACK_SIZE - 1))
        tcc_error("memory full (vstack)");
    vcheck_cmp();
    vtop++;
    vtop->type = *type;
    vtop->r = r;
    vtop->r2 = VT_CONST;
    vtop->c = *vc;
    vtop->sym = NULL;
}

ST_FUNC void vswap(void)
{
    SValue tmp;

    vcheck_cmp();
    tmp = vtop[0];
    vtop[0] = vtop[-1];
    vtop[-1] = tmp;
}

ST_FUNC void vpop(void)
{
    int v;
    v = vtop->r & VT_VALMASK;
#if defined(TCC_TARGET_I386) || defined(TCC_TARGET_X86_64)

    if (v == TREG_ST0) {
        o(0xd8dd);
    } else
#endif
        if (v == VT_CMP) {

        gsym(vtop->jtrue);
        gsym(vtop->jfalse);
    }
    vtop--;
}

static void vpush(CType *type)
{
    vset(type, VT_CONST, 0);
}

static void vpush64(int ty, unsigned long long v)
{
    CValue cval;
    CType ctype;
    ctype.t = ty;
    ctype.ref = NULL;
    cval.i = v;
    vsetc(&ctype, VT_CONST, &cval);
}

ST_FUNC void vpushi(int v)
{
    vpush64(VT_INT, v);
}

static void vpushs(addr_t v)
{
    vpush64(VT_SIZE_T, v);
}

static inline void vpushll(long long v)
{
    vpush64(VT_LLONG, v);
}

ST_FUNC void vset(CType *type, int r, int v)
{
    CValue cval;
    cval.i = v;
    vsetc(type, r, &cval);
}

static void vseti(int r, int v)
{
    CType type;
    type.t = VT_INT;
    type.ref = NULL;
    vset(&type, r, v);
}

ST_FUNC void vpushv(SValue *v)
{
    if (vtop >= vstack + (VSTACK_SIZE - 1))
        tcc_error("memory full (vstack)");
    vtop++;
    *vtop = *v;
}

static void vdup(void)
{
    vpushv(vtop);
}

ST_FUNC void vrotb(int n)
{
    SValue tmp;
    if (--n < 1)
        return;
    vcheck_cmp();
    tmp = vtop[-n];
    memmove(vtop - n, vtop - n + 1, sizeof *vtop * n);
    vtop[0] = tmp;
}

ST_FUNC void vrott(int n)
{
    SValue tmp;
    if (--n < 1)
        return;
    vcheck_cmp();
    tmp = vtop[0];
    memmove(vtop - n + 1, vtop - n, sizeof *vtop * n);
    vtop[-n] = tmp;
}

ST_FUNC void vrev(int n)
{
    int i;
    SValue tmp;
    vcheck_cmp();
    for (i = 0, n = -n; i > ++n; --i)
        tmp = vtop[i], vtop[i] = vtop[n], vtop[n] = tmp;
}

ST_FUNC void vset_VT_CMP(int op)
{
    vtop->r = VT_CMP;
    vtop->cmp_op = op;
    vtop->jfalse = 0;
    vtop->jtrue = 0;
}

static void vset_VT_JMP(void)
{
    int op = vtop->cmp_op;

    if (vtop->jtrue || vtop->jfalse) {
        int origt = vtop->type.t;

        int inv = op & (op < 2);
        vseti(VT_JMP + inv, gvtst(inv, 0));
        vtop->type.t |= origt & (VT_UNSIGNED | VT_DEFSIGN);
    } else {

        vtop->c.i = op;
        if (op < 2)
            vtop->r = VT_CONST;
    }
}

static void gvtst_set(int inv, int t)
{
    int *p;

    if (vtop->r != VT_CMP) {
        vpushi(0);
        gen_op(TOK_NE);
        if (vtop->r != VT_CMP)
            vset_VT_CMP(vtop->c.i != 0);
    }

    p = inv ? &vtop->jfalse : &vtop->jtrue;
    *p = gjmp_append(*p, t);
}

static int gvtst(int inv, int t)
{
    int op, x, u;

    gvtst_set(inv, t);
    t = vtop->jtrue, u = vtop->jfalse;
    if (inv)
        x = u, u = t, t = x;
    op = vtop->cmp_op;

    if (op > 1)
        t = gjmp_cond(op ^ inv, t);
    else if (op != inv)
        t = gjmp(t);

    gsym(u);

    vtop--;
    return t;
}

static void gen_test_zero(int op)
{
    if (vtop->r == VT_CMP) {
        int j;
        if (op == TOK_EQ) {
            j = vtop->jfalse;
            vtop->jfalse = vtop->jtrue;
            vtop->jtrue = j;
            vtop->cmp_op ^= 1;
        }
    } else {
        vpushi(0);
        gen_op(op);
    }
}

ST_FUNC void vpushsym(CType *type, Sym *sym)
{
    CValue cval;
    cval.i = 0;
    vsetc(type, VT_CONST | VT_SYM, &cval);
    vtop->sym = sym;
}

ST_FUNC Sym *get_sym_ref(CType *type, Section *sec, unsigned long offset, unsigned long size)
{
    int v;
    Sym *sym;

    v = anon_sym++;
    sym = sym_push(v, type, VT_CONST | VT_SYM, 0);
    sym->type.t |= VT_STATIC;
    put_extern_sym(sym, sec, offset, size);
    return sym;
}

static void vpush_ref(CType *type, Section *sec, unsigned long offset, unsigned long size)
{
    vpushsym(type, get_sym_ref(type, sec, offset, size));
}

ST_FUNC Sym *external_global_sym(int v, CType *type)
{
    Sym *s;

    s = sym_find(v);
    if (!s) {

        s = global_identifier_push(v, type->t | VT_EXTERN, 0);
        s->type.ref = type->ref;
    } else if (IS_ASM_SYM(s)) {
        s->type.t = type->t | (s->type.t & VT_EXTERN);
        s->type.ref = type->ref;
        update_storage(s);
    }
    return s;
}

ST_FUNC Sym *external_helper_sym(int v)
{
    CType ct = {VT_ASM_FUNC, NULL};
    return external_global_sym(v, &ct);
}

ST_FUNC void vpush_helper_func(int v)
{
    vpushsym(&func_old_type, external_helper_sym(v));
}

static void merge_symattr(struct SymAttr *sa, struct SymAttr *sa1)
{
    if (sa1->aligned && !sa->aligned)
        sa->aligned = sa1->aligned;
    sa->packed |= sa1->packed;
    sa->weak |= sa1->weak;
    sa->nodebug |= sa1->nodebug;
    if (sa1->visibility != STV_DEFAULT) {
        int vis = sa->visibility;
        if (vis == STV_DEFAULT || vis > sa1->visibility)
            vis = sa1->visibility;
        sa->visibility = vis;
    }
    sa->dllexport |= sa1->dllexport;
    sa->nodecorate |= sa1->nodecorate;
    sa->dllimport |= sa1->dllimport;
}

static void merge_funcattr(struct FuncAttr *fa, struct FuncAttr *fa1)
{
    if (fa1->func_call && !fa->func_call)
        fa->func_call = fa1->func_call;
    if (fa1->func_type && !fa->func_type)
        fa->func_type = fa1->func_type;
    if (fa1->func_args && !fa->func_args)
        fa->func_args = fa1->func_args;
    if (fa1->func_noreturn)
        fa->func_noreturn = 1;
    if (fa1->func_ctor)
        fa->func_ctor = 1;
    if (fa1->func_dtor)
        fa->func_dtor = 1;
}

static void merge_attr(AttributeDef *ad, AttributeDef *ad1)
{
    merge_symattr(&ad->a, &ad1->a);
    merge_funcattr(&ad->f, &ad1->f);

    if (ad1->section)
        ad->section = ad1->section;
    if (ad1->alias_target)
        ad->alias_target = ad1->alias_target;
    if (ad1->asm_label)
        ad->asm_label = ad1->asm_label;
    if (ad1->attr_mode)
        ad->attr_mode = ad1->attr_mode;
}

static void patch_type(Sym *sym, CType *type)
{
    if (!(type->t & VT_EXTERN) || IS_ENUM_VAL(sym->type.t)) {
        if (!(sym->type.t & VT_EXTERN))
            tcc_error("redefinition of '%s'", get_tok_str(sym->v, NULL));
        sym->type.t &= ~VT_EXTERN;
    }

    if (IS_ASM_SYM(sym)) {

        sym->type.t = type->t & (sym->type.t | ~VT_STATIC);
        sym->type.ref = type->ref;
        if ((type->t & VT_BTYPE) != VT_FUNC && !(type->t & VT_ARRAY))
            sym->r |= VT_LVAL;
    }

    if (!is_compatible_types(&sym->type, type)) {
        tcc_error("incompatible types for redefinition of '%s'", get_tok_str(sym->v, NULL));

    } else if ((sym->type.t & VT_BTYPE) == VT_FUNC) {
        int static_proto = sym->type.t & VT_STATIC;
        int ft1 = sym->type.ref->f.func_type;
        int ft2 = type->ref->f.func_type;

        if ((type->t & VT_STATIC) && !static_proto

            && !((type->t | sym->type.t) & VT_INLINE))
            tcc_warning("static storage ignored for redefinition of '%s'",
                        get_tok_str(sym->v, NULL));

        if ((type->t | sym->type.t) & VT_INLINE) {
            if (!((type->t ^ sym->type.t) & VT_INLINE) || ((type->t | sym->type.t) & VT_STATIC))
                static_proto |= VT_INLINE;
        }

        if (0 == (type->t & VT_EXTERN)) {
            struct FuncAttr f = sym->type.ref->f;

            sym->type.t = (type->t & ~(VT_STATIC | VT_INLINE)) | static_proto;
            if (ft1 != FUNC_OLD)
                type->ref->f.func_type = ft1;
            sym->type.ref = type->ref;
            merge_funcattr(&sym->type.ref->f, &f);
        } else {
            sym->type.t &= ~VT_INLINE | static_proto;
            if (ft1 == FUNC_OLD && ft2 != FUNC_OLD)
                sym->type.ref = type->ref;
        }

    } else {
        if ((sym->type.t & VT_ARRAY) && type->ref->c >= 0) {

            sym->type.ref->c = type->ref->c;
        }
        if ((type->t ^ sym->type.t) & VT_STATIC)
            tcc_warning("storage mismatch for redefinition of '%s'", get_tok_str(sym->v, NULL));
    }
}

static void patch_storage(Sym *sym, AttributeDef *ad, CType *type)
{
    if (type)
        patch_type(sym, type);

#ifdef TCC_TARGET_PE
    if (sym->a.dllimport != ad->a.dllimport)
        tcc_error("incompatible dll linkage for redefinition of '%s'", get_tok_str(sym->v, NULL));
#endif
    merge_symattr(&sym->a, &ad->a);
    if (ad->asm_label)
        sym->asm_label = ad->asm_label;
    update_storage(sym);
}

static Sym *sym_copy(Sym *s0, Sym **ps)
{
    Sym *s;
    s = sym_malloc(), *s = *s0;
    s->prev = *ps, *ps = s;
    if ((s->v & ~SYM_STRUCT) < SYM_FIRST_ANOM)
        sym_link(s, 1);
    return s;
}

static void move_ref_to_global(Sym *s)
{
    Sym *l, **lp;
    int n, bt;

    bt = s->type.t & VT_BTYPE;
    if (!(bt == VT_PTR || bt == VT_FUNC || bt == VT_STRUCT || IS_ENUM(s->type.t)))
        return;

    for (s = s->type.ref, n = 0; s; s = s->next) {
        for (lp = &local_stack; !!(l = *lp); lp = &l->prev) {
            if (l == s) {
                *lp = s->prev;
                s->prev = global_stack, global_stack = s;
                if (n || bt == VT_PTR || bt == VT_FUNC) {
                    move_ref_to_global(s);
                } else {
                    if ((s->v & ~SYM_STRUCT) < SYM_FIRST_ANOM) {

                        s->v |= SYM_FIELD;
                        l = sym_copy(s, lp);
                        l->v &= ~SYM_FIELD;
                    }
                }
                if (bt != VT_PTR)
                    n = 1;
                break;
            }
        }
        if (n == 0)
            break;
    }
}

static Sym *external_sym(int v, CType *type, int r, AttributeDef *ad)
{
    Sym *s;

    s = sym_find(v);
    while (s && s->sym_scope)
        s = s->prev_tok;

    if (!s) {

        s = global_identifier_push(v, type->t, 0);
        s->r |= r;
        s->a = ad->a;
        s->asm_label = ad->asm_label;
        s->type.ref = type->ref;
    } else {
        patch_storage(s, ad, type);
    }
    if (local_stack) {

        move_ref_to_global(s);

        sym_copy(s, &local_stack);
    }
    return s;
}

ST_FUNC void save_regs(int n)
{
    SValue *p, *p1;
    for (p = vstack, p1 = vtop - n; p <= p1; p++)
        save_reg(p->r);
}

ST_FUNC void save_reg(int r)
{
    save_reg_upstack(r, 0);
}

ST_FUNC void save_reg_upstack(int r, int n)
{
    int l, size, align, bt, r2;
    SValue *p, *p1, sv;

    if ((r &= VT_VALMASK) >= VT_CONST)
        return;
    if (nocode_wanted)
        return;
    l = r2 = 0;
    for (p = vstack, p1 = vtop - n; p <= p1; p++) {
        if ((p->r & VT_VALMASK) == r || p->r2 == r) {

            if (!l) {
                bt = p->type.t & VT_BTYPE;
                if (bt == VT_VOID)
                    continue;
                if ((p->r & VT_LVAL) || bt == VT_FUNC)
                    bt = VT_PTR;
                sv.type.t = bt;
                size = type_size(&sv.type, &align);
                l = get_temp_local_var(size, align, &r2);
                sv.r = VT_LOCAL | VT_LVAL;
                sv.c.i = l;
                sv.sym = NULL;
                store(p->r & VT_VALMASK, &sv);
#if defined(TCC_TARGET_I386) || defined(TCC_TARGET_X86_64)

                if (r == TREG_ST0) {
                    o(0xd8dd);
                }
#endif

                if (p->r2 < VT_CONST && USING_TWO_WORDS(bt)) {
                    sv.c.i += PTR_SIZE;
                    store(p->r2, &sv);
                }
            }

            if (p->r & VT_LVAL) {

                p->r = (p->r & ~(VT_VALMASK | VT_BOUNDED)) | VT_LLOCAL;
            } else {
                p->r = VT_LVAL | VT_LOCAL;
                p->type.t &= ~VT_ARRAY;
            }
            p->sym = NULL;
            p->r2 = r2;
            p->c.i = l;
        }
    }
}

#ifdef TCC_TARGET_ARM

ST_FUNC int get_reg_ex(int rc, int rc2)
{
    int r;
    SValue *p;

    for (r = 0; r < NB_REGS; r++) {
        if (reg_classes[r] & rc2) {
            int n;
            n = 0;
            for (p = vstack; p <= vtop; p++) {
                if ((p->r & VT_VALMASK) == r || p->r2 == r)
                    n++;
            }
            if (n <= 1)
                return r;
        }
    }
    return get_reg(rc);
}
#endif

ST_FUNC int get_reg(int rc)
{
    int r;
    SValue *p;

    for (r = 0; r < NB_REGS; r++) {
        if (reg_classes[r] & rc) {
            if (nocode_wanted)
                return r;
            for (p = vstack; p <= vtop; p++) {
                if ((p->r & VT_VALMASK) == r || p->r2 == r)
                    goto notfound;
            }
            return r;
        }
    notfound:;
    }

    for (p = vstack; p <= vtop; p++) {

        r = p->r2;
        if (r < VT_CONST && (reg_classes[r] & rc))
            goto save_found;
        r = p->r & VT_VALMASK;
        if (r < VT_CONST && (reg_classes[r] & rc)) {
        save_found:
            save_reg(r);
            return r;
        }
    }

    return -1;
}

static int get_temp_local_var(int size, int align, int *r2)
{
    int i;
    struct temp_local_variable *temp_var;
    SValue *p;
    int r;
    unsigned used = 0;

    for (p = vstack; p <= vtop; p++) {
        r = p->r & VT_VALMASK;
        if (r == VT_LOCAL || r == VT_LLOCAL) {
            r = p->r2 - (VT_CONST + 1);
            if (r >= 0 && r < MAX_TEMP_LOCAL_VARIABLE_NUMBER)
                used |= 1 << r;
        }
    }
    for (i = 0; i < nb_temp_local_vars; i++) {
        temp_var = &arr_temp_local_vars[i];
        if (!(used & 1 << i) && temp_var->size >= size && temp_var->align >= align) {
        ret_tmp:
            *r2 = (VT_CONST + 1) + i;
            return temp_var->location;
        }
    }
    loc = (loc - size) & -align;
    if (nb_temp_local_vars < MAX_TEMP_LOCAL_VARIABLE_NUMBER) {
        temp_var = &arr_temp_local_vars[i];
        temp_var->location = loc;
        temp_var->size = size;
        temp_var->align = align;
        nb_temp_local_vars++;
        goto ret_tmp;
    }
    *r2 = VT_CONST;
    return loc;
}

static void move_reg(int r, int s, int t)
{
    SValue sv;

    if (r != s) {
        save_reg(r);
        sv.type.t = t;
        sv.type.ref = NULL;
        sv.r = s;
        sv.c.i = 0;
        load(r, &sv);
    }
}

ST_FUNC void gaddrof(void)
{
    vtop->r &= ~VT_LVAL;

    if ((vtop->r & VT_VALMASK) == VT_LLOCAL)
        vtop->r = (vtop->r & ~VT_VALMASK) | VT_LOCAL | VT_LVAL;
}

#ifdef CONFIG_TCC_BCHECK

static void gen_bounded_ptr_add(void)
{
    int save = (vtop[-1].r & VT_VALMASK) == VT_LOCAL;
    if (save) {
        vpushv(&vtop[-1]);
        vrott(3);
    }
    vpush_helper_func(TOK___bound_ptr_add);
    vrott(3);
    gfunc_call(2);
    vtop -= save;
    vpushi(0);

    vtop->r = REG_IRET | VT_BOUNDED;
    if (nocode_wanted)
        return;

    vtop->c.i = (cur_text_section->reloc->data_offset - sizeof(ElfW_Rel));
}

static void gen_bounded_ptr_deref(void)
{
    addr_t func;
    int size, align;
    ElfW_Rel *rel;
    Sym *sym;

    if (nocode_wanted)
        return;

    size = type_size(&vtop->type, &align);
    switch (size) {
        case 1:
            func = TOK___bound_ptr_indir1;
            break;
        case 2:
            func = TOK___bound_ptr_indir2;
            break;
        case 4:
            func = TOK___bound_ptr_indir4;
            break;
        case 8:
            func = TOK___bound_ptr_indir8;
            break;
        case 12:
            func = TOK___bound_ptr_indir12;
            break;
        case 16:
            func = TOK___bound_ptr_indir16;
            break;
        default:

            return;
    }
    sym = external_helper_sym(func);
    if (!sym->c)
        put_extern_sym(sym, NULL, 0, 0);

    rel = (ElfW_Rel *)(cur_text_section->reloc->data + vtop->c.i);
    rel->r_info = ELFW(R_INFO)(sym->c, ELFW(R_TYPE)(rel->r_info));
}

static void gbound(void)
{
    CType type1;

    vtop->r &= ~VT_MUSTBOUND;

    if (vtop->r & VT_LVAL) {

        if (!(vtop->r & VT_BOUNDED)) {

            type1 = vtop->type;
            vtop->type.t = VT_PTR;
            gaddrof();
            vpushi(0);
            gen_bounded_ptr_add();
            vtop->r |= VT_LVAL;
            vtop->type = type1;
        }

        gen_bounded_ptr_deref();
    }
}

ST_FUNC void gbound_args(int nb_args)
{
    int i, v;
    SValue *sv;

    for (i = 1; i <= nb_args; ++i)
        if (vtop[1 - i].r & VT_MUSTBOUND) {
            vrotb(i);
            gbound();
            vrott(i);
        }

    sv = vtop - nb_args;
    if (sv->r & VT_SYM) {
        v = sv->sym->v;
        if (v == TOK_setjmp || v == TOK__setjmp
#ifndef TCC_TARGET_PE
            || v == TOK_sigsetjmp || v == TOK___sigsetjmp
#endif
        ) {
            vpush_helper_func(TOK___bound_setjmp);
            vpushv(sv + 1);
            gfunc_call(1);
            func_bound_add_epilog = 1;
        }
        if (v == TOK_alloca)
            func_bound_add_epilog = 1;
#if TARGETOS_NetBSD
        if (v == TOK_longjmp)
            sv->sym->asm_label = TOK___bound_longjmp;
#endif
    }
}

static void add_local_bounds(Sym *s, Sym *e)
{
    for (; s != e; s = s->prev) {
        if (!s->v || (s->r & VT_VALMASK) != VT_LOCAL)
            continue;

        if ((s->type.t & VT_ARRAY) || (s->type.t & VT_BTYPE) == VT_STRUCT || s->a.addrtaken) {

            int align, size = type_size(&s->type, &align);
            addr_t *bounds_ptr = section_ptr_add(lbounds_section, 2 * sizeof(addr_t));
            bounds_ptr[0] = s->c;
            bounds_ptr[1] = size;
        }
    }
}
#endif

static void tcc_debug_end_scope(Sym *b, int bounds)
{
#ifdef CONFIG_TCC_BCHECK
    if (tcc_state->do_bounds_check && bounds)
        add_local_bounds(local_stack, b);
#endif
    tcc_add_debug_info(tcc_state, local_stack, b);
}

static void incr_offset(int offset)
{
    int t = vtop->type.t;
    gaddrof();
    vtop->type.t = VT_PTRDIFF_T;
    vpushs(offset);
    gen_op('+');
    vtop->r |= VT_LVAL;
    vtop->type.t = t;
}

static void incr_bf_adr(int o)
{
    vtop->type.t = VT_BYTE | VT_UNSIGNED;
    incr_offset(o);
}

static void load_packed_bf(CType *type, int bit_pos, int bit_size)
{
    int n, o, bits;
    save_reg_upstack(vtop->r, 1);
    vpush64(type->t & VT_BTYPE, 0);
    bits = 0, o = bit_pos >> 3, bit_pos &= 7;
    do {
        vswap();
        incr_bf_adr(o);
        vdup();
        n = 8 - bit_pos;
        if (n > bit_size)
            n = bit_size;
        if (bit_pos)
            vpushi(bit_pos), gen_op(TOK_SHR), bit_pos = 0;
        if (n < 8)
            vpushi((1 << n) - 1), gen_op('&');
        gen_cast(type);
        if (bits)
            vpushi(bits), gen_op(TOK_SHL);
        vrotb(3);
        gen_op('|');
        bits += n, bit_size -= n, o = 1;
    } while (bit_size);
    vswap(), vpop();
    if (!(type->t & VT_UNSIGNED)) {
        n = ((type->t & VT_BTYPE) == VT_LLONG ? 64 : 32) - bits;
        vpushi(n), gen_op(TOK_SHL);
        vpushi(n), gen_op(TOK_SAR);
    }
}

static void store_packed_bf(int bit_pos, int bit_size)
{
    int bits, n, o, m, c;
    c = (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    vswap();
    save_reg_upstack(vtop->r, 1);
    bits = 0, o = bit_pos >> 3, bit_pos &= 7;
    do {
        incr_bf_adr(o);
        vswap();
        c ? vdup() : gv_dup();
        vrott(3);
        if (bits)
            vpushi(bits), gen_op(TOK_SHR);
        if (bit_pos)
            vpushi(bit_pos), gen_op(TOK_SHL);
        n = 8 - bit_pos;
        if (n > bit_size)
            n = bit_size;
        if (n < 8) {
            m = ((1 << n) - 1) << bit_pos;
            vpushi(m), gen_op('&');
            vpushv(vtop - 1);
            vpushi(m & 0x80 ? ~m & 0x7f : ~m);
            gen_op('&');
            gen_op('|');
        }
        vdup(), vtop[-1] = vtop[-2];
        vstore(), vpop();
        bits += n, bit_size -= n, bit_pos = 0, o = 1;
    } while (bit_size);
    vpop(), vpop();
}

static int adjust_bf(SValue *sv, int bit_pos, int bit_size)
{
    int t;
    if (0 == sv->type.ref)
        return 0;
    t = sv->type.ref->auxtype;
    if (t != -1 && t != VT_STRUCT) {
        sv->type.t = (sv->type.t & ~(VT_BTYPE | VT_LONG)) | t;
        sv->r |= VT_LVAL;
    }
    return t;
}

ST_FUNC int gv(int rc)
{
    int r, r2, r_ok, r2_ok, rc2, bt;
    int bit_pos, bit_size, size, align;

    if (vtop->type.t & VT_BITFIELD) {
        CType type;

        bit_pos = BIT_POS(vtop->type.t);
        bit_size = BIT_SIZE(vtop->type.t);

        vtop->type.t &= ~VT_STRUCT_MASK;

        type.ref = NULL;
        type.t = vtop->type.t & VT_UNSIGNED;
        if ((vtop->type.t & VT_BTYPE) == VT_BOOL)
            type.t |= VT_UNSIGNED;

        r = adjust_bf(vtop, bit_pos, bit_size);

        if ((vtop->type.t & VT_BTYPE) == VT_LLONG)
            type.t |= VT_LLONG;
        else
            type.t |= VT_INT;

        if (r == VT_STRUCT) {
            load_packed_bf(&type, bit_pos, bit_size);
        } else {
            int bits = (type.t & VT_BTYPE) == VT_LLONG ? 64 : 32;

            gen_cast(&type);

            vpushi(bits - (bit_pos + bit_size));
            gen_op(TOK_SHL);
            vpushi(bits - bit_size);

            gen_op(TOK_SAR);
        }
        r = gv(rc);
    } else {
        if (is_float(vtop->type.t) && (vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST) {

            init_params p = {rodata_section};
            unsigned long offset;
            size = type_size(&vtop->type, &align);
            if (NODATA_WANTED)
                size = 0, align = 1;
            offset = section_add(p.sec, size, align);
            vpush_ref(&vtop->type, p.sec, offset, size);
            vswap();
            init_putv(&p, &vtop->type, offset);
            vtop->r |= VT_LVAL;
        }
#ifdef CONFIG_TCC_BCHECK
        if (vtop->r & VT_MUSTBOUND)
            gbound();
#endif

        bt = vtop->type.t & VT_BTYPE;
        if (bt == VT_VOID || bt == VT_STRUCT)
            return vtop->r;

#ifdef TCC_TARGET_RISCV64

        if (bt == VT_LDOUBLE && rc == RC_FLOAT)
            rc = RC_INT;
#endif
        rc2 = RC2_TYPE(bt, rc);

        r = vtop->r & VT_VALMASK;
        r_ok = !(vtop->r & VT_LVAL) && (r < VT_CONST) && (reg_classes[r] & rc);
        r2_ok = !rc2 || ((vtop->r2 < VT_CONST) && (reg_classes[vtop->r2] & rc2));

        if (!r_ok || !r2_ok) {

            if (!r_ok) {
                if (1 && r < VT_CONST && (reg_classes[r] & rc) && !rc2)
                    save_reg_upstack(r, 1);
                else
                    r = get_reg(rc);
            }

            if (rc2) {
                int load_type = (bt == VT_QFLOAT) ? VT_DOUBLE : VT_PTRDIFF_T;
                int original_type = vtop->type.t;

                if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST) {

                    unsigned long long ll = vtop->c.i;
                    vtop->c.i = ll;
                    load(r, vtop);
                    vtop->r = r;
                    vpushi(ll >> 32);
                } else if (vtop->r & VT_LVAL) {

                    save_reg_upstack(vtop->r, 1);

                    vtop->type.t = load_type;
                    load(r, vtop);
                    vdup();
                    vtop[-1].r = r;

                    incr_offset(PTR_SIZE);
                } else {

                    if (!r_ok)
                        load(r, vtop);
                    if (r2_ok && vtop->r2 < VT_CONST)
                        goto done;
                    vdup();
                    vtop[-1].r = r;
                    vtop->r = vtop[-1].r2;
                }

                r2 = get_reg(rc2);
                load(r2, vtop);
                vpop();

                vtop->r2 = r2;
            done:
                vtop->type.t = original_type;
            } else {
                if (vtop->r == VT_CMP)
                    vset_VT_JMP();

                load(r, vtop);
            }
        }
        vtop->r = r;
#ifdef TCC_TARGET_C67

        if (bt == VT_DOUBLE)
            vtop->r2 = r + 1;
#endif
    }
    return r;
}

ST_FUNC void gv2(int rc1, int rc2)
{

    if (vtop->r != VT_CMP && rc1 <= rc2) {
        vswap();
        gv(rc1);
        vswap();
        gv(rc2);

        if ((vtop[-1].r & VT_VALMASK) >= VT_CONST) {
            vswap();
            gv(rc1);
            vswap();
        }
    } else {
        gv(rc2);
        vswap();
        gv(rc1);
        vswap();

        if ((vtop[0].r & VT_VALMASK) >= VT_CONST) {
            gv(rc2);
        }
    }
}

#if PTR_SIZE == 4

ST_FUNC void lexpand(void)
{
    int u, v;
    u = vtop->type.t & (VT_DEFSIGN | VT_UNSIGNED);
    v = vtop->r & (VT_VALMASK | VT_LVAL);
    if (v == VT_CONST) {
        vdup();
        vtop[0].c.i >>= 32;
    } else if (v == (VT_LVAL | VT_CONST) || v == (VT_LVAL | VT_LOCAL)) {
        vdup();
        vtop[0].c.i += 4;
    } else {
        gv(RC_INT);
        vdup();
        vtop[0].r = vtop[-1].r2;
        vtop[0].r2 = vtop[-1].r2 = VT_CONST;
    }
    vtop[0].type.t = vtop[-1].type.t = VT_INT | u;
}
#endif

#if PTR_SIZE == 4

static void lbuild(int t)
{
    gv2(RC_INT, RC_INT);
    vtop[-1].r2 = vtop[0].r;
    vtop[-1].type.t = t;
    vpop();
}
#endif

static void gv_dup(void)
{
    int t, rc, r;

    t = vtop->type.t;
#if PTR_SIZE == 4
    if ((t & VT_BTYPE) == VT_LLONG) {
        if (t & VT_BITFIELD) {
            gv(RC_INT);
            t = vtop->type.t;
        }
        lexpand();
        gv_dup();
        vswap();
        vrotb(3);
        gv_dup();
        vrotb(4);

        lbuild(t);
        vrotb(3);
        vrotb(3);
        vswap();
        lbuild(t);
        vswap();
        return;
    }
#endif

    rc = RC_TYPE(t);
    gv(rc);
    r = get_reg(rc);
    vdup();
    load(r, vtop);
    vtop->r = r;
}

#if PTR_SIZE == 4

static void gen_opl(int op)
{
    int t, a, b, op1, c, i;
    int func;
    unsigned short reg_iret = REG_IRET;
    unsigned short reg_lret = REG_IRE2;
    SValue tmp;

    switch (op) {
        case '/':
        case TOK_PDIV:
            func = TOK___divdi3;
            goto gen_func;
        case TOK_UDIV:
            func = TOK___udivdi3;
            goto gen_func;
        case '%':
            func = TOK___moddi3;
            goto gen_mod_func;
        case TOK_UMOD:
            func = TOK___umoddi3;
        gen_mod_func:
#ifdef TCC_ARM_EABI
            reg_iret = TREG_R2;
            reg_lret = TREG_R3;
#endif
        gen_func:

            vpush_helper_func(func);
            vrott(3);
            gfunc_call(2);
            vpushi(0);
            vtop->r = reg_iret;
            vtop->r2 = reg_lret;
            break;
        case '^':
        case '&':
        case '|':
        case '*':
        case '+':
        case '-':

            t = vtop->type.t;
            vswap();
            lexpand();
            vrotb(3);
            lexpand();

            tmp = vtop[0];
            vtop[0] = vtop[-3];
            vtop[-3] = tmp;
            tmp = vtop[-2];
            vtop[-2] = vtop[-3];
            vtop[-3] = tmp;
            vswap();

            if (op == '*') {
                vpushv(vtop - 1);
                vpushv(vtop - 1);
                gen_op(TOK_UMULL);
                lexpand();

                for (i = 0; i < 4; i++)
                    vrotb(6);

                tmp = vtop[0];
                vtop[0] = vtop[-2];
                vtop[-2] = tmp;

                gen_op('*');
                vrotb(3);
                vrotb(3);
                gen_op('*');

                gen_op('+');
                gen_op('+');
            } else if (op == '+' || op == '-') {

                if (op == '+')
                    op1 = TOK_ADDC1;
                else
                    op1 = TOK_SUBC1;
                gen_op(op1);

                vrotb(3);
                vrotb(3);
                gen_op(op1 + 1);
            } else {
                gen_op(op);

                vrotb(3);
                vrotb(3);

                gen_op(op);
            }

            lbuild(t);
            break;
        case TOK_SAR:
        case TOK_SHR:
        case TOK_SHL:
            if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
                t = vtop[-1].type.t;
                vswap();
                lexpand();
                vrotb(3);

                c = (int)vtop->c.i;

                vpop();
                if (op != TOK_SHL)
                    vswap();
                if (c >= 32) {

                    vpop();
                    if (c > 32) {
                        vpushi(c - 32);
                        gen_op(op);
                    }
                    if (op != TOK_SAR) {
                        vpushi(0);
                    } else {
                        gv_dup();
                        vpushi(31);
                        gen_op(TOK_SAR);
                    }
                    vswap();
                } else {
                    vswap();
                    gv_dup();

                    vpushi(c);
                    gen_op(op);
                    vswap();
                    vpushi(32 - c);
                    if (op == TOK_SHL)
                        gen_op(TOK_SHR);
                    else
                        gen_op(TOK_SHL);
                    vrotb(3);

                    vpushi(c);
                    if (op == TOK_SHL)
                        gen_op(TOK_SHL);
                    else
                        gen_op(TOK_SHR);
                    gen_op('|');
                }
                if (op != TOK_SHL)
                    vswap();
                lbuild(t);
            } else {

                switch (op) {
                    case TOK_SAR:
                        func = TOK___ashrdi3;
                        goto gen_func;
                    case TOK_SHR:
                        func = TOK___lshrdi3;
                        goto gen_func;
                    case TOK_SHL:
                        func = TOK___ashldi3;
                        goto gen_func;
                }
            }
            break;
        default:

            t = vtop->type.t;
            vswap();
            lexpand();
            vrotb(3);
            lexpand();

            tmp = vtop[-1];
            vtop[-1] = vtop[-2];
            vtop[-2] = tmp;

            if (!cur_switch || cur_switch->bsym) {

                save_regs(4);
            }

            op1 = op;

            if (op1 == TOK_LT)
                op1 = TOK_LE;
            else if (op1 == TOK_GT)
                op1 = TOK_GE;
            else if (op1 == TOK_ULT)
                op1 = TOK_ULE;
            else if (op1 == TOK_UGT)
                op1 = TOK_UGE;
            a = 0;
            b = 0;
            gen_op(op1);
            if (op == TOK_NE) {
                b = gvtst(0, 0);
            } else {
                a = gvtst(1, 0);
                if (op != TOK_EQ) {

                    vpushi(0);
                    vset_VT_CMP(TOK_NE);
                    b = gvtst(0, 0);
                }
            }

            op1 = op;
            if (op1 == TOK_LT)
                op1 = TOK_ULT;
            else if (op1 == TOK_LE)
                op1 = TOK_ULE;
            else if (op1 == TOK_GT)
                op1 = TOK_UGT;
            else if (op1 == TOK_GE)
                op1 = TOK_UGE;
            gen_op(op1);
#if 0 
        if (op == TOK_NE) { gsym(b); break; }
        if (op == TOK_EQ) { gsym(a); break; }
#endif
            gvtst_set(1, a);
            gvtst_set(0, b);
            break;
    }
}
#endif

static uint64_t value64(uint64_t l1, int t)
{
    if ((t & VT_BTYPE) == VT_LLONG || (PTR_SIZE == 8 && (t & VT_BTYPE) == VT_PTR))
        return l1;
    else if (t & VT_UNSIGNED)
        return (uint32_t)l1;
    else
        return (uint32_t)l1 | -(l1 & 0x80000000);
}

static uint64_t gen_opic_sdiv(uint64_t a, uint64_t b)
{
    uint64_t x = (a >> 63 ? -a : a) / (b >> 63 ? -b : b);
    return (a ^ b) >> 63 ? -x : x;
}

static int gen_opic_lt(uint64_t a, uint64_t b)
{
    return (a ^ (uint64_t)1 << 63) < (b ^ (uint64_t)1 << 63);
}

static void gen_opic(int op)
{
    SValue *v1 = vtop - 1;
    SValue *v2 = vtop;
    int t1 = v1->type.t & VT_BTYPE;
    int t2 = v2->type.t & VT_BTYPE;
    int c1 = (v1->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    int c2 = (v2->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    uint64_t l1 = c1 ? value64(v1->c.i, v1->type.t) : 0;
    uint64_t l2 = c2 ? value64(v2->c.i, v2->type.t) : 0;
    int shm = (t1 == VT_LLONG) ? 63 : 31;
    int r;

    if (c1 && c2) {
        switch (op) {
            case '+':
                l1 += l2;
                break;
            case '-':
                l1 -= l2;
                break;
            case '&':
                l1 &= l2;
                break;
            case '^':
                l1 ^= l2;
                break;
            case '|':
                l1 |= l2;
                break;
            case '*':
                l1 *= l2;
                break;

            case TOK_PDIV:
            case '/':
            case '%':
            case TOK_UDIV:
            case TOK_UMOD:

                if (l2 == 0) {
                    if (CONST_WANTED && !NOEVAL_WANTED)
                        tcc_error("division by zero in constant");
                    goto general_case;
                }
                switch (op) {
                    default:
                        l1 = gen_opic_sdiv(l1, l2);
                        break;
                    case '%':
                        l1 = l1 - l2 * gen_opic_sdiv(l1, l2);
                        break;
                    case TOK_UDIV:
                        l1 = l1 / l2;
                        break;
                    case TOK_UMOD:
                        l1 = l1 % l2;
                        break;
                }
                break;
            case TOK_SHL:
                l1 <<= (l2 & shm);
                break;
            case TOK_SHR:
                l1 >>= (l2 & shm);
                break;
            case TOK_SAR:
                l1 = (l1 >> 63) ? ~(~l1 >> (l2 & shm)) : l1 >> (l2 & shm);
                break;

            case TOK_ULT:
                l1 = l1 < l2;
                break;
            case TOK_UGE:
                l1 = l1 >= l2;
                break;
            case TOK_EQ:
                l1 = l1 == l2;
                break;
            case TOK_NE:
                l1 = l1 != l2;
                break;
            case TOK_ULE:
                l1 = l1 <= l2;
                break;
            case TOK_UGT:
                l1 = l1 > l2;
                break;
            case TOK_LT:
                l1 = gen_opic_lt(l1, l2);
                break;
            case TOK_GE:
                l1 = !gen_opic_lt(l1, l2);
                break;
            case TOK_LE:
                l1 = !gen_opic_lt(l2, l1);
                break;
            case TOK_GT:
                l1 = gen_opic_lt(l2, l1);
                break;

            case TOK_LAND:
                l1 = l1 && l2;
                break;
            case TOK_LOR:
                l1 = l1 || l2;
                break;
            default:
                goto general_case;
        }
        v1->c.i = value64(l1, v1->type.t);
        v1->r |= v2->r & VT_NONCONST;
        vtop--;
    } else {

        if (c1 && (op == '+' || op == '&' || op == '^' || op == '|' || op == '*' || op == TOK_EQ ||
                   op == TOK_NE)) {
            vswap();
            c2 = c1;
            l2 = l1;
        }
        if (c1 && ((l1 == 0 && (op == TOK_SHL || op == TOK_SHR || op == TOK_SAR)) ||
                   (l1 == -1 && op == TOK_SAR))) {

            vpop();
        } else if (c2 && ((l2 == 0 && (op == '&' || op == '*')) ||
                          (op == '|' && (l2 == -1 || (l2 == 0xFFFFFFFF && t2 != VT_LLONG))) ||
                          (l2 == 1 && (op == '%' || op == TOK_UMOD)))) {

            if (l2 == 1)
                vtop->c.i = 0;
            vswap();
            vtop--;
        } else if (c2 &&
                   (((op == '*' || op == '/' || op == TOK_UDIV || op == TOK_PDIV) && l2 == 1) ||
                    ((op == '+' || op == '-' || op == '|' || op == '^' || op == TOK_SHL ||
                      op == TOK_SHR || op == TOK_SAR) &&
                     l2 == 0) ||
                    (op == '&' && (l2 == -1 || (l2 == 0xFFFFFFFF && t2 != VT_LLONG))))) {

            vtop--;
        } else if (c2 && (op == '*' || op == TOK_PDIV || op == TOK_UDIV || op == TOK_UMOD)) {

            if (l2 > 0 && (l2 & (l2 - 1)) == 0) {
                int n = -1;
                if (op == TOK_UMOD) {
                    vtop->c.i = l2 - 1;
                    op = '&';
                    goto general_case;
                }
                while (l2) {
                    l2 >>= 1;
                    n++;
                }
                vtop->c.i = n;
                if (op == '*')
                    op = TOK_SHL;
                else if (op == TOK_PDIV)
                    op = TOK_SAR;
                else
                    op = TOK_SHR;
            }
            goto general_case;
        } else if (c2 && (op == '+' || op == '-') &&
                   (r = vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM),
                    r == (VT_CONST | VT_SYM) || r == VT_LOCAL)) {

            if (op == '-')
                l2 = -l2;
            l2 += vtop[-1].c.i;

            if ((int)l2 != l2)
                goto general_case;
            vtop--;
            vtop->c.i = l2;
        } else {
        general_case:

            if (t1 == VT_LLONG || t2 == VT_LLONG ||
                (PTR_SIZE == 8 && (t1 == VT_PTR || t2 == VT_PTR)))
                gen_opl(op);
            else
                gen_opi(op);
        }
        if (vtop->r == VT_CONST)
            vtop->r |= VT_NONCONST;
    }
}

#if defined TCC_TARGET_X86_64 || defined TCC_TARGET_I386 || defined TCC_TARGET_ARM64
#define gen_negf gen_opf
#elif defined TCC_TARGET_ARM
void gen_negf(int op)
{

    vpushi(0), vswap(), gen_op('-');
}
#else

void gen_negf(int op)
{

    int align, size, bt;
    size = type_size(&vtop->type, &align);
    bt = vtop->type.t & VT_BTYPE;
#if defined TCC_TARGET_X86_64 || defined TCC_TARGET_I386

    if (bt == VT_LDOUBLE)
        size = 10;
#endif
    if (nocode_wanted)
        goto gv2;
    save_reg(gv(RC_TYPE(bt)));
    vdup();
    incr_bf_adr(size - 1);
    vdup();
    vpushi(0x80);
    gen_op('^');
    vstore();
    vpop();
gv2:
    gv(RC_TYPE(bt));
}
#endif

static void gen_opif(int op)
{
    int c1, c2, i, bt;
    SValue *v1, *v2;
#if defined _MSC_VER && defined __x86_64__

    volatile
#endif
        long double f1,
        f2;

    v1 = vtop - 1;
    v2 = vtop;
    if (op == TOK_NEG)
        v1 = v2;
    bt = v1->type.t & VT_BTYPE;

    c1 = (v1->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    c2 = (v2->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    if (c1 && c2) {
        if (bt == VT_FLOAT) {
            f1 = v1->c.f;
            f2 = v2->c.f;
        } else if (bt == VT_DOUBLE) {
            f1 = v1->c.d;
            f2 = v2->c.d;
        } else {
            f1 = v1->c.ld;
            f2 = v2->c.ld;
        }

        if (!(ieee_finite(f1) || !ieee_finite(f2)) && !CONST_WANTED)
            goto general_case;
        switch (op) {
            case '+':
                f1 += f2;
                break;
            case '-':
                f1 -= f2;
                break;
            case '*':
                f1 *= f2;
                break;
            case '/':
                if (f2 == 0.0) {
                    union {
                        float f;
                        unsigned u;
                    } x1, x2, y;

                    if (!CONST_WANTED)
                        goto general_case;

                    x1.f = f1, x2.f = f2;
                    if (f1 == 0.0)
                        y.u = 0x7fc00000;
                    else
                        y.u = 0x7f800000;
                    y.u |= (x1.u ^ x2.u) & 0x80000000;
                    f1 = y.f;
                    break;
                }
                f1 /= f2;
                break;
            case TOK_NEG:
                f1 = -f1;
                goto unary_result;
            case TOK_EQ:
                i = f1 == f2;
            make_int:
                vtop -= 2;
                vpushi(i);
                return;
            case TOK_NE:
                i = f1 != f2;
                goto make_int;
            case TOK_LT:
                i = f1 < f2;
                goto make_int;
            case TOK_GE:
                i = f1 >= f2;
                goto make_int;
            case TOK_LE:
                i = f1 <= f2;
                goto make_int;
            case TOK_GT:
                i = f1 > f2;
                goto make_int;
            default:
                goto general_case;
        }
        vtop--;
    unary_result:

        if (bt == VT_FLOAT) {
            v1->c.f = f1;
        } else if (bt == VT_DOUBLE) {
            v1->c.d = f1;
        } else {
            v1->c.ld = f1;
        }
    } else {
    general_case:
        if (op == TOK_NEG) {
            gen_negf(op);
        } else {
            gen_opf(op);
        }
    }
}

static void type_to_str(char *buf, int buf_size, CType *type, const char *varstr)
{
    int bt, v, t;
    Sym *s, *sa;
    char buf1[256];
    const char *tstr;

    t = type->t;
    bt = t & VT_BTYPE;
    buf[0] = '\0';

    if (t & VT_EXTERN)
        pstrcat(buf, buf_size, "extern ");
    if (t & VT_STATIC)
        pstrcat(buf, buf_size, "static ");
    if (t & VT_TYPEDEF)
        pstrcat(buf, buf_size, "typedef ");
    if (t & VT_INLINE)
        pstrcat(buf, buf_size, "inline ");
    if (bt != VT_PTR) {
        if (t & VT_RESTRICT)
            pstrcat(buf, buf_size, "restrict ");
        if (t & VT_VOLATILE)
            pstrcat(buf, buf_size, "volatile ");
        if (t & VT_CONSTANT)
            pstrcat(buf, buf_size, "const ");
    }
    if (((t & VT_DEFSIGN) && bt == VT_BYTE) ||
        ((t & VT_UNSIGNED) && (bt == VT_SHORT || bt == VT_INT || bt == VT_LLONG) && !IS_ENUM(t)))
        pstrcat(buf, buf_size, (t & VT_UNSIGNED) ? "unsigned " : "signed ");

    buf_size -= strlen(buf);
    buf += strlen(buf);

    switch (bt) {
        case VT_VOID:
            tstr = "void";
            goto add_tstr;
        case VT_BOOL:
            tstr = "_Bool";
            goto add_tstr;
        case VT_BYTE:
            tstr = "char";
            goto add_tstr;
        case VT_SHORT:
            tstr = "short";
            goto add_tstr;
        case VT_INT:
            tstr = "int";
            goto maybe_long;
        case VT_LLONG:
            tstr = "long long";
        maybe_long:
            if (t & VT_LONG)
                tstr = "long";
            if (!IS_ENUM(t))
                goto add_tstr;
            tstr = "enum ";
            goto tstruct;
        case VT_FLOAT:
            tstr = "float";
            goto add_tstr;
        case VT_DOUBLE:
            tstr = "double";
            if (!(t & VT_LONG))
                goto add_tstr;
        case VT_LDOUBLE:
            tstr = "long double";
        add_tstr:
            pstrcat(buf, buf_size, tstr);
            break;
        case VT_STRUCT:
            tstr = "struct ";
            if (IS_UNION(t))
                tstr = "union ";
        tstruct:
            pstrcat(buf, buf_size, tstr);
            v = type->ref->v & ~SYM_STRUCT;
            if (v >= SYM_FIRST_ANOM)
                pstrcat(buf, buf_size, "<anonymous>");
            else
                pstrcat(buf, buf_size, get_tok_str(v, NULL));
            break;
        case VT_FUNC:
            s = type->ref;
            buf1[0] = 0;
            if (varstr && '*' == *varstr) {
                pstrcat(buf1, sizeof(buf1), "(");
                pstrcat(buf1, sizeof(buf1), varstr);
                pstrcat(buf1, sizeof(buf1), ")");
            }
            pstrcat(buf1, sizeof(buf1), "(");
            sa = s->next;
            while (sa != NULL) {
                char buf2[256];
                type_to_str(buf2, sizeof(buf2), &sa->type, NULL);
                pstrcat(buf1, sizeof(buf1), buf2);
                sa = sa->next;
                if (sa)
                    pstrcat(buf1, sizeof(buf1), ", ");
            }
            if (s->f.func_type == FUNC_ELLIPSIS)
                pstrcat(buf1, sizeof(buf1), ", ...");
            pstrcat(buf1, sizeof(buf1), ")");
            type_to_str(buf, buf_size, &s->type, buf1);
            goto no_var;
        case VT_PTR:
            s = type->ref;
            if (t & (VT_ARRAY | VT_VLA)) {
                if (varstr && '*' == *varstr)
                    snprintf(buf1, sizeof(buf1), "(%s)[%d]", varstr, s->c);
                else
                    snprintf(buf1, sizeof(buf1), "%s[%d]", varstr ? varstr : "", s->c);
                type_to_str(buf, buf_size, &s->type, buf1);
                goto no_var;
            }
            pstrcpy(buf1, sizeof(buf1), "*");
            if (t & VT_RESTRICT)
                pstrcat(buf1, sizeof(buf1), "restrict ");
            if (t & VT_CONSTANT)
                pstrcat(buf1, sizeof(buf1), "const ");
            if (t & VT_VOLATILE)
                pstrcat(buf1, sizeof(buf1), "volatile ");
            if (varstr)
                pstrcat(buf1, sizeof(buf1), varstr);
            type_to_str(buf, buf_size, &s->type, buf1);
            goto no_var;
    }
    if (varstr) {
        pstrcat(buf, buf_size, " ");
        pstrcat(buf, buf_size, varstr);
    }
no_var:;
}

static void type_incompatibility_error(CType *st, CType *dt, const char *fmt)
{
    char buf1[256], buf2[256];
    type_to_str(buf1, sizeof(buf1), st, NULL);
    type_to_str(buf2, sizeof(buf2), dt, NULL);
    tcc_error(fmt, buf1, buf2);
}

static void type_incompatibility_warning(CType *st, CType *dt, const char *fmt)
{
    char buf1[256], buf2[256];
    type_to_str(buf1, sizeof(buf1), st, NULL);
    type_to_str(buf2, sizeof(buf2), dt, NULL);
    tcc_warning(fmt, buf1, buf2);
}

static int pointed_size(CType *type)
{
    int align;
    return type_size(pointed_type(type), &align);
}

static inline int is_null_pointer(SValue *p)
{
    if ((p->r & (VT_VALMASK | VT_LVAL | VT_SYM | VT_NONCONST)) != VT_CONST)
        return 0;
    return ((p->type.t & VT_BTYPE) == VT_INT && (uint32_t)p->c.i == 0) ||
           ((p->type.t & VT_BTYPE) == VT_LLONG && p->c.i == 0) ||
           ((p->type.t & VT_BTYPE) == VT_PTR &&
            (PTR_SIZE == 4 ? (uint32_t)p->c.i == 0 : p->c.i == 0) &&
            ((pointed_type(&p->type)->t & VT_BTYPE) == VT_VOID) &&
            0 == (pointed_type(&p->type)->t & VT_QUAL));
}

static int is_compatible_func(CType *type1, CType *type2)
{
    Sym *s1, *s2;

    s1 = type1->ref;
    s2 = type2->ref;
    if (s1->f.func_call != s2->f.func_call)
        return 0;
    if (s1->f.func_type != s2->f.func_type && s1->f.func_type != FUNC_OLD &&
        s2->f.func_type != FUNC_OLD)
        return 0;
    for (;;) {
        if (!is_compatible_unqualified_types(&s1->type, &s2->type))
            return 0;
        if (s1->f.func_type == FUNC_OLD || s2->f.func_type == FUNC_OLD)
            return 1;
        s1 = s1->next;
        s2 = s2->next;
        if (!s1)
            return !s2;
        if (!s2)
            return 0;
    }
}

static int compare_types(CType *type1, CType *type2, int unqualified)
{
    int bt1, t1, t2;

    if (IS_ENUM(type1->t)) {
        if (IS_ENUM(type2->t))
            return type1->ref == type2->ref;
        type1 = &type1->ref->type;
    } else if (IS_ENUM(type2->t))
        type2 = &type2->ref->type;

    t1 = type1->t & VT_TYPE;
    t2 = type2->t & VT_TYPE;
    if (unqualified) {

        t1 &= ~VT_QUAL;
        t2 &= ~VT_QUAL;
    }

    if ((t1 & VT_BTYPE) != VT_BYTE) {
        t1 &= ~VT_DEFSIGN;
        t2 &= ~VT_DEFSIGN;
    }

    if (t1 != t2)
        return 0;

    if ((t1 & VT_ARRAY) &&
        !(type1->ref->c < 0 || type2->ref->c < 0 || type1->ref->c == type2->ref->c))
        return 0;

    bt1 = t1 & VT_BTYPE;
    if (bt1 == VT_PTR) {
        type1 = pointed_type(type1);
        type2 = pointed_type(type2);
        if (unqualified && (t1 & VT_ARRAY))
            return compare_types(type1, type2, 1);
        return is_compatible_types(type1, type2);
    } else if (bt1 == VT_STRUCT) {
        return (type1->ref == type2->ref);
    } else if (bt1 == VT_FUNC) {
        return is_compatible_func(type1, type2);
    } else {
        return 1;
    }
}

#define CMP_OP 'C'
#define SHIFT_OP 'S'

static int combine_types(CType *dest, SValue *op1, SValue *op2, int op)
{
    CType *type1, *type2, type;
    int t1, t2, bt1, bt2;
    int ret = 1;

    if (op == SHIFT_OP)
        op2 = op1;

    type1 = &op1->type, type2 = &op2->type;
    t1 = type1->t, t2 = type2->t;
    bt1 = t1 & VT_BTYPE, bt2 = t2 & VT_BTYPE;

    type.t = VT_VOID;
    type.ref = NULL;

    if (bt1 == VT_VOID || bt2 == VT_VOID) {
        if (op != '?')
            tcc_error("operation on void value");

        type.t = VT_VOID;
    } else if (bt1 == VT_PTR || bt2 == VT_PTR) {
        if (op == '+') {
            if (!is_integer_btype(bt1 == VT_PTR ? bt2 : bt1))
                ret = 0;
        }

        else if (is_null_pointer(op2))
            type = *type1;
        else if (is_null_pointer(op1))
            type = *type2;
        else if (bt1 != bt2) {

            if ((op == '?' || op == CMP_OP) && (is_integer_btype(bt1) || is_integer_btype(bt2)))
                tcc_warning("pointer/integer mismatch in %s",
                            op == '?' ? "conditional expression" : "comparison");
            else if (op != '-' || !is_integer_btype(bt2))
                ret = 0;
            type = *(bt1 == VT_PTR ? type1 : type2);
        } else {
            CType *pt1 = pointed_type(type1);
            CType *pt2 = pointed_type(type2);
            int pbt1 = pt1->t & VT_BTYPE;
            int pbt2 = pt2->t & VT_BTYPE;
            int newquals, copied = 0;
            if (pbt1 != VT_VOID && pbt2 != VT_VOID && !compare_types(pt1, pt2, 1)) {
                if (op != '?' && op != CMP_OP)
                    ret = 0;
                else
                    type_incompatibility_warning(
                        type1, type2,
                        op == '?'
                            ? "pointer type mismatch in conditional expression ('%s' and '%s')"
                            : "pointer type mismatch in comparison('%s' and '%s')");
            }
            if (op == '?') {

                type = *((pbt1 == VT_VOID) ? type1 : type2);

                newquals = type_qualifiers(pt1) | type_qualifiers(pt2);
                if ((~type_qualifiers(pointed_type(&type))) & newquals) {

                    type.ref = sym_push(SYM_FIELD, &type.ref->type, 0, type.ref->c);
                    copied = 1;
                    parse_btype_qualify(pointed_type(&type), newquals);
                }

                if (pt1->t & VT_ARRAY && pt2->t & VT_ARRAY && pointed_type(&type)->ref->c < 0 &&
                    (pt1->ref->c > 0 || pt2->ref->c > 0)) {
                    if (!copied)
                        type.ref = sym_push(SYM_FIELD, &type.ref->type, 0, type.ref->c);
                    pointed_type(&type)->ref = sym_push(SYM_FIELD, &pointed_type(&type)->ref->type,
                                                        0, pointed_type(&type)->ref->c);
                    pointed_type(&type)->ref->c = 0 < pt1->ref->c ? pt1->ref->c : pt2->ref->c;
                }
            }
        }
        if (op == CMP_OP)
            type.t = VT_SIZE_T;
    } else if (bt1 == VT_STRUCT || bt2 == VT_STRUCT) {
        if (op != '?' || !compare_types(type1, type2, 1))
            ret = 0;
        type = *type1;
    } else if (is_float(bt1) || is_float(bt2)) {
        if (bt1 == VT_LDOUBLE || bt2 == VT_LDOUBLE) {
            type.t = VT_LDOUBLE;
        } else if (bt1 == VT_DOUBLE || bt2 == VT_DOUBLE) {
            type.t = VT_DOUBLE;
        } else {
            type.t = VT_FLOAT;
        }
    } else if (bt1 == VT_LLONG || bt2 == VT_LLONG) {

        type.t = VT_LLONG | VT_LONG;
        if (bt1 == VT_LLONG)
            type.t &= t1;
        if (bt2 == VT_LLONG)
            type.t &= t2;

        if ((t1 & (VT_BTYPE | VT_UNSIGNED)) == (VT_LLONG | VT_UNSIGNED) ||
            (t2 & (VT_BTYPE | VT_UNSIGNED)) == (VT_LLONG | VT_UNSIGNED))
            type.t |= VT_UNSIGNED;
    } else {

        type.t = VT_INT | (VT_LONG & (t1 | t2));

        if (((t1 & (VT_BTYPE | VT_UNSIGNED)) == (VT_INT | VT_UNSIGNED) &&
             (!(t1 & VT_BITFIELD) || BIT_SIZE(t1) == 32)) ||
            ((t2 & (VT_BTYPE | VT_UNSIGNED)) == (VT_INT | VT_UNSIGNED) &&
             (!(t2 & VT_BITFIELD) || BIT_SIZE(t2) == 32)))
            type.t |= VT_UNSIGNED;
    }
    if (dest)
        *dest = type;
    return ret;
}

ST_FUNC void gen_op(int op)
{
    int t1, t2, bt1, bt2, t;
    CType type1, combtype;
    int op_class = op;

    if (op == TOK_SHR || op == TOK_SAR || op == TOK_SHL)
        op_class = SHIFT_OP;
    else if (TOK_ISCOND(op))
        op_class = CMP_OP;

redo:
    t1 = vtop[-1].type.t;
    t2 = vtop[0].type.t;
    bt1 = t1 & VT_BTYPE;
    bt2 = t2 & VT_BTYPE;

    if (bt1 == VT_FUNC || bt2 == VT_FUNC) {
        if (bt2 == VT_FUNC) {
            mk_pointer(&vtop->type);
            gaddrof();
        }
        if (bt1 == VT_FUNC) {
            vswap();
            mk_pointer(&vtop->type);
            gaddrof();
            vswap();
        }
        goto redo;
    } else if (!combine_types(&combtype, vtop - 1, vtop, op_class)) {
    op_err:
        tcc_error("invalid operand types for binary operation");
    } else if (bt1 == VT_PTR || bt2 == VT_PTR) {

        int align;
        if (op_class == CMP_OP)
            goto std_op;

        if (bt1 == VT_PTR && bt2 == VT_PTR) {
            if (op != '-')
                goto op_err;
            vpush_type_size(pointed_type(&vtop[-1].type), &align);
            vtop->type.t &= ~VT_UNSIGNED;
            vrott(3);
            gen_opic(op);
            vtop->type.t = VT_PTRDIFF_T;
            vswap();
            gen_op(TOK_PDIV);
        } else {

            if (op != '-' && op != '+')
                goto op_err;

            if (bt2 == VT_PTR) {
                vswap();
                t = t1, t1 = t2, t2 = t;
                bt2 = bt1;
            }
#if PTR_SIZE == 4
            if (bt2 == VT_LLONG)

                gen_cast_s(VT_INT);
#endif
            type1 = vtop[-1].type;
            vpush_type_size(pointed_type(&vtop[-1].type), &align);
            vtop->type.t &= ~VT_UNSIGNED;
            gen_op('*');
#ifdef CONFIG_TCC_BCHECK
            if (tcc_state->do_bounds_check && !CONST_WANTED) {

                if (op == '-') {
                    vpushi(0);
                    vswap();
                    gen_op('-');
                }
                gen_bounded_ptr_add();
            } else
#endif
            {
                gen_opic(op);
            }
            type1.t &= ~(VT_ARRAY | VT_VLA);

            vtop->type = type1;
        }
    } else {

        if (is_float(combtype.t) && op != '+' && op != '-' && op != '*' && op != '/' &&
            op_class != CMP_OP) {
            goto op_err;
        }
    std_op:
        t = t2 = combtype.t;

        if (op_class == SHIFT_OP)
            t2 = VT_INT;

        if (t & VT_UNSIGNED) {
            if (op == TOK_SAR)
                op = TOK_SHR;
            else if (op == '/')
                op = TOK_UDIV;
            else if (op == '%')
                op = TOK_UMOD;
            else if (op == TOK_LT)
                op = TOK_ULT;
            else if (op == TOK_GT)
                op = TOK_UGT;
            else if (op == TOK_LE)
                op = TOK_ULE;
            else if (op == TOK_GE)
                op = TOK_UGE;
        }
        vswap();
        gen_cast_s(t);
        vswap();
        gen_cast_s(t2);
        if (is_float(t))
            gen_opif(op);
        else
            gen_opic(op);
        if (op_class == CMP_OP) {

            vtop->type.t = VT_INT;
        } else {
            vtop->type.t = t;
        }
    }

    if (vtop->r & VT_LVAL)
        gv(RC_TYPE(vtop->type.t));
}

#if defined TCC_TARGET_ARM64 || defined TCC_TARGET_RISCV64 || defined TCC_TARGET_ARM
#define gen_cvt_itof1 gen_cvt_itof
#else

static void gen_cvt_itof1(int t)
{
    if ((vtop->type.t & (VT_BTYPE | VT_UNSIGNED)) == (VT_LLONG | VT_UNSIGNED)) {

        if (t == VT_FLOAT)
            vpush_helper_func(TOK___floatundisf);
#if LDOUBLE_SIZE != 8
        else if (t == VT_LDOUBLE)
            vpush_helper_func(TOK___floatundixf);
#endif
        else
            vpush_helper_func(TOK___floatundidf);
        vrott(2);
        gfunc_call(1);
        vpushi(0);
        PUT_R_RET(vtop, t);
    } else {
        gen_cvt_itof(t);
    }
}
#endif

#if defined TCC_TARGET_ARM64 || defined TCC_TARGET_RISCV64
#define gen_cvt_ftoi1 gen_cvt_ftoi
#else

static void gen_cvt_ftoi1(int t)
{
    int st;
    if (t == (VT_LLONG | VT_UNSIGNED)) {

        st = vtop->type.t & VT_BTYPE;
        if (st == VT_FLOAT)
            vpush_helper_func(TOK___fixunssfdi);
#if LDOUBLE_SIZE != 8
        else if (st == VT_LDOUBLE)
            vpush_helper_func(TOK___fixunsxfdi);
#endif
        else
            vpush_helper_func(TOK___fixunsdfdi);
        vrott(2);
        gfunc_call(1);
        vpushi(0);
        PUT_R_RET(vtop, t);
    } else {
        gen_cvt_ftoi(t);
    }
}
#endif

static void force_charshort_cast(void)
{
    int sbt = BFGET(vtop->r, VT_MUSTCAST) == 2 ? VT_LLONG : VT_INT;
    int dbt = vtop->type.t;
    vtop->r &= ~VT_MUSTCAST;
    vtop->type.t = sbt;
    gen_cast_s(dbt == VT_BOOL ? VT_BYTE | VT_UNSIGNED : dbt);
    vtop->type.t = dbt;
}

static void gen_cast_s(int t)
{
    CType type;
    type.t = t;
    type.ref = NULL;
    gen_cast(&type);
}

static void gen_cast(CType *type)
{
    int sbt, dbt, sf, df, c;
    int dbt_bt, sbt_bt, ds, ss, bits, trunc;

    if (vtop->r & VT_MUSTCAST)
        force_charshort_cast();

    if (vtop->type.t & VT_BITFIELD)
        gv(RC_INT);

    if (IS_ENUM(type->t) && type->ref->c < 0)
        tcc_error("cast to incomplete type");

    dbt = type->t & (VT_BTYPE | VT_UNSIGNED);
    sbt = vtop->type.t & (VT_BTYPE | VT_UNSIGNED);
    if (sbt == VT_FUNC)
        sbt = VT_PTR;

again:
    if (sbt != dbt) {
        sf = is_float(sbt);
        df = is_float(dbt);
        dbt_bt = dbt & VT_BTYPE;
        sbt_bt = sbt & VT_BTYPE;
        if (dbt_bt == VT_VOID) {

            vpop(), vpushi(0);
            goto done;
        }
        if (sbt_bt == VT_VOID) {
        error:
            cast_error(&vtop->type, type);
        }

        c = (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
        if (c) {

            if (sbt == VT_FLOAT)
                vtop->c.ld = vtop->c.f;
            else if (sbt == VT_DOUBLE)
                vtop->c.ld = vtop->c.d;

            if (df) {
                if (sbt_bt == VT_LLONG) {
                    if ((sbt & VT_UNSIGNED) || !(vtop->c.i >> 63))
                        vtop->c.ld = vtop->c.i;
                    else
                        vtop->c.ld = -(long double)-vtop->c.i;
                } else if (!sf) {
                    if ((sbt & VT_UNSIGNED) || !(vtop->c.i >> 31))
                        vtop->c.ld = (uint32_t)vtop->c.i;
                    else
                        vtop->c.ld = -(long double)-(uint32_t)vtop->c.i;
                }

                if (dbt == VT_FLOAT)
                    vtop->c.f = (float)vtop->c.ld;
                else if (dbt == VT_DOUBLE)
                    vtop->c.d = (double)vtop->c.ld;
            } else if (sf && dbt == VT_BOOL) {
                vtop->c.i = (vtop->c.ld != 0);
            } else {
                if (sf) {
                    if (dbt & VT_UNSIGNED)
                        vtop->c.i = (uint64_t)vtop->c.ld;
                    else
                        vtop->c.i = (int64_t)vtop->c.ld;
                } else if (sbt_bt == VT_LLONG || (PTR_SIZE == 8 && sbt == VT_PTR))
                    ;
                else if (sbt & VT_UNSIGNED)
                    vtop->c.i = (uint32_t)vtop->c.i;
                else
                    vtop->c.i = ((uint32_t)vtop->c.i | -(vtop->c.i & 0x80000000));

                if (dbt_bt == VT_LLONG || (PTR_SIZE == 8 && dbt == VT_PTR))
                    ;
                else if (dbt == VT_BOOL)
                    vtop->c.i = (vtop->c.i != 0);
                else {
                    uint32_t m = dbt_bt == VT_BYTE    ? 0xff
                                 : dbt_bt == VT_SHORT ? 0xffff
                                                      : 0xffffffff;
                    vtop->c.i &= m;
                    if (!(dbt & VT_UNSIGNED))
                        vtop->c.i |= -(vtop->c.i & ((m >> 1) + 1));
                }
            }
            goto done;

        } else if (dbt == VT_BOOL &&
                   (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_CONST | VT_SYM)) {

            vtop->r = VT_CONST;
            vtop->c.i = 1;
            goto done;
        }

        if (nocode_wanted & DATA_ONLY_WANTED) {
            if (df)
                vtop->r = get_reg(RC_FLOAT);
            goto done;
        }

        if (dbt == VT_BOOL) {
            gen_test_zero(TOK_NE);
            goto done;
        }

        if (sf || df) {
            if (sf && df) {

                gen_cvt_ftof(dbt);
            } else if (df) {

                gen_cvt_itof1(dbt);
            } else {

                sbt = dbt;
                if (dbt_bt != VT_LLONG && dbt_bt != VT_INT)
                    sbt = VT_INT;
                gen_cvt_ftoi1(sbt);
                goto again;
            }
            goto done;
        }

        ds = btype_size(dbt_bt);
        ss = btype_size(sbt_bt);
        if (ds == 0 || ss == 0)
            goto error;

        if (ds == ss && ds >= 4)
            goto done;
        if (dbt_bt == VT_PTR || sbt_bt == VT_PTR) {
            tcc_warning("cast between pointer and integer of different size");
            if (sbt_bt == VT_PTR) {

                vtop->type.t = (PTR_SIZE == 8 ? VT_LLONG : VT_INT);
            }
        }

#define ALLOW_SUBTYPE_ACCESS 1

        if (ALLOW_SUBTYPE_ACCESS && (vtop->r & VT_LVAL)) {

            if (ds <= ss)
                goto done;

            if (ds <= 4 && !(dbt == (VT_SHORT | VT_UNSIGNED) && sbt == VT_BYTE)) {
                gv(RC_INT);
                goto done;
            }
        }
        gv(RC_INT);

        trunc = 0;
#if PTR_SIZE == 4
        if (ds == 8) {

            if (sbt & VT_UNSIGNED) {
                vpushi(0);
                gv(RC_INT);
            } else {
                gv_dup();
                vpushi(31);
                gen_op(TOK_SAR);
            }
            lbuild(dbt);
        } else if (ss == 8) {

            lexpand();
            vpop();
        }
        ss = 4;

#elif PTR_SIZE == 8
        if (ds == 8) {

            if (sbt & VT_UNSIGNED) {
#if defined(TCC_TARGET_RISCV64)

                trunc = 32;
#else
                goto done;
#endif
            } else {
                gen_cvt_sxtw();
                goto done;
            }
            ss = ds, ds = 4, dbt = sbt;
        } else if (ss == 8) {

#if !defined(TCC_TARGET_RISCV64)
            trunc = 32;
#endif
        } else {
            ss = 4;
        }
#endif

        if (ds >= ss)
            goto done;
#if defined TCC_TARGET_I386 || defined TCC_TARGET_X86_64 || defined TCC_TARGET_ARM64 ||            \
    defined TCC_TARGET_RISCV64
        if (ss == 4) {
            gen_cvt_csti(dbt);
            goto done;
        }
#endif
        bits = (ss - ds) * 8;

        vtop->type.t = (ss == 8 ? VT_LLONG : VT_INT) | (dbt & VT_UNSIGNED);
        vpushi(bits);
        gen_op(TOK_SHL);
        vpushi(bits - trunc);
        gen_op(TOK_SAR);
        vpushi(trunc);
        gen_op(TOK_SHR);
    }
done:
    vtop->type = *type;
    vtop->type.t &= ~(VT_QUAL | VT_ARRAY | VT_TLS);
}

ST_FUNC int type_size(CType *type, int *a)
{
    Sym *s;
    int bt;

    bt = type->t & VT_BTYPE;
    if (bt == VT_STRUCT) {

        s = type->ref;
        *a = s->r;
        return s->c;
    } else if (bt == VT_PTR) {
        if (type->t & VT_ARRAY) {
            int ts;
            s = type->ref;
            ts = type_size(&s->type, a);
            if (s->c < 0)
                return s->c;
            return ts * s->c;
        } else {
            *a = PTR_SIZE;
            return PTR_SIZE;
        }
    } else if (IS_ENUM(type->t) && type->ref->c < 0) {
        *a = 0;
        return -1;
    } else if (bt == VT_LDOUBLE) {
        *a = LDOUBLE_ALIGN;
        return LDOUBLE_SIZE;
    } else if (bt == VT_DOUBLE || bt == VT_LLONG) {
#if (defined TCC_TARGET_I386 && !defined TCC_TARGET_PE) ||                                         \
    (defined TCC_TARGET_ARM && !defined TCC_ARM_EABI)
        *a = 4;
#else
        *a = 8;
#endif
        return 8;
    } else if (bt == VT_INT || bt == VT_FLOAT) {
        *a = 4;
        return 4;
    } else if (bt == VT_SHORT) {
        *a = 2;
        return 2;
    } else if (bt == VT_QLONG || bt == VT_QFLOAT) {
        *a = 8;
        return 16;
    } else {

        *a = 1;
        return 1;
    }
}

static void vpush_type_size(CType *type, int *a)
{
    if (type->t & VT_VLA) {
        type_size(&type->ref->type, a);
        vset(&int_type, VT_LOCAL | VT_LVAL, type->ref->c);
    } else {
        int size = type_size(type, a);
        if (size < 0)
            tcc_error("unknown type size");
        vpushs(size);
    }
}

static inline CType *pointed_type(CType *type)
{
    return &type->ref->type;
}

static void check_restrict_type(CType *type)
{
    while ((type->t & VT_BTYPE) == VT_PTR) {
        if ((type->t & VT_RESTRICT) && (pointed_type(type)->t & VT_BTYPE) == VT_FUNC)
            tcc_error("restrict-qualified type must be a pointer to object or incomplete type");
        type = pointed_type(type);
    }
    if (type->t & VT_RESTRICT)
        tcc_error("restrict-qualified type must be a pointer to object or incomplete type");
}

static int type_qualifiers(CType *type)
{
    while (type->t & VT_ARRAY)
        type = pointed_type(type);
    return type->t & VT_QUAL;
}

ST_FUNC void mk_pointer(CType *type)
{
    Sym *s;
    s = sym_push(SYM_FIELD, type, 0, -1);
    type->t = VT_PTR | (type->t & VT_STORAGE);
    type->ref = s;
}

static int is_compatible_types(CType *type1, CType *type2)
{
    return compare_types(type1, type2, 0);
}

static int is_compatible_unqualified_types(CType *type1, CType *type2)
{
    return compare_types(type1, type2, 1);
}

static void cast_error(CType *st, CType *dt)
{
    type_incompatibility_error(st, dt, "cannot convert '%s' to '%s'");
}

static void verify_assign_cast(CType *dt)
{
    CType *st, *type1, *type2;
    int dbt, sbt, qualwarn, lvl;

    st = &vtop->type;
    dbt = dt->t & VT_BTYPE;
    sbt = st->t & VT_BTYPE;
    if (dt->t & VT_CONSTANT)
        tcc_warning("assignment of read-only location");
    switch (dbt) {
        case VT_VOID:
            if (sbt != dbt)
                tcc_error("assignment to void expression");
            break;
        case VT_PTR:

            if (is_null_pointer(vtop))
                break;

            if (is_integer_btype(sbt)) {
                tcc_warning("assignment makes pointer from integer without a cast");
                break;
            }
            type1 = pointed_type(dt);
            if (sbt == VT_PTR)
                type2 = pointed_type(st);
            else if (sbt == VT_FUNC)
                type2 = st;
            else
                goto error;
            if (is_compatible_types(type1, type2))
                break;
            for (qualwarn = lvl = 0;; ++lvl) {
                if ((type2->t & ~type1->t) & VT_QUAL)
                    qualwarn = 1;
                dbt = type1->t & (VT_BTYPE | VT_LONG);
                sbt = type2->t & (VT_BTYPE | VT_LONG);
                if (dbt != VT_PTR || sbt != VT_PTR)
                    break;
                type1 = pointed_type(type1);
                type2 = pointed_type(type2);
            }
            if (!is_compatible_unqualified_types(type1, type2)) {
                if ((dbt == VT_VOID || sbt == VT_VOID) && lvl == 0) {

                } else if (dbt == sbt && is_integer_btype(sbt & VT_BTYPE) &&
                           IS_ENUM(type1->t) + IS_ENUM(type2->t) +
                                   !!((type1->t ^ type2->t) & VT_UNSIGNED) <
                               2) {

                } else {
                    tcc_warning("assignment from incompatible pointer type");
                    break;
                }
            }
            if (qualwarn)
                tcc_warning_c(warn_discarded_qualifiers)(
                    "assignment discards qualifiers from pointer target type");
            break;
        case VT_BYTE:
        case VT_SHORT:
        case VT_INT:
        case VT_LLONG:
            if (sbt == VT_PTR || sbt == VT_FUNC) {
                tcc_warning("assignment makes integer from pointer without a cast");
            } else if (sbt == VT_STRUCT) {
                goto case_VT_STRUCT;
            }

            break;
        case VT_STRUCT:
        case_VT_STRUCT:
            if (!is_compatible_unqualified_types(dt, st)) {
            error:
                cast_error(st, dt);
            }
            break;
    }
}

static void gen_assign_cast(CType *dt)
{
    verify_assign_cast(dt);
    gen_cast(dt);
}

ST_FUNC void vstore(void)
{
    int sbt, dbt, ft, r, size, align, bit_size, bit_pos, delayed_cast;

    ft = vtop[-1].type.t;
    sbt = vtop->type.t & VT_BTYPE;
    dbt = ft & VT_BTYPE;
    verify_assign_cast(&vtop[-1].type);

    if (sbt == VT_STRUCT) {

        size = type_size(&vtop->type, &align);

        vpushv(vtop - 1);
#ifdef CONFIG_TCC_BCHECK
        if (vtop->r & VT_MUSTBOUND)
            gbound();
#endif
        vtop->type.t = VT_PTR;
        gaddrof();

        vswap();
#ifdef CONFIG_TCC_BCHECK
        if (vtop->r & VT_MUSTBOUND)
            gbound();
#endif
        vtop->type.t = VT_PTR;
        gaddrof();

#ifdef TCC_TARGET_NATIVE_STRUCT_COPY
        if (1
#ifdef CONFIG_TCC_BCHECK
            && !tcc_state->do_bounds_check
#endif
        ) {
            gen_struct_copy(size);
        } else
#endif
        {

            vpushi(size);

#ifdef TCC_ARM_EABI
            if (!(align & 7))
                vpush_helper_func(TOK_memmove8);
            else if (!(align & 3))
                vpush_helper_func(TOK_memmove4);
            else
#endif
                vpush_helper_func(TOK_memmove);
            vrott(4);
            gfunc_call(3);
        }

    } else if (ft & VT_BITFIELD) {

        vdup(), vtop[-1] = vtop[-2];

        bit_pos = BIT_POS(ft);
        bit_size = BIT_SIZE(ft);

        vtop[-1].type.t = ft & ~VT_STRUCT_MASK;

        if (dbt == VT_BOOL) {
            gen_cast(&vtop[-1].type);
            vtop[-1].type.t = (vtop[-1].type.t & ~VT_BTYPE) | (VT_BYTE | VT_UNSIGNED);
        }
        r = adjust_bf(vtop - 1, bit_pos, bit_size);
        if (dbt != VT_BOOL) {
            gen_cast(&vtop[-1].type);
            dbt = vtop[-1].type.t & VT_BTYPE;
        }
        if (r == VT_STRUCT) {
            store_packed_bf(bit_pos, bit_size);
        } else {
            unsigned long long mask = (1ULL << bit_size) - 1;
            if (dbt != VT_BOOL) {

                if (dbt == VT_LLONG)
                    vpushll(mask);
                else
                    vpushi((unsigned)mask);
                gen_op('&');
            }

            vpushi(bit_pos);
            gen_op(TOK_SHL);
            vswap();

            vdup();
            vrott(3);

            if (dbt == VT_LLONG)
                vpushll(~(mask << bit_pos));
            else
                vpushi(~((unsigned)mask << bit_pos));
            gen_op('&');
            gen_op('|');

            vstore();

            vpop();
        }
    } else if (dbt == VT_VOID) {
        --vtop;
    } else {

        delayed_cast = 0;
        if ((dbt == VT_BYTE || dbt == VT_SHORT) && is_integer_btype(sbt)) {
            if ((vtop->r & VT_MUSTCAST) && btype_size(dbt) > btype_size(sbt))
                force_charshort_cast();
            delayed_cast = 1;
        } else {
            gen_cast(&vtop[-1].type);
        }

#ifdef CONFIG_TCC_BCHECK

        if (vtop[-1].r & VT_MUSTBOUND) {
            vswap();
            gbound();
            vswap();
        }
#endif
        gv(RC_TYPE(dbt));

        if (delayed_cast) {
            vtop->r |= BFVAL(VT_MUSTCAST, (sbt == VT_LLONG) + 1);

            vtop->type.t = ft & VT_TYPE;
        }

        if ((vtop[-1].r & VT_VALMASK) == VT_LLOCAL) {
            SValue sv;
            r = get_reg(RC_INT);
            sv.type.t = VT_PTRDIFF_T;
            sv.r = VT_LOCAL | VT_LVAL;
            sv.c.i = vtop[-1].c.i;
            sv.sym = NULL;
            load(r, &sv);
            vtop[-1].r = r | VT_LVAL;
        }

        r = vtop->r & VT_VALMASK;

        if (USING_TWO_WORDS(dbt)) {
            int load_type = (dbt == VT_QFLOAT) ? VT_DOUBLE : VT_PTRDIFF_T;
            vtop[-1].type.t = load_type;
            store(r, vtop - 1);
            vswap();
            incr_offset(PTR_SIZE);
            vswap();

            store(vtop->r2, vtop - 1);
        } else {

            store(r, vtop - 1);
        }
        vswap();
        vtop--;
    }
}

ST_FUNC void inc(int post, int c)
{
    test_lvalue();
    vdup();
    if (post) {
        gv_dup();
        vrotb(3);
        vrotb(3);
    }

    vpushi(c - TOK_MID);
    gen_op('+');
    vstore();
    if (post)
        vpop();
}

ST_FUNC CString *parse_mult_str(const char *msg)
{

    if (tok != TOK_STR)
        expect(msg);
    cstr_reset(&initstr);
    while (tok == TOK_STR) {

        cstr_cat(&initstr, tokc.str.data, -1);
        next();
    }
    cstr_ccat(&initstr, '\0');
    return &initstr;
}

ST_FUNC int exact_log2p1(int i)
{
    int ret;
    if (!i)
        return 0;
    for (ret = 1; i >= 1 << 8; ret += 8)
        i >>= 8;
    if (i >= 1 << 4)
        ret += 4, i >>= 4;
    if (i >= 1 << 2)
        ret += 2, i >>= 2;
    if (i >= 1 << 1)
        ret++;
    return ret;
}

static void parse_attribute(AttributeDef *ad)
{
    int t, n;
    char *astr;
    AttributeDef ad_tmp;

redo:
    if (tok != TOK_ATTRIBUTE1 && tok != TOK_ATTRIBUTE2)
        return;
    if (NULL == ad)
        ad = &ad_tmp;

    next();
    skip('(');
    skip('(');
    while (tok != ')') {
        if (tok < TOK_IDENT)
            expect("attribute name");
        t = tok;
        next();
        switch (t) {
            case TOK_CLEANUP1:
            case TOK_CLEANUP2: {
                Sym *s;

                skip('(');
                s = sym_find(tok);
                if (!s) {
                    tcc_warning_c(warn_implicit_function_declaration)(
                        "implicit declaration of function '%s'", get_tok_str(tok, &tokc));
                    s = external_global_sym(tok, &func_old_type);
                } else if ((s->type.t & VT_BTYPE) != VT_FUNC)
                    tcc_error("'%s' is not declared as function", get_tok_str(tok, &tokc));
                ad->cleanup_func = s;
                next();
                skip(')');
                break;
            }
            case TOK_CONSTRUCTOR1:
            case TOK_CONSTRUCTOR2:
                ad->f.func_ctor = 1;
                break;
            case TOK_DESTRUCTOR1:
            case TOK_DESTRUCTOR2:
                ad->f.func_dtor = 1;
                break;
            case TOK_ALWAYS_INLINE1:
            case TOK_ALWAYS_INLINE2:
                ad->f.func_alwinl = 1;
                break;
            case TOK_SECTION1:
            case TOK_SECTION2:
                skip('(');
                astr = parse_mult_str("section name")->data;
                n = tcc_state->nb_sections;
                ad->section = find_section(tcc_state, astr);
                ad->new_section = n < tcc_state->nb_sections;
                skip(')');
                break;
            case TOK_ALIAS1:
            case TOK_ALIAS2:
                skip('(');
                astr = parse_mult_str("alias(\"target\")")->data;

                ad->alias_target = tok_alloc_const(astr);
                skip(')');
                break;
            case TOK_VISIBILITY1:
            case TOK_VISIBILITY2:
                skip('(');
                astr = parse_mult_str("visibility(\"default|hidden|internal|protected\")")->data;
                if (!strcmp(astr, "default"))
                    ad->a.visibility = STV_DEFAULT;
                else if (!strcmp(astr, "hidden"))
                    ad->a.visibility = STV_HIDDEN;
                else if (!strcmp(astr, "internal"))
                    ad->a.visibility = STV_INTERNAL;
                else if (!strcmp(astr, "protected"))
                    ad->a.visibility = STV_PROTECTED;
                else
                    expect("visibility(\"default|hidden|internal|protected\")");
                skip(')');
                break;
            case TOK_ALIGNED1:
            case TOK_ALIGNED2:
                if (tok == '(') {
                    next();
                    n = expr_const();
                    if (n <= 0 || (n & (n - 1)) != 0)
                        tcc_error("alignment must be a positive power of two");
                    skip(')');
                } else {
                    n = MAX_ALIGN;
                }
                ad->a.aligned = exact_log2p1(n);
                if (n != 1 << (ad->a.aligned - 1))
                    tcc_error("alignment of %d is larger than implemented", n);
                break;
            case TOK_PACKED1:
            case TOK_PACKED2:
                ad->a.packed = 1;
                break;
            case TOK_WEAK1:
            case TOK_WEAK2:
                ad->a.weak = 1;
                break;
            case TOK_NODEBUG1:
            case TOK_NODEBUG2:
                ad->a.nodebug = 1;
                break;
            case TOK_USED1:
            case TOK_USED2:
            case TOK_UNUSED1:
            case TOK_UNUSED2:

                break;
            case TOK_CONST1:
            case TOK_CONST2:
            case TOK_CONST3:
            case TOK_PURE1:
            case TOK_PURE2:

                break;
            case TOK_NOINLINE:

                break;
            case TOK_FORMAT1:
            case TOK_FORMAT2:

                goto skip_param;
            case TOK_NORETURN1:
            case TOK_NORETURN2:
                ad->f.func_noreturn = 1;
                break;
            case TOK_CDECL1:
            case TOK_CDECL2:
            case TOK_CDECL3:
                ad->f.func_call = FUNC_CDECL;
                break;
            case TOK_STDCALL1:
            case TOK_STDCALL2:
            case TOK_STDCALL3:
                ad->f.func_call = FUNC_STDCALL;
                break;
#ifdef TCC_TARGET_I386
            case TOK_REGPARM1:
            case TOK_REGPARM2:
                skip('(');
                n = expr_const();
                if (n > 3)
                    n = 3;
                else if (n < 0)
                    n = 0;
                if (n > 0)
                    ad->f.func_call = FUNC_FASTCALL1 + n - 1;
                skip(')');
                break;
            case TOK_FASTCALL1:
            case TOK_FASTCALL2:
            case TOK_FASTCALL3:
                ad->f.func_call = FUNC_FASTCALLW;
                break;
            case TOK_THISCALL1:
            case TOK_THISCALL2:
            case TOK_THISCALL3:
                ad->f.func_call = FUNC_THISCALL;
                break;
#endif
            case TOK_MODE:
                skip('(');
                switch (tok) {
                    case TOK_MODE_DI:
                        ad->attr_mode = VT_LLONG + 1;
                        break;
                    case TOK_MODE_QI:
                        ad->attr_mode = VT_BYTE + 1;
                        break;
                    case TOK_MODE_HI:
                        ad->attr_mode = VT_SHORT + 1;
                        break;
                    case TOK_MODE_SI:
                    case TOK_MODE_word:
                        ad->attr_mode = VT_INT + 1;
                        break;
                    default:
                        tcc_warning("__mode__(%s) not supported\n", get_tok_str(tok, NULL));
                        break;
                }
                next();
                skip(')');
                break;
            case TOK_DLLEXPORT:
                ad->a.dllexport = 1;
                break;
            case TOK_NODECORATE:
                ad->a.nodecorate = 1;
                break;
            case TOK_DLLIMPORT:
                ad->a.dllimport = 1;
                break;
            default:
                tcc_warning_c(warn_unsupported)("'%s' attribute ignored", get_tok_str(t, NULL));

            skip_param:
                if (tok == '(') {
                    int parenthesis = 0;
                    do {
                        if (tok == '(')
                            parenthesis++;
                        else if (tok == ')')
                            parenthesis--;
                        next();
                    } while (parenthesis && tok != -1);
                }
                break;
        }
        if (tok != ',')
            break;
        next();
    }
    skip(')');
    skip(')');
    goto redo;
}

static Sym *find_field(CType *type, int v, int *cumofs)
{
    Sym *s = type->ref;
    int v1 = v | SYM_FIELD;
    if (!(v & SYM_FIELD)) {
        if ((type->t & VT_BTYPE) != VT_STRUCT)
            expect("struct or union");
        if (v < TOK_UIDENT)
            expect("field name");
        if (s->c < 0)
            tcc_error("dereferencing incomplete type '%s'", get_tok_str(s->v & ~SYM_STRUCT, 0));
    }
    while ((s = s->next) != NULL) {
        if (s->v == v1) {
            *cumofs = s->c;
            return s;
        }
        if ((s->type.t & VT_BTYPE) == VT_STRUCT && s->v >= (SYM_FIRST_ANOM | SYM_FIELD)) {

            Sym *ret = find_field(&s->type, v1, cumofs);
            if (ret) {
                *cumofs += s->c;
                return ret;
            }
        }
    }
    if (!(v & SYM_FIELD))
        tcc_error("field not found: %s", get_tok_str(v, NULL));
    return s;
}

static void check_fields(CType *type, int check)
{
    Sym *s = type->ref;

    while ((s = s->next) != NULL) {
        int v = s->v & ~SYM_FIELD;
        if (v < SYM_FIRST_ANOM) {
            TokenSym *ts = table_ident[v - TOK_IDENT];
            if (check && (ts->tok & SYM_FIELD))
                tcc_error("duplicate member '%s'", get_tok_str(v, NULL));
            ts->tok ^= SYM_FIELD;
        } else if ((s->type.t & VT_BTYPE) == VT_STRUCT)
            check_fields(&s->type, check);
    }
}

static void struct_layout(CType *type, AttributeDef *ad)
{
    int size, align, maxalign, offset, c, bit_pos, bit_size;
    int packed, a, bt, prevbt, prev_bit_size;
    int pcc = !tcc_state->ms_bitfields;
    int pragma_pack = *tcc_state->pack_stack_ptr;
    Sym *f;

    maxalign = 1;
    offset = 0;
    c = 0;
    bit_pos = 0;
    prevbt = VT_STRUCT;
    prev_bit_size = 0;

    for (f = type->ref->next; f; f = f->next) {
        if (f->type.t & VT_BITFIELD)
            bit_size = BIT_SIZE(f->type.t);
        else
            bit_size = -1;
        size = type_size(&f->type, &align);
        a = f->a.aligned ? 1 << (f->a.aligned - 1) : 0;
        packed = 0;

        if (pcc && bit_size == 0) {

        } else {

            if (pcc && (f->a.packed || ad->a.packed))
                align = packed = 1;

            if (pragma_pack) {
                packed = 1;
                if (pragma_pack < align)
                    align = pragma_pack;

                if (pcc && pragma_pack < a)
                    a = 0;
            }
        }

        if (a)
            align = a;

        if (type->ref->type.t == VT_UNION) {
            if (pcc && bit_size >= 0)
                size = (bit_size + 7) >> 3;
            offset = 0;
            if (size > c)
                c = size;

        } else if (bit_size < 0) {
            if (pcc)
                c += (bit_pos + 7) >> 3;
            c = (c + align - 1) & -align;
            offset = c;
            if (size > 0)
                c += size;
            bit_pos = 0;
            prevbt = VT_STRUCT;
            prev_bit_size = 0;

        } else {

            if (pcc) {

                if (bit_size == 0) {
                new_field:
                    c = (c + ((bit_pos + 7) >> 3) + align - 1) & -align;
                    bit_pos = 0;
                } else if (f->a.aligned) {
                    goto new_field;
                } else if (!packed) {
                    int a8 = align * 8;
                    int ofs = ((c * 8 + bit_pos) % a8 + bit_size + a8 - 1) / a8;
                    if (ofs > size / align)
                        goto new_field;
                }

                if (size == 8 && bit_size <= 32)
                    f->type.t = (f->type.t & ~VT_BTYPE) | VT_INT, size = 4;

                while (bit_pos >= align * 8)
                    c += align, bit_pos -= align * 8;
                offset = c;

                if (f->v & SYM_FIRST_ANOM

                )
                    align = 1;

            } else {
                bt = f->type.t & VT_BTYPE;
                if ((bit_pos + bit_size > size * 8) || (bit_size > 0) == (bt != prevbt)) {
                    c = (c + align - 1) & -align;
                    offset = c;
                    bit_pos = 0;

                    if (bit_size || prev_bit_size)
                        c += size;
                }

                if (bit_size == 0 && prevbt != bt)
                    align = 1;
                prevbt = bt;
                prev_bit_size = bit_size;
            }

            f->type.t = (f->type.t & ~(0x3f << VT_STRUCT_SHIFT)) | (bit_pos << VT_STRUCT_SHIFT);
            bit_pos += bit_size;
        }
        if (align > maxalign)
            maxalign = align;

#ifdef BF_DEBUG
        printf("set field %s offset %-2d size %-2d align %-2d",
               get_tok_str(f->v & ~SYM_FIELD, NULL), offset, size, align);
        if (f->type.t & VT_BITFIELD) {
            printf(" pos %-2d bits %-2d", BIT_POS(f->type.t), BIT_SIZE(f->type.t));
        }
        printf("\n");
#endif

        f->c = offset;
        f->r = 0;
    }

    if (pcc)
        c += (bit_pos + 7) >> 3;

    a = bt = ad->a.aligned ? 1 << (ad->a.aligned - 1) : 1;
    if (a < maxalign)
        a = maxalign;
    type->ref->r = a;
    if (pragma_pack && pragma_pack < maxalign && 0 == pcc) {

        a = pragma_pack;
        if (a < bt)
            a = bt;
    }
    c = (c + a - 1) & -a;
    type->ref->c = c;

#ifdef BF_DEBUG
    printf("struct size %-2d align %-2d\n\n", c, a), fflush(stdout);
#endif

    for (f = type->ref->next; f; f = f->next) {
        int s, px, cx, c0;
        CType t;

        if (0 == (f->type.t & VT_BITFIELD))
            continue;
        f->type.ref = f;
        f->auxtype = -1;
        bit_size = BIT_SIZE(f->type.t);
        if (bit_size == 0)
            continue;
        bit_pos = BIT_POS(f->type.t);
        size = type_size(&f->type, &align);

        if (bit_pos + bit_size <= size * 8 && f->c + size <= c
#ifdef TCC_TARGET_ARM
            && !(f->c & (align - 1))
#endif
        )
            continue;

        c0 = -1, s = align = 1;
        t.t = VT_BYTE;
        for (;;) {
            px = f->c * 8 + bit_pos;
            cx = (px >> 3) & -align;
            px = px - (cx << 3);
            if (c0 == cx)
                break;
            s = (px + bit_size + 7) >> 3;
            if (s > 4) {
                t.t = VT_LLONG;
            } else if (s > 2) {
                t.t = VT_INT;
            } else if (s > 1) {
                t.t = VT_SHORT;
            } else {
                t.t = VT_BYTE;
            }
            s = type_size(&t, &align);
            c0 = cx;
        }

        if (px + bit_size <= s * 8 && cx + s <= c
#ifdef TCC_TARGET_ARM
            && !(cx & (align - 1))
#endif
        ) {

            f->c = cx;
            bit_pos = px;
            f->type.t = (f->type.t & ~(0x3f << VT_STRUCT_SHIFT)) | (bit_pos << VT_STRUCT_SHIFT);
            if (s != size)
                f->auxtype = t.t;
#ifdef BF_DEBUG
            printf("FIX field %s offset %-2d size %-2d align %-2d "
                   "pos %-2d bits %-2d\n",
                   get_tok_str(f->v & ~SYM_FIELD, NULL), cx, s, align, px, bit_size);
#endif
        } else {

            f->auxtype = VT_STRUCT;
#ifdef BF_DEBUG
            printf("FIX field %s : load byte-wise\n", get_tok_str(f->v & ~SYM_FIELD, NULL));
#endif
        }
    }
}

static int in_range(long long n, int t)
{
    unsigned long long m = (1ULL << (btype_size(t & VT_BTYPE) * 8 - 1)) - 1;
    if (t & VT_UNSIGNED)
        return n <= (m << 1) + 1;
    return n >= -(long long)m - 1 && n <= (long long)m;
}

static void struct_decl(CType *type, int u)
{
    int v, c, size, align, flexible;
    int bit_size, bsize, bt, ut;
    Sym *s, *ss, **ps;
    AttributeDef ad, ad1;
    CType type1, btype;

    memset(&ad, 0, sizeof ad);
    next();
    parse_attribute(&ad);

    v = 0;
    if (tok >= TOK_IDENT)
        v = tok, next();

    bt = ut = 0;
    if (u == VT_ENUM) {
        ut = VT_INT;
        if (tok == ':') {
            next();
            if (!parse_btype(&btype, &ad1, 0) || !is_integer_btype(btype.t & VT_BTYPE))
                expect("enum type");
            bt = ut = btype.t & (VT_BTYPE | VT_LONG | VT_UNSIGNED | VT_DEFSIGN);
        }
    }

    if (v) {

        s = struct_find(v);
        if (s && (s->sym_scope == local_scope || (tok != '{' && tok != ';'))) {
            if (u == s->type.t)
                goto do_decl;
            if (u == VT_ENUM && IS_ENUM(s->type.t))
                goto do_decl;
            tcc_error("redeclaration of '%s'", get_tok_str(v, NULL));
        }
    } else {
        if (tok != '{')
            expect("struct/union/enum name");
        v = anon_sym++;
    }

    type1.t = u | ut;
    type1.ref = NULL;

    s = sym_push(v | SYM_STRUCT, &type1, 0, bt ? 0 : -1);
    s->r = 0;
do_decl:
    type->t = s->type.t;
    type->ref = s;

    if (tok == '{') {
        next();
        if (s->c != -1 && !(u == VT_ENUM && s->c == 0))
            tcc_error("struct/union/enum already defined");
        s->c = -2;

        ps = &s->next;
        if (u == VT_ENUM) {
            long long ll = 0, pl = 0, nl = 0;
            CType t;
            t.ref = s;
            s->sym_scope = local_scope;

            t.t = VT_INT | VT_STATIC | VT_ENUM_VAL;
            if (bt)
                t.t = bt | VT_STATIC | VT_ENUM_VAL;
            for (;;) {
                v = tok;
                if (v < TOK_UIDENT)
                    expect("identifier");
                next();
                if (tok == '=') {
                    next();
                    ll = expr_const64();
                }
                if (bt && !in_range(ll, t.t))
                    tcc_error("enumerator '%s' out of range of its type", get_tok_str(v, NULL));
                ss = sym_push(v, &t, VT_CONST, 0);
                ss->enum_val = ll;
                *ps = ss, ps = &ss->next;
                if (ll < nl)
                    nl = ll;
                if (ll > pl)
                    pl = ll;
                if (tok != ',')
                    break;
                next();
                ll++;

                if (tok == '}')
                    break;
            }
            skip('}');

            if (bt) {
                t.t = bt;
                s->c = 2;
                goto enum_done;
            }

            t.t = VT_INT;
            if (nl >= 0) {
                if (pl != (unsigned)pl)
                    t.t = (LONG_SIZE == 8 ? VT_LLONG | VT_LONG : VT_LLONG);
                t.t |= VT_UNSIGNED;
            } else if (pl != (int)pl || nl != (int)nl)
                t.t = (LONG_SIZE == 8 ? VT_LLONG | VT_LONG : VT_LLONG);

            for (ss = s->next; ss; ss = ss->next) {
                ll = ss->enum_val;
                if (ll == (int)ll)
                    continue;
                if (t.t & VT_UNSIGNED) {
                    ss->type.t |= VT_UNSIGNED;
                    if (ll == (unsigned)ll)
                        continue;
                }
                ss->type.t =
                    (ss->type.t & ~VT_BTYPE) | (LONG_SIZE == 8 ? VT_LLONG | VT_LONG : VT_LLONG);
            }
            s->c = 1;
        enum_done:
            s->type.t = type->t = t.t | VT_ENUM;

        } else {
            c = 0;
            flexible = 0;
            while (tok != '}') {
                if (!parse_btype(&btype, &ad1, 0)) {
                    if (tok == TOK_STATIC_ASSERT) {
                        do_Static_assert();
                        continue;
                    }
                    skip(';');
                    continue;
                }
                while (1) {
                    if (flexible)
                        tcc_error("flexible array member '%s' not at the end of struct",
                                  get_tok_str(v, NULL));
                    bit_size = -1;
                    v = 0;
                    type1 = btype;
                    if (tok != ':') {
                        if (tok != ';')
                            type_decl(&type1, &ad1, &v, TYPE_DIRECT);
                        if (v == 0) {
                            if ((type1.t & VT_BTYPE) != VT_STRUCT)
                                expect("identifier");
                            else {
                                int v = btype.ref->v;
                                if ((v & ~SYM_STRUCT) < SYM_FIRST_ANOM) {
                                    if (tcc_state->ms_extensions == 0)
                                        expect("identifier");
                                }
                            }
                        }
                        if (type_size(&type1, &align) < 0) {
                            if ((u == VT_STRUCT) && (type1.t & VT_ARRAY) && c)
                                flexible = 1;
                            else
                                tcc_error("field '%s' has incomplete type", get_tok_str(v, NULL));
                        }
                        if ((type1.t & VT_BTYPE) == VT_FUNC || (type1.t & VT_BTYPE) == VT_VOID ||
                            (type1.t & VT_STORAGE))
                            tcc_error("invalid type for '%s'", get_tok_str(v, NULL));
                    }
                    if (tok == ':') {
                        next();
                        bit_size = expr_const();

                        if (bit_size < 0)
                            tcc_error("negative width in bit-field '%s'", get_tok_str(v, NULL));
                        if (v && bit_size == 0)
                            tcc_error("zero width for bit-field '%s'", get_tok_str(v, NULL));
                        parse_attribute(&ad1);
                    }
                    size = type_size(&type1, &align);
                    if (bit_size >= 0) {
                        bt = type1.t & VT_BTYPE;
                        if (bt != VT_INT && bt != VT_BYTE && bt != VT_SHORT && bt != VT_BOOL &&
                            bt != VT_LLONG)
                            tcc_error("bitfields must have scalar type");
                        bsize = size * 8;
                        if (bit_size > bsize) {
                            tcc_error("width of '%s' exceeds its type", get_tok_str(v, NULL));
                        } else if (bit_size == bsize && !*tcc_state->pack_stack_ptr &&
                                   !ad.a.packed && !ad1.a.packed) {

                            ;
                        } else if (bit_size == 64) {
                            tcc_error("field width 64 not implemented");
                        } else {
                            type1.t = (type1.t & ~VT_STRUCT_MASK) | VT_BITFIELD |
                                      ((unsigned)bit_size << (VT_STRUCT_SHIFT + 6));
                        }
                    }
                    if (v != 0 || (type1.t & VT_BTYPE) == VT_STRUCT) {

                        c = 1;
                    }

                    if (v == 0 && ((type1.t & VT_BTYPE) == VT_STRUCT || bit_size >= 0)) {
                        v = anon_sym++;
                    }
                    if (v) {
                        ss = sym_push(v | SYM_FIELD, &type1, 0, 0);
                        ss->a = ad1.a;
                        *ps = ss;
                        ps = &ss->next;
                    }
                    if (tok == ';' || tok == TOK_EOF)
                        break;
                    skip(',');
                }
                skip(';');
            }
            skip('}');
            parse_attribute(&ad);
            if (ad.cleanup_func) {
                tcc_warning("attribute '__cleanup__' ignored on type");
            }
            check_fields(type, 1);
            check_fields(type, 0);
            struct_layout(type, &ad);
        }
        if (debug_modes)
            tcc_debug_fix_forw(tcc_state, type);
    }
}

static void sym_to_attr(AttributeDef *ad, Sym *s)
{
    merge_symattr(&ad->a, &s->a);
    merge_funcattr(&ad->f, &s->f);
}

static void parse_btype_qualify(CType *type, int qualifiers)
{
    while (type->t & VT_ARRAY) {
        type->ref = sym_push(SYM_FIELD, &type->ref->type, 0, type->ref->c);
        type = &type->ref->type;
    }
    if (qualifiers & VT_RESTRICT)
        check_restrict_type(type);
    type->t |= qualifiers;
}

static int parse_btype(CType *type, AttributeDef *ad, int ignore_label)
{
    int t, u, bt, st, type_found, typespec_found, g, n;
    Sym *s;
    CType type1;

    memset(ad, 0, sizeof(AttributeDef));
    type_found = 0;
    typespec_found = 0;
    t = VT_INT;
    bt = st = -1;
    type->ref = NULL;

    while (1) {
        switch (tok) {
            case TOK_EXTENSION:

                next();
                continue;

            case TOK_CHAR:
                u = VT_BYTE;
            basic_type:
                next();
            basic_type1:
                if (u == VT_SHORT || u == VT_LONG) {
                    if (st != -1 || (bt != -1 && bt != VT_INT))
                    tmbt:
                        tcc_error("too many basic types");
                    st = u;
                } else {
                    if (bt != -1 || (st != -1 && u != VT_INT))
                        goto tmbt;
                    if ((t & VT_DEFSIGN) && (u == VT_VOID || u > VT_LLONG))
                        goto tmbt;
                    bt = u;
                }
                if (u != VT_INT)
                    t = (t & ~(VT_BTYPE | VT_LONG)) | u;
                typespec_found = 1;
                break;
            case TOK_VOID:
                u = VT_VOID;
                goto basic_type;
            case TOK_SHORT:
                u = VT_SHORT;
                goto basic_type;
            case TOK_INT:
                u = VT_INT;
                goto basic_type;
            case TOK_ALIGNAS: {
                int n;
                AttributeDef ad1;
                next();
                skip('(');
                memset(&ad1, 0, sizeof(AttributeDef));
                if (parse_btype(&type1, &ad1, 0)) {
                    type_decl(&type1, &ad1, &n, TYPE_ABSTRACT);
                    if (ad1.a.aligned)
                        n = 1 << (ad1.a.aligned - 1);
                    else
                        type_size(&type1, &n);
                } else {
                    n = expr_const();
                    if (n < 0 || (n & (n - 1)) != 0)
                        tcc_error("alignment must be a positive power of two");
                }
                skip(')');
                ad->a.aligned = exact_log2p1(n);
            }
                continue;
            case TOK_LONG:
                if ((t & VT_BTYPE) == VT_DOUBLE) {
                    t = (t & ~(VT_BTYPE | VT_LONG)) | VT_LDOUBLE;
                } else if ((t & (VT_BTYPE | VT_LONG)) == VT_LONG) {
                    t = (t & ~(VT_BTYPE | VT_LONG)) | VT_LLONG;
                } else {
                    u = VT_LONG;
                    goto basic_type;
                }
                next();
                break;
            case TOK_BOOL:
                u = VT_BOOL;
                goto basic_type;
            case TOK_COMPLEX:
                tcc_error("_Complex is not yet supported");
            case TOK_FLOAT:
                u = VT_FLOAT;
                goto basic_type;
            case TOK_DOUBLE:
                if ((t & (VT_BTYPE | VT_LONG)) == VT_LONG) {
                    t = (t & ~(VT_BTYPE | VT_LONG)) | VT_LDOUBLE;
                } else {
                    u = VT_DOUBLE;
                    goto basic_type;
                }
                next();
                break;
            case TOK_ENUM:
                struct_decl(&type1, VT_ENUM);
            basic_type2:
                u = type1.t;
                type->ref = type1.ref;
                goto basic_type1;
            case TOK_STRUCT:
                struct_decl(&type1, VT_STRUCT);
                goto basic_type2;
            case TOK_UNION:
                struct_decl(&type1, VT_UNION);
                goto basic_type2;

            case TOK__Atomic:
                next();
                type->t = t;
                parse_btype_qualify(type, VT_ATOMIC);
                t = type->t;
                if (tok == '(') {
                    parse_expr_type(&type1);

                    type1.t &= ~(VT_STORAGE & ~VT_TYPEDEF);
                    if (type1.ref)
                        sym_to_attr(ad, type1.ref);
                    goto basic_type2;
                }
                break;
            case TOK_CONST1:
            case TOK_CONST2:
            case TOK_CONST3:
                type->t = t;
                parse_btype_qualify(type, VT_CONSTANT);
                t = type->t;
                next();
                break;
            case TOK_VOLATILE1:
            case TOK_VOLATILE2:
            case TOK_VOLATILE3:
                type->t = t;
                parse_btype_qualify(type, VT_VOLATILE);
                t = type->t;
                next();
                break;
            case TOK_SIGNED1:
            case TOK_SIGNED2:
            case TOK_SIGNED3:
                if ((t & (VT_DEFSIGN | VT_UNSIGNED)) == (VT_DEFSIGN | VT_UNSIGNED))
                    tcc_error("signed and unsigned modifier");
                t |= VT_DEFSIGN;
                next();
                typespec_found = 1;
                break;
            case TOK_RESTRICT1:
            case TOK_RESTRICT2:
            case TOK_RESTRICT3:
                t |= VT_RESTRICT;
                next();
                break;
            case TOK_REGISTER:
            case TOK_AUTO:
                next();
                break;
            case TOK_UNSIGNED:
                if ((t & (VT_DEFSIGN | VT_UNSIGNED)) == VT_DEFSIGN)
                    tcc_error("signed and unsigned modifier");
                t |= VT_DEFSIGN | VT_UNSIGNED;
                next();
                typespec_found = 1;
                break;

            case TOK_EXTERN:
                g = VT_EXTERN;
                goto storage;
            case TOK_STATIC:
                g = VT_STATIC;
                goto storage;
            case TOK_TYPEDEF:
                g = VT_TYPEDEF;
                goto storage;
            storage:
                if (t & (VT_EXTERN | VT_STATIC | VT_TYPEDEF) & ~g)
                    tcc_error("multiple storage classes");
                t |= g;
                next();
                break;
            case TOK_INLINE1:
            case TOK_INLINE2:
            case TOK_INLINE3:
                t |= VT_INLINE;
                next();
                break;
            case TOK_NORETURN3:
                next();
                ad->f.func_noreturn = 1;
                break;

            case TOK_ATTRIBUTE1:
            case TOK_ATTRIBUTE2:
                parse_attribute(ad);
                if (ad->attr_mode) {
                    u = ad->attr_mode - 1;
                    t = (t & ~(VT_BTYPE | VT_LONG)) | u;
                }
                continue;

            case TOK_TYPEOF1:
            case TOK_TYPEOF2:
            case TOK_TYPEOF3:
                next();
                parse_expr_type(&type1);

                type1.t &= ~(VT_STORAGE & ~VT_TYPEDEF);
                if (type1.ref) {
                    sym_to_attr(ad, type1.ref);
                    if (type1.t & VT_ARRAY)
                        type1.t |= VT_BT_ARRAY;
                }
                goto basic_type2;
            case TOK_THREAD_LOCAL:
            case TOK___thread:
                if (t & VT_TLS)
                    tcc_error("multiple thread-local storage specifiers");
                t |= VT_TLS;
                next();
                break;
            default:
                if (typespec_found)
                    goto the_end;
                s = sym_find(tok);
                if (!s || !(s->type.t & VT_TYPEDEF))
                    goto the_end;

                n = tok, next();
                if (tok == ':' && ignore_label) {

                    unget_tok(n);
                    goto the_end;
                }

                t &= ~(VT_BTYPE | VT_LONG);
                u = t & ~VT_QUAL, t ^= u;
                type->t = (s->type.t & ~VT_TYPEDEF) | u;
                type->ref = s->type.ref;
                if (t)
                    parse_btype_qualify(type, t);
                t = type->t;
                if (t & VT_ARRAY)
                    t |= VT_BT_ARRAY;

                sym_to_attr(ad, s);
                typespec_found = 1;
                st = bt = -2;
                break;
        }
        type_found = 1;
    }
the_end:
    if (tcc_state->char_is_unsigned) {
        if ((t & (VT_DEFSIGN | VT_BTYPE)) == VT_BYTE)
            t |= VT_UNSIGNED;
    }

    bt = t & (VT_BTYPE | VT_LONG);
    if (bt == VT_LONG)
        t |= LONG_SIZE == 8 ? VT_LLONG : VT_INT;
#ifdef TCC_USING_DOUBLE_FOR_LDOUBLE
    if (bt == VT_LDOUBLE)
        t = (t & ~(VT_BTYPE | VT_LONG)) | (VT_DOUBLE | VT_LONG);
#endif
    type->t = t;
    check_restrict_type(type);
    return type_found;
}

static inline void convert_parameter_type(CType *pt)
{

    pt->t &= ~(VT_ARRAY | VT_VLA);
    if ((pt->t & VT_BTYPE) == VT_FUNC) {
        mk_pointer(pt);
    }
}

static inline void convert_expression_type(CType *pt)
{
    pt->t &= ~VT_QUAL;
    convert_parameter_type(pt);
}

ST_FUNC CString *parse_asm_str(void)
{
    skip('(');
    return parse_mult_str("string constant");
}

static int asm_label_instr(void)
{
    int v;
    char *astr;

    next();
    astr = parse_asm_str()->data;
    skip(')');
#ifdef ASM_DEBUG
    printf("asm_alias: \"%s\"\n", astr);
#endif
    v = tok_alloc_const(astr);
    return v;
}

static int post_type(CType *type, AttributeDef *ad, int storage, int td)
{
    int n, l, t1, arg_size, align;
    Sym **plast, *s, *first, **ps, *sr;
    AttributeDef ad1;
    CType pt;
    TokenString *vla_array_tok = NULL;
    int *vla_array_str = NULL;

    if (tok == '(') {

        next();
        if (TYPE_DIRECT == (td & (TYPE_DIRECT | TYPE_ABSTRACT)))
            return 0;

        ps = local_stack ? &local_stack : &global_stack;
        ++local_scope;
        sr = sym_push2(ps, SYM_FIELD, 0, 0);

        if (tok == ')')
            l = 0;
        else if (parse_btype(&pt, &ad1, 0))
            l = FUNC_NEW;
        else if (td & (TYPE_DIRECT | TYPE_ABSTRACT)) {
            sym_pop(ps, sr->prev, 0);
            --local_scope;
            merge_attr(ad, &ad1);
            return 0;
        } else
            l = FUNC_OLD;

        first = NULL;
        plast = &first;
        arg_size = 0;
        if (l) {
            for (;;) {

                if (l != FUNC_OLD) {
                    if ((pt.t & VT_BTYPE) == VT_VOID && tok == ')')
                        break;
                    type_decl(&pt, &ad1, &n, TYPE_DIRECT | TYPE_ABSTRACT | TYPE_PARAM);
                    if ((pt.t & VT_BTYPE) == VT_VOID)
                        tcc_error("parameter declared as void");
                    if (n == 0 || (td & TYPE_PARAM))
                        n |= SYM_FIELD;
                } else {
                    n = tok;
                    pt.t = VT_INT | VT_EXTERN;
                    pt.ref = NULL;
                    next();
                }
                if (n < TOK_UIDENT)
                    expect("identifier");
                convert_parameter_type(&pt);
                arg_size += (type_size(&pt, &align) + PTR_SIZE - 1) / PTR_SIZE;

                s = sym_push(n, &pt, VT_LOCAL | VT_LVAL, 0);
                *plast = s;
                plast = &s->next;
                if (tok == ')')
                    break;
                skip(',');
                if (l == FUNC_NEW && tok == TOK_DOTS) {
                    l = FUNC_ELLIPSIS;
                    next();
                    break;
                }
                if (l == FUNC_NEW && !parse_btype(&pt, &ad1, 0))
                    tcc_error("invalid type");
            }
        } else

            l = FUNC_OLD;
        skip(')');

        type->t &= ~VT_QUAL;

        if (tok == '[') {
            next();
            skip(']');
            mk_pointer(type);
        }
        ad->f.func_args = arg_size;
        ad->f.func_type = l;
        sr->type = *type, s = sr;
        s->a = ad->a;
        s->f = ad->f;
        s->next = first;
        type->t = VT_FUNC;
        type->ref = s;

        sym_pop(ps, sr, 1);
        --local_scope;

    } else if (tok == '[') {
        int saved_nocode_wanted = nocode_wanted;
        int array_qualifiers = 0;

        next();
        n = -1;
        t1 = 0;
        if (td & TYPE_PARAM)
            while (1) {

                switch (tok) {
                    case TOK_CONST1:
                    case TOK_CONST2:
                    case TOK_CONST3:
                        array_qualifiers |= VT_CONSTANT;
                        next();
                        continue;
                    case TOK_VOLATILE1:
                    case TOK_VOLATILE2:
                    case TOK_VOLATILE3:
                        array_qualifiers |= VT_VOLATILE;
                        next();
                        continue;
                    case TOK_RESTRICT1:
                    case TOK_RESTRICT2:
                    case TOK_RESTRICT3:
                        array_qualifiers |= VT_RESTRICT;
                        next();
                        continue;
                    case TOK_STATIC:
                    case '*':
                        next();
                        continue;
                    default:
                        break;
                }
                if (tok != ']') {

                    nocode_wanted = 1;
                    skip_or_save_block(&vla_array_tok);
                    unget_tok(0);
                    vla_array_str = vla_array_tok->str;
                    begin_macro(vla_array_tok, 2);
                    next();
                    gexpr();
                    end_macro();
                    next();
                    goto check;
                }
                break;
            }
        else if (tok != ']') {
            if (!local_stack || (storage & VT_STATIC))
                vpushi(expr_const());
            else {

                nocode_wanted = 0;
                gexpr();
            }
        check:
            if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
                n = vtop->c.i;
                if (n < 0)
                    tcc_error("invalid array size");
            } else {
                if (!is_integer_btype(vtop->type.t & VT_BTYPE))
                    tcc_error("size of variable length array should be an integer");
                n = 0;
                t1 = VT_VLA;
            }
        }
        skip(']');

        post_type(type, ad, storage, (td & ~(TYPE_DIRECT | TYPE_ABSTRACT)) | TYPE_NEST);

        if ((type->t & VT_BTYPE) == VT_FUNC)
            tcc_error("declaration of an array of functions");
        if ((type->t & VT_BTYPE) == VT_VOID || type_size(type, &align) < 0)
            tcc_error("declaration of an array of incomplete type elements");

        t1 |= type->t & VT_VLA;

        if (t1 & VT_VLA) {
            if (n < 0) {
                if (td & TYPE_NEST)
                    tcc_error("need explicit inner array size in VLAs");
            } else {
                loc -= type_size(&int_type, &align);
                loc &= -align;
                n = loc;

                vpush_type_size(type, &align);
                gen_op('*');
                vset(&int_type, VT_LOCAL | VT_LVAL, n);
                vswap();
                vstore();
            }
        }
        if (n != -1)
            vpop();
        nocode_wanted = saved_nocode_wanted;

        s = sym_push(SYM_FIELD, type, 0, n);
        type->t = (t1 ? VT_VLA : VT_ARRAY) | VT_PTR;
        type->ref = s;
        type->t |= array_qualifiers;

        if (vla_array_str) {

            if ((t1 & VT_VLA) && (td & TYPE_NEST))
                s->vla_array_str = vla_array_str;
            else
                tok_str_free_str(vla_array_str);
        }
    }
    return 1;
}

static CType *type_decl(CType *type, AttributeDef *ad, int *v, int td)
{
    CType *post, *ret;
    int qualifiers, storage;

    storage = type->t & VT_STORAGE;
    type->t &= ~VT_STORAGE;
    post = ret = type;

    while (tok == '*') {
        qualifiers = 0;
    redo:
        next();
        switch (tok) {
            case TOK__Atomic:
                qualifiers |= VT_ATOMIC;
                goto redo;
            case TOK_CONST1:
            case TOK_CONST2:
            case TOK_CONST3:
                qualifiers |= VT_CONSTANT;
                goto redo;
            case TOK_VOLATILE1:
            case TOK_VOLATILE2:
            case TOK_VOLATILE3:
                qualifiers |= VT_VOLATILE;
                goto redo;
            case TOK_RESTRICT1:
            case TOK_RESTRICT2:
            case TOK_RESTRICT3:
                qualifiers |= VT_RESTRICT;
                goto redo;

            case TOK_ATTRIBUTE1:
            case TOK_ATTRIBUTE2:
                parse_attribute(ad);
                break;
        }
        mk_pointer(type);
        type->t |= qualifiers;
        if (ret == type)

            ret = pointed_type(type);
    }

    if (tok == '(') {

        if (!post_type(type, ad, 0, td)) {

            parse_attribute(ad);
            post = type_decl(type, ad, v, td);
            skip(')');
        } else
            goto abstract;
    } else if (tok >= TOK_IDENT && (td & TYPE_DIRECT)) {

        *v = tok;
        next();
    } else {
    abstract:
        if (!(td & TYPE_ABSTRACT))
            expect("identifier");
        *v = 0;
    }
    post_type(post, ad, post != ret ? 0 : storage, td & ~(TYPE_DIRECT | TYPE_ABSTRACT));
    check_restrict_type(type);
    parse_attribute(ad);
    type->t |= storage;
    return ret;
}

ST_FUNC void indir(void)
{
    if ((vtop->type.t & VT_BTYPE) != VT_PTR) {
        if ((vtop->type.t & VT_BTYPE) == VT_FUNC)
            return;
        expect("pointer");
    }
    if (vtop->r & VT_LVAL)
        gv(RC_INT);
    vtop->type = *pointed_type(&vtop->type);

    if (!(vtop->type.t & (VT_ARRAY | VT_VLA)) && (vtop->type.t & VT_BTYPE) != VT_FUNC) {
        vtop->r |= VT_LVAL;

#ifdef CONFIG_TCC_BCHECK
        if (tcc_state->do_bounds_check)
            vtop->r |= VT_MUSTBOUND;
#endif
    }
}

static void gfunc_param_typed(Sym *func, Sym *arg)
{
    int func_type;
    CType type;

    func_type = func->f.func_type;
    if (func_type == FUNC_OLD || (func_type == FUNC_ELLIPSIS && arg == NULL)) {

        if ((vtop->type.t & VT_BTYPE) == VT_FLOAT) {
            gen_cast_s(VT_DOUBLE);
        } else if (vtop->type.t & VT_BITFIELD) {
            type.t = vtop->type.t & (VT_BTYPE | VT_UNSIGNED);
            type.ref = vtop->type.ref;
            gen_cast(&type);
        } else if (vtop->r & VT_MUSTCAST) {
            force_charshort_cast();
        }
    } else if (arg == NULL) {
        tcc_error("too many arguments to function");
    } else {
        type = arg->type;
        type.t &= ~VT_CONSTANT;
        gen_assign_cast(&type);
    }
}

static void expr_type(CType *type, void (*expr_fn)(void))
{
    nocode_wanted++;
    expr_fn();
    *type = vtop->type;
    vpop();
    nocode_wanted--;
}

static void parse_expr_type(CType *type)
{
    int n;
    AttributeDef ad;

    skip('(');
    if (parse_btype(type, &ad, 0)) {
        type_decl(type, &ad, &n, TYPE_ABSTRACT);
    } else {
        expr_type(type, gexpr);
    }
    skip(')');
}

static void parse_type(CType *type)
{
    AttributeDef ad;
    int n;

    if (!parse_btype(type, &ad, 0)) {
        expect("type");
    }
    type_decl(type, &ad, &n, TYPE_ABSTRACT);
}

static void parse_builtin_params(int nc, const char *args)
{
    char c, sep = '(';
    CType type;
    if (nc)
        nocode_wanted++;
    next();
    if (*args == 0)
        skip(sep);
    while ((c = *args++)) {
        skip(sep);
        sep = ',';
        if (c == 't') {
            parse_type(&type);
            vpush(&type);
            continue;
        }
        expr_eq();
        type.ref = NULL;
        type.t = 0;
        switch (c) {
            case 'e':
                continue;
            case 'V':
                type.t = VT_CONSTANT;
            case 'v':
                type.t |= VT_VOID;
                mk_pointer(&type);
                break;
            case 'S':
                type.t = VT_CONSTANT;
            case 's':
                type.t |= char_type.t;
                mk_pointer(&type);
                break;
            case 'i':
                type.t = VT_INT;
                break;
            case 'l':
                type.t = VT_SIZE_T;
                break;
            default:
                break;
        }
        gen_assign_cast(&type);
    }
    skip(')');
    if (nc)
        nocode_wanted--;
}

static void parse_atomic(int atok)
{
    int size, align, arg, t, save = 0;
    CType *atom, *atom_ptr, ct = {0};
    SValue store;
    char buf[40];
    static const char *const templates[] = {

        "alm.?", "Asm.v", "alsm.v", "aplbmm.b", "avm.v", "avm.v", "avm.v", "avm.v",
        "avm.v", "avm.v", "avm.v",  "avm.v",    "avm.v", "avm.v", "avm.v", "avm.v"};
    const char *template = templates[(atok - TOK___atomic_store)];

    atom = atom_ptr = NULL;
    size = 0;
    next();
    skip('(');
    for (arg = 0;;) {
        expr_eq();
        switch (template[arg]) {
            case 'a':
            case 'A':
                atom_ptr = &vtop->type;
                if ((atom_ptr->t & VT_BTYPE) != VT_PTR)
                    expect("pointer");
                atom = pointed_type(atom_ptr);
                size = type_size(atom, &align);
                if (size > 8 || (size & (size - 1)) ||
                    (atok > TOK___atomic_compare_exchange &&
                     (0 == btype_size(atom->t & VT_BTYPE) || (atom->t & VT_BTYPE) == VT_PTR)))
                    expect("integral or integer-sized pointer target type");

                break;

            case 'p':
                if ((vtop->type.t & VT_BTYPE) != VT_PTR ||
                    type_size(pointed_type(&vtop->type), &align) != size)
                    tcc_error("pointer target type mismatch in argument %d", arg + 1);
                gen_assign_cast(atom_ptr);
                break;
            case 'v':
                gen_assign_cast(atom);
                break;
            case 'l':
                indir();
                gen_assign_cast(atom);
                break;
            case 's':
                save = 1;
                indir();
                store = *vtop;
                vpop();
                break;
            case 'm':
                gen_assign_cast(&int_type);
                break;
            case 'b':
                ct.t = VT_BOOL;
                gen_assign_cast(&ct);
                break;
        }
        if ('.' == template[++arg])
            break;
        skip(',');
    }
    skip(')');

    ct.t = VT_VOID;
    switch (template[arg + 1]) {
        case 'b':
            ct.t = VT_BOOL;
            break;
        case 'v':
            ct = *atom;
            break;
    }

    sprintf(buf, "%s_%d", get_tok_str(atok, 0), size);
    vpush_helper_func(tok_alloc_const(buf));
    vrott(arg - save + 1);
    gfunc_call(arg - save);

    vpush(&ct);
    PUT_R_RET(vtop, ct.t);
    t = ct.t & VT_BTYPE;
    if (t == VT_BYTE || t == VT_SHORT || t == VT_BOOL) {
#ifdef PROMOTE_RET
        vtop->r |= BFVAL(VT_MUSTCAST, 1);
#else
        vtop->type.t = VT_INT;
#endif
    }
    gen_cast(&ct);
    if (save) {
        vpush(&ct);
        *vtop = store;
        vswap();
        vstore();
    }
}

ST_FUNC void unary(void)
{
    int n, t, align, size, r;
    CType type;
    Sym *s;
    AttributeDef ad;

    if (debug_modes)
        tcc_debug_line(tcc_state), tcc_tcov_check_line(tcc_state, 1);

    type.ref = NULL;

tok_next:
    switch (tok) {
        case TOK_EXTENSION:
            next();
            goto tok_next;
        case TOK_LCHAR:
#ifdef TCC_TARGET_PE
            t = VT_SHORT | VT_UNSIGNED;
            goto push_tokc;
#endif
        case TOK_CINT:
        case TOK_CCHAR:
            t = VT_INT;
        push_tokc:
            type.t = t;
            vsetc(&type, VT_CONST, &tokc);
            next();
            break;
        case TOK_CUINT:
            t = VT_INT | VT_UNSIGNED;
            goto push_tokc;
        case TOK_CLLONG:
            t = VT_LLONG;
            goto push_tokc;
        case TOK_CULLONG:
            t = VT_LLONG | VT_UNSIGNED;
            goto push_tokc;
        case TOK_CFLOAT:
            t = VT_FLOAT;
            goto push_tokc;
        case TOK_CDOUBLE:
            t = VT_DOUBLE;
            goto push_tokc;
        case TOK_CLDOUBLE:
#ifdef TCC_USING_DOUBLE_FOR_LDOUBLE
            t = VT_DOUBLE | VT_LONG;
            tokc.d = tokc.ld;
#else
            t = VT_LDOUBLE;
#endif
            goto push_tokc;
        case TOK_CLONG:
            t = (LONG_SIZE == 8 ? VT_LLONG : VT_INT) | VT_LONG;
            goto push_tokc;
        case TOK_CULONG:
            t = (LONG_SIZE == 8 ? VT_LLONG : VT_INT) | VT_LONG | VT_UNSIGNED;
            goto push_tokc;
        case TOK___FUNCTION__:
            if (!gnu_ext)
                goto tok_identifier;

        case TOK___FUNC__:
            tok = TOK_STR;
            cstr_reset(&tokcstr);
            cstr_cat(&tokcstr, funcname, 0);
            tokc.str.size = tokcstr.size;
            tokc.str.data = tokcstr.data;
            goto case_TOK_STR;
        case TOK_LSTR:
#ifdef TCC_TARGET_PE
            t = VT_SHORT | VT_UNSIGNED;
#else
            t = VT_INT;
#endif
            goto str_init;
        case TOK_STR:
        case_TOK_STR:

            t = char_type.t;
        str_init:
            if (tcc_state->warn_write_strings & WARN_ON)
                t |= VT_CONSTANT;
            type.t = t;
            mk_pointer(&type);
            type.t |= VT_ARRAY;
            memset(&ad, 0, sizeof(AttributeDef));
            ad.section = rodata_section;
            decl_initializer_alloc(&type, &ad, VT_CONST, 2, 0, 0);
            break;
        case TOK_SOTYPE:
        case '(':
            t = tok;
            next();

            if (parse_btype(&type, &ad, 0)) {
                type_decl(&type, &ad, &n, TYPE_ABSTRACT);
                skip(')');

                if (tok == '{') {

                    if (global_expr)
                        r = VT_CONST;
                    else
                        r = VT_LOCAL;

                    if (!(type.t & VT_ARRAY))
                        r |= VT_LVAL;
                    memset(&ad, 0, sizeof(AttributeDef));
                    decl_initializer_alloc(&type, &ad, r, 1, 0, 0);
                } else if (t == TOK_SOTYPE) {
                    vpush(&type);
                    return;
                } else {
                    unary();
                    gen_cast(&type);
                }
            } else if (tok == '{') {
                int saved_nocode_wanted = nocode_wanted;
                if (CONST_WANTED && !NOEVAL_WANTED)
                    expect("constant");
                if (0 == local_scope)
                    tcc_error("statement expression outside of function");

                save_regs(0);

                vpushi(0), vtop->type.t = VT_VOID;
                block(STMT_EXPR);

                if (saved_nocode_wanted)
                    nocode_wanted = saved_nocode_wanted;
                skip(')');
            } else {
                gexpr();
                skip(')');
            }
            break;
        case '*':
            next();
            unary();
            indir();
            break;
        case '&':
            next();
            unary();

            if ((vtop->type.t & VT_BTYPE) != VT_FUNC && !(vtop->type.t & (VT_ARRAY | VT_VLA)))
                test_lvalue();
            if (vtop->sym)
                vtop->sym->a.addrtaken = 1;
            mk_pointer(&vtop->type);
            gaddrof();
            break;
        case '!':
            next();
            unary();
            gen_test_zero(TOK_EQ);
            break;
        case '~':
            next();
            unary();
            vpushi(-1);
            gen_op('^');
            break;
        case '+':
            next();
            unary();
            if ((vtop->type.t & VT_BTYPE) == VT_PTR)
                tcc_error("pointer not accepted for unary plus");

            if (!is_float(vtop->type.t)) {
                vpushi(0);
                gen_op('+');
            }
            break;
        case TOK_SIZEOF:
        case TOK_ALIGNOF1:
        case TOK_ALIGNOF2:
        case TOK_ALIGNOF3:
            t = tok;
            next();
            if (tok == '(')
                tok = TOK_SOTYPE;
            expr_type(&type, unary);
            if (t == TOK_SIZEOF) {
                vpush_type_size(&type, &align);
                gen_cast_s(VT_SIZE_T);
            } else {
                type_size(&type, &align);
                s = NULL;
                if (vtop[1].r & VT_SYM)
                    s = vtop[1].sym;
                if (s && s->a.aligned)
                    align = 1 << (s->a.aligned - 1);
                vpushs(align);
            }
            break;

        case TOK_builtin_expect:

            parse_builtin_params(0, "ee");
            vpop();
            break;
        case TOK_builtin_types_compatible_p:
            parse_builtin_params(0, "tt");
            n = compare_types(&vtop[-1].type, &vtop[0].type, 1);
            vtop -= 2;
            vpushi(n);
            break;
        case TOK_builtin_choose_expr: {
            int64_t c;
            next();
            skip('(');
            c = expr_const64();
            skip(',');
            if (!c) {
                nocode_wanted++;
            }
            expr_eq();
            if (!c) {
                vpop();
                nocode_wanted--;
            }
            skip(',');
            if (c) {
                nocode_wanted++;
            }
            expr_eq();
            if (c) {
                vpop();
                nocode_wanted--;
            }
            skip(')');
        } break;
        case TOK_builtin_constant_p:
            parse_builtin_params(1, "e");
            n = 1;
            if ((vtop->r & (VT_VALMASK | VT_LVAL)) != VT_CONST ||
                ((vtop->r & VT_SYM) && vtop->sym->a.addrtaken))
                n = 0;
            vtop--;
            vpushi(n);
            break;
        case TOK_builtin_unreachable:
            parse_builtin_params(0, "");
            type.t = VT_VOID;
            vpush(&type);
            CODE_OFF();
            break;
        case TOK_builtin_frame_address:
        case TOK_builtin_return_address: {
            int tok1 = tok;
            int level;
            next();
            skip('(');
            level = expr_const();
            if (level < 0)
                tcc_error("%s only takes positive integers", get_tok_str(tok1, 0));
            skip(')');
            type.t = VT_VOID;
            mk_pointer(&type);
            vset(&type, VT_LOCAL, 0);
            while (level--) {
#ifdef TCC_TARGET_RISCV64
                vpushi(2 * PTR_SIZE);
                gen_op('-');
#endif
                mk_pointer(&vtop->type);
                indir();
            }
            if (tok1 == TOK_builtin_return_address) {

#ifdef TCC_TARGET_ARM
                vpushi(2 * PTR_SIZE);
                gen_op('+');
#elif defined TCC_TARGET_RISCV64
                vpushi(PTR_SIZE);
                gen_op('-');
#else
                vpushi(PTR_SIZE);
                gen_op('+');
#endif
                mk_pointer(&vtop->type);
                indir();
            }
        } break;
#ifdef TCC_TARGET_RISCV64
        case TOK_builtin_va_start:
            parse_builtin_params(0, "ee");
            r = vtop->r & VT_VALMASK;
            if (r == VT_LLOCAL)
                r = VT_LOCAL;
            if (r != VT_LOCAL)
                tcc_error("__builtin_va_start expects a local variable");
            gen_va_start();
            vstore();
            break;
#endif
#ifdef TCC_TARGET_X86_64
#ifdef TCC_TARGET_PE
        case TOK_builtin_va_start:
            parse_builtin_params(0, "ee");
            r = vtop->r & VT_VALMASK;
            if (r == VT_LLOCAL)
                r = VT_LOCAL;
            if (r != VT_LOCAL)
                tcc_error("__builtin_va_start expects a local variable");
            vtop->r = r;
            vtop->type = char_pointer_type;
            vtop->c.i += 8;
            vstore();
            break;
#else
        case TOK_builtin_va_arg_types:
            parse_builtin_params(0, "t");
            vpushi(classify_x86_64_va_arg(&vtop->type));
            vswap();
            vpop();
            break;
#endif
#endif

#ifdef TCC_TARGET_ARM64
        case TOK_builtin_va_start: {
            parse_builtin_params(0, "ee");

            gen_va_start();
            vpushi(0);
            vtop->type.t = VT_VOID;
            break;
        }
        case TOK_builtin_va_arg: {
            parse_builtin_params(0, "et");
            type = vtop->type;
            vpop();

            gen_va_arg(&type);
            vtop->type = type;
            break;
        }
#endif
#ifdef TCC_TARGET_ARM64
        case TOK___arm64_clear_cache: {
            parse_builtin_params(0, "ee");
            gen_clear_cache();
            vpushi(0);
            vtop->type.t = VT_VOID;
            break;
        }
#endif
#ifdef TCC_TARGET_RISCV64
        case TOK___riscv64_clear_cache: {
            parse_builtin_params(0, "ee");
            vpop();
            vpop();
            gen_clear_cache();
            vpushi(0);
            vtop->type.t = VT_VOID;
            break;
        }
#endif

        case TOK___atomic_store:
        case TOK___atomic_load:
        case TOK___atomic_exchange:
        case TOK___atomic_compare_exchange:
        case TOK___atomic_fetch_add:
        case TOK___atomic_fetch_sub:
        case TOK___atomic_fetch_or:
        case TOK___atomic_fetch_xor:
        case TOK___atomic_fetch_and:
        case TOK___atomic_fetch_nand:
        case TOK___atomic_add_fetch:
        case TOK___atomic_sub_fetch:
        case TOK___atomic_or_fetch:
        case TOK___atomic_xor_fetch:
        case TOK___atomic_and_fetch:
        case TOK___atomic_nand_fetch:
            parse_atomic(tok);
            break;

        case TOK_INC:
        case TOK_DEC:
            t = tok;
            next();
            unary();
            inc(0, t);
            break;
        case '-':
            next();
            unary();
            if (is_float(vtop->type.t)) {
                gen_opif(TOK_NEG);
            } else {
                vpushi(0);
                vswap();
                gen_op('-');
            }
            break;
        case TOK_LAND:
            if (!gnu_ext)
                goto tok_identifier;
            next();

            if (tok < TOK_UIDENT)
                expect("label identifier");
            s = label_find(tok);
            if (!s) {
                s = label_push(&global_label_stack, tok, LABEL_FORWARD);
            } else {
                if (s->r == LABEL_DECLARED)
                    s->r = LABEL_FORWARD;
            }
            if ((s->type.t & VT_BTYPE) != VT_PTR) {
                s->type.t = VT_VOID;
                mk_pointer(&s->type);
                s->type.t |= VT_STATIC;
            }
            vpushsym(&s->type, s);
            next();
            break;

        case TOK_GENERIC: {
            CType controlling_type;
            int has_default = 0;
            int has_match = 0;
            int learn = 0;
            TokenString *str = NULL;
            int saved_nocode_wanted = nocode_wanted;
            nocode_wanted &= ~CONST_WANTED_MASK;

            next();
            skip('(');
            expr_type(&controlling_type, expr_eq);
            convert_expression_type(&controlling_type);

            nocode_wanted = saved_nocode_wanted;

            for (;;) {
                learn = 0;
                skip(',');
                if (tok == TOK_DEFAULT) {
                    if (has_default)
                        tcc_error("too many 'default'");
                    has_default = 1;
                    if (!has_match)
                        learn = 1;
                    next();
                } else {
                    int v;
                    parse_btype(&type, &ad, 0);
                    type_decl(&type, &ad, &v, TYPE_ABSTRACT);
                    if (compare_types(&controlling_type, &type, 0)) {
                        if (has_match) {
                            tcc_error("type match twice");
                        }
                        has_match = 1;
                        learn = 1;
                    }
                }
                skip(':');
                if (learn) {
                    if (str)
                        tok_str_free(str);
                    skip_or_save_block(&str);
                } else {
                    skip_or_save_block(NULL);
                }
                if (tok == ')')
                    break;
            }
            if (!str) {
                char buf[60];
                type_to_str(buf, sizeof buf, &controlling_type, NULL);
                tcc_error("type '%s' does not match any association", buf);
            }
            begin_macro(str, 1);
            next();
            expr_eq();
            if (tok != TOK_EOF)
                expect(",");
            end_macro();
            next();
            break;
        }

        case TOK___NAN__:
            n = 0x7fc00000;
        special_math_val:
            vpushi(n);
            vtop->type.t = VT_FLOAT;
            next();
            break;
        case TOK___SNAN__:
            n = 0x7f800001;
            goto special_math_val;
        case TOK___INF__:
            n = 0x7f800000;
            goto special_math_val;

        default:
        tok_identifier:
            if (tok < TOK_UIDENT)
                tcc_error("expression expected before '%s'", get_tok_str(tok, &tokc));
            t = tok;
            next();
            s = sym_find(t);
            if (!s || IS_ASM_SYM(s)) {
                const char *name = get_tok_str(t, NULL);
                if (tok != '(')
                    tcc_error("'%s' undeclared", name);

                if (!func_old)
                    tcc_warning_c(warn_implicit_function_declaration)(
                        "implicit declaration of function '%s'", name);
                s = external_global_sym(t, &func_old_type);
            }

            r = s->r;

            if ((r & VT_VALMASK) < VT_CONST)
                r = (r & ~VT_VALMASK) | VT_LOCAL;

            vset(&s->type, r, s->c);

            vtop->sym = s;

            if (r & VT_SYM) {
                vtop->c.i = 0;
#ifdef TCC_TARGET_PE
                if (s->a.dllimport) {
                    mk_pointer(&vtop->type);
                    vtop->r |= VT_LVAL;
                    indir();
                }
#endif
            } else if (r == VT_CONST && IS_ENUM_VAL(s->type.t)) {
                vtop->c.i = s->enum_val;
            }
            break;
    }

    while (1) {
        if (tok == TOK_INC || tok == TOK_DEC) {
            inc(1, tok);
            next();
        } else if (tok == '.' || tok == TOK_ARROW) {
            int qualifiers, cumofs;

            if (tok == TOK_ARROW)
                indir();
            qualifiers = vtop->type.t & VT_QUAL;
            test_lvalue();

            next();
            s = find_field(&vtop->type, tok, &cumofs);

            gaddrof();
            vtop->type = char_pointer_type;
            vpushi(cumofs);
            gen_op('+');

            vtop->type = s->type;
            if (qualifiers)
                parse_btype_qualify(&vtop->type, qualifiers);

            if (!(vtop->type.t & VT_ARRAY)) {
                vtop->r |= VT_LVAL;
#ifdef CONFIG_TCC_BCHECK

                if (tcc_state->do_bounds_check)
                    vtop->r |= VT_MUSTBOUND;
#endif
            }
            next();
        } else if (tok == '[') {
            next();
            gexpr();
            gen_op('+');
            indir();
            skip(']');
        } else if (tok == '(') {
            SValue ret;
            Sym *sa;
            int nb_args, ret_nregs, ret_align, regsize, variadic;
            TokenString *p, *p2;

            if ((vtop->type.t & VT_BTYPE) != VT_FUNC) {

                if ((vtop->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR) {
                    vtop->type = *pointed_type(&vtop->type);
                    if ((vtop->type.t & VT_BTYPE) != VT_FUNC)
                        goto error_func;
                } else {
                error_func:
                    expect("function pointer");
                }
            } else {
                vtop->r &= ~VT_LVAL;
            }

            s = vtop->type.ref;
            next();
            sa = s->next;
            nb_args = regsize = 0;
            ret.r2 = VT_CONST;

            if ((s->type.t & VT_BTYPE) == VT_STRUCT) {
                variadic = (s->f.func_type == FUNC_ELLIPSIS);
                ret_nregs = gfunc_sret(&s->type, variadic, &ret.type, &ret_align, &regsize);
                if (ret_nregs <= 0) {

                    size = type_size(&s->type, &align);
#ifdef TCC_TARGET_ARM64

                    if (size < 16)
                        while (size & (size - 1))
                            size = (size | (size - 1)) + 1;
#endif
                    loc = (loc - size) & -align;
                    ret.type = s->type;
                    ret.r = VT_LOCAL | VT_LVAL;

                    vseti(VT_LOCAL, loc);
#ifdef CONFIG_TCC_BCHECK
                    if (tcc_state->do_bounds_check)
                        --loc;
#endif
                    ret.c = vtop->c;
                    if (ret_nregs < 0)
                        vtop--;
                    else
                        nb_args++;
                }
            } else {
                ret_nregs = 1;
                ret.type = s->type;
            }

            if (ret_nregs > 0) {

                ret.c.i = 0;
                PUT_R_RET(&ret, ret.type.t);
            }

            p = NULL;
            if (tok != ')') {
                r = tcc_state->reverse_funcargs;
                for (;;) {
                    if (r) {
                        skip_or_save_block(&p2);
                        p2->prev = p, p = p2;
                    } else {
                        expr_eq();
                        gfunc_param_typed(s, sa);
                    }
                    nb_args++;
                    if (sa)
                        sa = sa->next;
                    if (tok == ')')
                        break;
                    skip(',');
                }
            }
            if (sa)
                tcc_error("too few arguments to function");

            if (p) {
                for (n = 0; p; p = p2, ++n) {
                    p2 = p, sa = s;
                    do {
                        sa = sa->next, p2 = p2->prev;
                    } while (p2 && sa);
                    p2 = p->prev;
                    begin_macro(p, 1), next();
                    expr_eq();
                    gfunc_param_typed(s, sa);
                    end_macro();
                }
                vrev(n);
            }

            next();
            vcheck_cmp();
            gfunc_call(nb_args);

            if (ret_nregs < 0) {
                vsetc(&ret.type, ret.r, &ret.c);
#ifdef TCC_TARGET_RISCV64
                arch_transfer_ret_regs(1);
#endif
            } else {

                n = ret_nregs;
                while (n > 1) {
                    int rc = reg_classes[ret.r] & ~(RC_INT | RC_FLOAT);

                    rc <<= --n;
                    for (r = 0; r < NB_REGS; ++r)
                        if (reg_classes[r] & rc)
                            break;
                    vsetc(&ret.type, r, &ret.c);
                }
                vsetc(&ret.type, ret.r, &ret.c);
                vtop->r2 = ret.r2;

                if (((s->type.t & VT_BTYPE) == VT_STRUCT) && ret_nregs) {
                    int addr, offset;

                    size = type_size(&s->type, &align);

                    size = (size + regsize - 1) & -regsize;
                    if (ret_align > align)
                        align = ret_align;
                    loc = (loc - size) & -align;
                    addr = loc;
                    offset = 0;
                    for (;;) {
                        vset(&ret.type, VT_LOCAL | VT_LVAL, addr + offset);
                        vswap();
                        vstore();
                        vtop--;
                        if (--ret_nregs == 0)
                            break;
                        offset += regsize;
                    }
                    vset(&s->type, VT_LOCAL | VT_LVAL, addr);
                }

                t = s->type.t & VT_BTYPE;
                if (t == VT_BYTE || t == VT_SHORT || t == VT_BOOL) {
#ifdef PROMOTE_RET
                    vtop->r |= BFVAL(VT_MUSTCAST, 1);
#else
                    vtop->type.t = VT_INT;
#endif
                }
            }
            if (s->f.func_noreturn) {
                if (debug_modes)
                    tcc_tcov_block_end(tcc_state, -1);
                CODE_OFF();
            }
        } else {
            break;
        }
    }
}

#ifndef precedence_parser

static void expr_prod(void)
{
    int t;

    unary();
    while ((t = tok) == '*' || t == '/' || t == '%') {
        next();
        unary();
        gen_op(t);
    }
}

static void expr_sum(void)
{
    int t;

    expr_prod();
    while ((t = tok) == '+' || t == '-') {
        next();
        expr_prod();
        gen_op(t);
    }
}

static void expr_shift(void)
{
    int t;

    expr_sum();
    while ((t = tok) == TOK_SHL || t == TOK_SAR) {
        next();
        expr_sum();
        gen_op(t);
    }
}

static void expr_cmp(void)
{
    int t;

    expr_shift();
    while (((t = tok) >= TOK_ULE && t <= TOK_GT) || t == TOK_ULT || t == TOK_UGE) {
        next();
        expr_shift();
        gen_op(t);
    }
}

static void expr_cmpeq(void)
{
    int t;

    expr_cmp();
    while ((t = tok) == TOK_EQ || t == TOK_NE) {
        next();
        expr_cmp();
        gen_op(t);
    }
}

static void expr_and(void)
{
    expr_cmpeq();
    while (tok == '&') {
        next();
        expr_cmpeq();
        gen_op('&');
    }
}

static void expr_xor(void)
{
    expr_and();
    while (tok == '^') {
        next();
        expr_and();
        gen_op('^');
    }
}

static void expr_or(void)
{
    expr_xor();
    while (tok == '|') {
        next();
        expr_xor();
        gen_op('|');
    }
}

static void expr_landor(int op);

static void expr_land(void)
{
    expr_or();
    if (tok == TOK_LAND)
        expr_landor(tok);
}

static void expr_lor(void)
{
    expr_land();
    if (tok == TOK_LOR)
        expr_landor(tok);
}

#define expr_landor_next(op) op == TOK_LAND ? expr_or() : expr_land()
#else
#define expr_landor_next(op) unary(), expr_infix(precedence(op) + 1)
#define expr_lor() unary(), expr_infix(1)

static int precedence(int tok)
{
    switch (tok) {
        case TOK_LOR:
            return 1;
        case TOK_LAND:
            return 2;
        case '|':
            return 3;
        case '^':
            return 4;
        case '&':
            return 5;
        case TOK_EQ:
        case TOK_NE:
            return 6;
        relat:
        case TOK_ULT:
        case TOK_UGE:
            return 7;
        case TOK_SHL:
        case TOK_SAR:
            return 8;
        case '+':
        case '-':
            return 9;
        case '*':
        case '/':
        case '%':
            return 10;
        default:
            if (tok >= TOK_ULE && tok <= TOK_GT)
                goto relat;
            return 0;
    }
}
static unsigned char prec[256];
static void init_prec(void)
{
    int i;
    for (i = 0; i < 256; i++)
        prec[i] = precedence(i);
}
#define precedence(i) ((unsigned)i < 256 ? prec[i] : 0)

static void expr_landor(int op);

static void expr_infix(int p)
{
    int t = tok, p2;
    while ((p2 = precedence(t)) >= p) {
        if (t == TOK_LOR || t == TOK_LAND) {
            expr_landor(t);
        } else {
            next();
            unary();
            if (precedence(tok) > p2)
                expr_infix(p2 + 1);
            gen_op(t);
        }
        t = tok;
    }
}
#endif

static int condition_3way(void)
{
    int c = -1;
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST &&
        (!(vtop->r & VT_SYM) || !vtop->sym->a.weak)) {
        vdup();
        gen_cast_s(VT_BOOL);
        c = vtop->c.i;
        vpop();
    }
    return c;
}

static void expr_landor(int op)
{
    int t = 0, cc = 1, f = 0, i = op == TOK_LAND, c;
    for (;;) {
        c = f ? i : condition_3way();
        if (c < 0)
            save_regs(1), cc = 0;
        else if (c != i)
            nocode_wanted++, f = 1;
        if (tok != op)
            break;
        if (c < 0)
            t = gvtst(i, t);
        else
            vpop();
        next();
        expr_landor_next(op);
    }
    if (cc || f) {
        vpop();
        vpushi(i ^ f);
        gsym(t);
        nocode_wanted -= f;
    } else {
        gvtst_set(i, t);
    }
}

static int is_cond_bool(SValue *sv)
{
    if ((sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && (sv->type.t & VT_BTYPE) == VT_INT)
        return (unsigned)sv->c.i < 2;
    if (sv->r == VT_CMP)
        return 1;
    return 0;
}

static void expr_cond(void)
{
    int tt, u, r1, r2, rc, t1, t2, islv, c, g;
    SValue sv;
    CType type;

    expr_lor();
    if (tok == '?') {
        next();
        c = condition_3way();
        g = (tok == ':' && gnu_ext);
        tt = 0;
        if (!g) {
            if (c < 0) {
                save_regs(1);
                tt = gvtst(1, 0);
            } else {
                vpop();
            }
        } else if (c < 0) {

            save_regs(1);
            gv_dup();
            tt = gvtst(0, 0);
        }

        if (c == 0)
            nocode_wanted++;
        if (!g)
            gexpr();

        if ((vtop->type.t & VT_BTYPE) == VT_FUNC)
            mk_pointer(&vtop->type);
        sv = *vtop;
        vtop--;

        if (g) {
            u = tt;
        } else if (c < 0) {
            u = gjmp(0);
            gsym(tt);
        } else
            u = 0;

        if (c == 0)
            nocode_wanted--;
        if (c == 1)
            nocode_wanted++;
        skip(':');
        expr_cond();

        if ((vtop->type.t & VT_BTYPE) == VT_FUNC)
            mk_pointer(&vtop->type);

        if (!combine_types(&type, &sv, vtop, '?'))
            type_incompatibility_error(
                &sv.type, &vtop->type,
                "type mismatch in conditional expression (have '%s' and '%s')");

        if (c < 0 && is_cond_bool(vtop) && is_cond_bool(&sv)) {

            t1 = gvtst(0, 0);
            t2 = gjmp(0);
            gsym(u);
            vpushv(&sv);

            gvtst_set(0, t1);
            gvtst_set(1, t2);
            gen_cast(&type);

            return;
        }

        islv = VT_STRUCT == (type.t & VT_BTYPE);

        if (c != 1) {
            gen_cast(&type);
            if (islv) {
                mk_pointer(&vtop->type);
                gaddrof();
            }
        }

        rc = RC_TYPE(type.t);

        if (USING_TWO_WORDS(type.t))
            rc = RC_RET(type.t);

        tt = r2 = 0;
        if (c < 0) {
            if (type.t != VT_VOID)
                r2 = gv(rc);
            tt = gjmp(0);
        }
        gsym(u);
        if (c == 1)
            nocode_wanted--;

        if (c != 0) {
            *vtop = sv;
            gen_cast(&type);
            if (islv) {
                mk_pointer(&vtop->type);
                gaddrof();
            }
        }

        if (c < 0) {
            if (type.t != VT_VOID) {
                r1 = gv(rc);
                move_reg(r2, r1, islv ? VT_PTR : type.t);
                vtop->r = r2;
            }
            gsym(tt);
        }

        if (islv)
            indir();
    }
}

static void expr_eq(void)
{
    int t;

    expr_cond();
    if ((t = tok) == '=' || TOK_ASSIGN(t)) {
        test_lvalue();
        next();
        if (t == '=') {
            expr_eq();
        } else {
            vdup();
            expr_eq();
            gen_op(TOK_ASSIGN_OP(t));
        }
        vstore();
    }
}

ST_FUNC void gexpr(void)
{
    expr_eq();
    if (tok == ',') {
        do {
            vpop();
            next();
            expr_eq();
        } while (tok == ',');

        convert_expression_type(&vtop->type);

        if ((vtop->r & VT_VALMASK) == VT_CONST && nocode_wanted && !CONST_WANTED)
            if (vtop->type.t != VT_VOID && (vtop->type.t & VT_BTYPE) != VT_STRUCT)
                gv(RC_TYPE(vtop->type.t));
    }
}

static void expr_const1(void)
{
    nocode_wanted += CONST_WANTED_BIT;
    expr_cond();
    nocode_wanted -= CONST_WANTED_BIT;
}

static inline int64_t expr_const64(void)
{
    int64_t c;
    expr_const1();
    if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM | VT_NONCONST)) != VT_CONST)
        expect("constant expression");
    c = vtop->c.i;
    vpop();
    return c;
}

ST_FUNC int expr_const(void)
{
    int c;
    int64_t wc = expr_const64();
    c = wc;
    if (c != wc && (unsigned)c != wc)
        tcc_error("constant exceeds 32 bit");
    return c;
}

#ifndef TCC_TARGET_ARM64
static void gfunc_return(CType *func_type)
{
    if ((func_type->t & VT_BTYPE) == VT_STRUCT) {
        CType type, ret_type;
        int ret_align, ret_nregs, regsize;
        ret_nregs = gfunc_sret(func_type, func_var, &ret_type, &ret_align, &regsize);
        if (ret_nregs < 0) {
#ifdef TCC_TARGET_RISCV64
            arch_transfer_ret_regs(0);
#endif
        } else if (0 == ret_nregs) {

            type = *func_type;
            mk_pointer(&type);
            vset(&type, VT_LOCAL | VT_LVAL, func_vc);
            indir();
            vswap();

            vstore();
        } else {

            int size, addr, align, rc, n;
            size = type_size(func_type, &align);
            if (ret_nregs * regsize > size ||
                ((align & (ret_align - 1)) &&
                 ((vtop->r & VT_VALMASK) < VT_CONST || (vtop->c.i & (ret_align - 1))))) {
                if (ret_nregs * regsize > size)
                    size = ret_nregs * regsize;
                if (ret_align > align)
                    align = ret_align;
                loc = (loc - size) & -align;
                addr = loc;
                type = *func_type;
                vset(&type, VT_LOCAL | VT_LVAL, addr);
                vswap();
                vstore();
                vpop();
                vset(&ret_type, VT_LOCAL | VT_LVAL, addr);
            }
            vtop->type = ret_type;
            rc = RC_RET(ret_type.t);

            for (n = ret_nregs; --n > 0;) {
                vdup();
                gv(rc);
                vswap();
                incr_offset(regsize);

                rc <<= 1;
            }
            gv(rc);
            vtop -= ret_nregs - 1;
        }
    } else {
        gv(RC_RET(func_type->t));
    }
    vtop--;
}
#endif

static void check_func_return(void)
{
    if ((func_vt.t & VT_BTYPE) == VT_VOID)
        return;
    if ((!strcmp(funcname, "main") || func_old) && (func_vt.t & VT_BTYPE) == VT_INT) {

        vpushi(0);
        gfunc_return(&func_vt);
    } else {
        tcc_warning("function might return no value: '%s'", funcname);
    }
}

static int case_cmp(uint64_t a, uint64_t b)
{
    if (cur_switch->sv.type.t & VT_UNSIGNED)
        return a < b ? -1 : a > b;
    else
        return (int64_t)a<(int64_t)b ? -1 : (int64_t)a>(int64_t) b;
}

static int case_cmp_qs(const void *pa, const void *pb)
{
    return case_cmp((*(struct case_t **)pa)->v1, (*(struct case_t **)pb)->v1);
}

static void case_sort(struct switch_t *sw)
{
    struct case_t **p;
    if (sw->n < 2)
        return;
    qsort(sw->p, sw->n, sizeof *sw->p, case_cmp_qs);
    p = sw->p;
    while (p < sw->p + sw->n - 1) {
        if (case_cmp(p[0]->v2, p[1]->v1) >= 0) {
            int l1 = p[0]->line, l2 = p[1]->line;

            tcc_error("%i:duplicate case value", l1 > l2 ? l1 : l2);
        } else if (p[0]->v2 + 1 == p[1]->v1 && p[0]->ind == p[1]->ind) {

            p[1]->v1 = p[0]->v1;
            tcc_free(p[0]);
            memmove(p, p + 1, (--sw->n - (p - sw->p)) * sizeof *p);
        } else
            ++p;
    }
}

static int gcase(struct case_t **base, int len, int dsym)
{
    struct case_t *p;
    int t, l2, e;

    t = vtop->type.t & VT_BTYPE;
    if (t != VT_LLONG)
        t = VT_INT;
    while (len) {

        l2 = len > 8 ? len / 2 : 0;
        p = base[l2];
        vdup(), vpush64(t, p->v2);
        if (l2 == 0 && p->v1 == p->v2) {
            gen_op(TOK_EQ);
            gsym_addr(gvtst(0, 0), p->ind);
        } else {

            gen_op(TOK_GT);
            if (len == 1)
                dsym = gvtst(0, dsym), e = 0;
            else
                e = gvtst(0, 0);
            vdup(), vpush64(t, p->v1);
            gen_op(TOK_GE);
            gsym_addr(gvtst(0, 0), p->ind);
            dsym = gcase(base, l2, dsym);
            gsym(e);
        }
        ++l2, base += l2, len -= l2;
    }

    return gjmp(dsym);
}

static void end_switch(void)
{
    struct switch_t *sw = cur_switch;
    dynarray_reset(&sw->p, &sw->n);
    cur_switch = sw->prev;
    tcc_free(sw);
}

static void save_lvalues(void)
{
    SValue *sv = vtop;
    while (sv >= vstack) {
        if (sv->sym && (sv->r & VT_LVAL)) {
            int align, size = type_size(&sv->type, &align);
            int r2, l = get_temp_local_var(size, align, &r2);
            vset(&sv->type, VT_LOCAL | VT_LVAL, l), vtop->r2 = r2;
            vpushv(sv), *sv = vtop[-1], vstore(), --vtop;
        }
        --sv;
    }
}

static void try_call_scope_cleanup(Sym *stop)
{
    Sym *cls = cur_scope->cl.s;
    for (; cls != stop; cls = cls->next) {
        Sym *fs = cls->cleanup_func;
        Sym *vs = cls->cleanup_sym;
        save_lvalues();
        vpushsym(&fs->type, fs);
        vset(&vs->type, vs->r, vs->c);
        vtop->sym = vs;
        mk_pointer(&vtop->type);
        gaddrof();
        gfunc_call(1);
    }
}

static void try_call_cleanup_goto(Sym *cleanupstate)
{
    Sym *oc, *cc;
    int ocd, ccd;

    if (!cur_scope->cl.s)
        return;

    ocd = cleanupstate ? cleanupstate->v & ~SYM_FIELD : 0;
    for (ccd = cur_scope->cl.n, oc = cleanupstate; ocd > ccd; --ocd, oc = oc->next)
        ;
    for (cc = cur_scope->cl.s; ccd > ocd; --ccd, cc = cc->next)
        ;
    for (; cc != oc; cc = cc->next, oc = oc->next, --ccd)
        ;

    try_call_scope_cleanup(cc);
}

static void block_cleanup(struct scope *o)
{
    int jmp = 0;
    Sym *g, **pg;
    for (pg = &pending_gotos; (g = *pg) && g->c > o->cl.n;) {
        if (g->cleanup_label->r & LABEL_FORWARD) {
            Sym *pcl = g->next;
            if (!jmp)
                jmp = gjmp(0);
            gsym(pcl->jnext);
            try_call_scope_cleanup(o->cl.s);
            pcl->jnext = gjmp(0);
            if (!o->cl.n)
                goto remove_pending;
            g->c = o->cl.n;
            pg = &g->prev;
        } else {
        remove_pending:
            *pg = g->prev;
            sym_free(g);
        }
    }
    gsym(jmp);
    try_call_scope_cleanup(o->cl.s);
}

static void vla_restore(int loc)
{
    if (loc)
        gen_vla_sp_restore(loc);
}

static void vla_leave(struct scope *o)
{
    struct scope *c = cur_scope, *v = NULL;
    for (; c != o && c; c = c->prev)
        if (c->vla.num)
            v = c;
    if (v)
        vla_restore(v->vla.locorig);
}

static void new_scope(struct scope *o)
{

    *o = *cur_scope;
    o->prev = cur_scope;
    cur_scope = o;
    cur_scope->vla.num = 0;

    o->lstk = local_stack;
    o->llstk = local_label_stack;
    ++local_scope;
}

static void prev_scope(struct scope *o, int is_expr)
{
    vla_leave(o->prev);

    if (o->cl.s != o->prev->cl.s)
        block_cleanup(o->prev);

    if (debug_modes)
        tcc_debug_end_scope(o->lstk, !is_expr);

    label_pop(&local_label_stack, o->llstk, is_expr);

    sym_pop(&local_stack, o->lstk, is_expr);
    cur_scope = o->prev;
    --local_scope;
}

static void leave_scope(struct scope *o)
{
    if (!o)
        return;
    try_call_scope_cleanup(o->cl.s);
    vla_leave(o);
}

static void new_scope_s(struct scope *o)
{
    o->lstk = local_stack;
    ++local_scope;
}

static void prev_scope_s(struct scope *o)
{
    sym_pop(&local_stack, o->lstk, 0);
    --local_scope;
}

static void lblock(int *bsym, int *csym)
{
    struct scope *lo = loop_scope, *co = cur_scope;
    int *b = co->bsym, *c = co->csym;
    if (csym) {
        co->csym = csym;
        loop_scope = co;
    }
    co->bsym = bsym;
    block(0);
    co->bsym = b;
    if (csym) {
        co->csym = c;
        loop_scope = lo;
    }
}

static void gexpr_decl(void)
{
    int v = decl(VT_JMP);
    if (v > 1 && tok != ';') {
        Sym *s = sym_find(v);
        vset(&s->type, s->r, (s->r & VT_SYM) ? 0 : s->c);
        vtop->sym = s;
    } else {
        if (v)
            skip(';');
        gexpr();
    }
}

static void block(int flags)
{
    int a, b, c, d, e, t;
    struct scope o;
    Sym *s;

again:
    t = tok;

    if (TOK_HAS_VALUE(t))
        goto expr;
    next();

    if (debug_modes)
        tcc_tcov_check_line(tcc_state, 0), tcc_tcov_block_begin(tcc_state);

    if (t == TOK_IF) {
        new_scope_s(&o);
        skip('(');
        gexpr_decl();
        a = gvtst(1, 0);
        skip(')');
        block(0);
        if (tok == TOK_ELSE) {
            d = gjmp(0);
            gsym(a);
            next();
            block(0);
            gsym(d);
        } else {
            gsym(a);
        }
        prev_scope_s(&o);

    } else if (t == TOK_WHILE) {
        new_scope_s(&o);
        d = gind();
        skip('(');
        gexpr();
        a = gvtst(1, 0);
        skip(')');
        b = 0;
        lblock(&a, &b);
        gjmp_addr(d);
        gsym_addr(b, d);
        gsym(a);
        prev_scope_s(&o);

    } else if (t == '{') {
        if (debug_modes)
            tcc_debug_stabn(tcc_state, N_LBRAC, ind - func_ind);
        new_scope(&o);

        while (tok == TOK_LABEL) {
            do {
                next();
                if (tok < TOK_UIDENT)
                    expect("label identifier");
                label_push(&local_label_stack, tok, LABEL_DECLARED);
                next();
            } while (tok == ',');
            skip(';');
        }

        while (tok != '}') {
            decl(VT_LOCAL);
            if (tok != '}') {
                block(flags | STMT_COMPOUND);
            }
        }

        prev_scope(&o, flags & STMT_EXPR);
        if (debug_modes)
            tcc_debug_stabn(tcc_state, N_RBRAC, ind - func_ind);
        if (local_scope)
            next();
        else if (!nocode_wanted)
            check_func_return();

    } else if (t == TOK_RETURN) {
        b = (func_vt.t & VT_BTYPE) != VT_VOID;
        if (tok != ';') {
            gexpr();
            if (b) {
                gen_assign_cast(&func_vt);
            } else {
                if (vtop->type.t != VT_VOID)
                    tcc_warning("void function returns a value");
                vtop--;
            }
        } else if (b && func_old && (func_vt.t & VT_BTYPE) == VT_INT) {
            vpushi(0);
        } else if (b) {
            tcc_warning("'return' with no value");
            b = 0;
        }
        leave_scope(root_scope);
        if (b)
            gfunc_return(&func_vt);
        skip(';');

        if (tok != '}' || local_scope != 1)
            rsym = gjmp(rsym);
        if (debug_modes)
            tcc_tcov_block_end(tcc_state, -1);
        CODE_OFF();

    } else if (t == TOK_BREAK) {

        if (!cur_scope->bsym)
            tcc_error("cannot break");
        if (cur_switch && cur_scope->bsym == cur_switch->bsym)
            leave_scope(cur_switch->scope);
        else
            leave_scope(loop_scope);
        *cur_scope->bsym = gjmp(*cur_scope->bsym);
        skip(';');

    } else if (t == TOK_CONTINUE) {

        if (!cur_scope->csym)
            tcc_error("cannot continue");
        leave_scope(loop_scope);
        *cur_scope->csym = gjmp(*cur_scope->csym);
        skip(';');

    } else if (t == TOK_FOR) {
        new_scope(&o);

        skip('(');
        if (tok != ';') {

            if (!decl(VT_JMP)) {

                gexpr();
                vpop();
            }
        }
        skip(';');
        a = b = 0;
        c = d = gind();
        if (tok != ';') {
            gexpr();
            a = gvtst(1, 0);
        }
        skip(';');
        if (tok != ')') {
            e = gjmp(0);
            d = gind();
            gexpr();
            vpop();
            gjmp_addr(c);
            gsym(e);
        }
        skip(')');
        lblock(&a, &b);
        gjmp_addr(d);
        gsym_addr(b, d);
        gsym(a);
        prev_scope(&o, 0);

    } else if (t == TOK_DO) {
        new_scope_s(&o);
        a = b = 0;
        d = gind();
        lblock(&a, &b);
        gsym(b);
        skip(TOK_WHILE);
        skip('(');
        gexpr();
        c = gvtst(0, 0);
        skip(')');
        skip(';');
        gsym_addr(c, d);
        gsym(a);
        prev_scope_s(&o);

    } else if (t == TOK_SWITCH) {
        struct switch_t *sw;

        sw = tcc_mallocz(sizeof *sw);
        sw->bsym = &a;
        sw->scope = cur_scope;
        sw->prev = cur_switch;
        sw->nocode_wanted = nocode_wanted;
        cur_switch = sw;

        new_scope_s(&o);
        skip('(');
        gexpr_decl();
        if (!is_integer_btype(vtop->type.t & VT_BTYPE))
            tcc_error("switch value not an integer");
        skip(')');
        sw->sv = *vtop--;
        a = 0;
        b = gjmp(0);
        lblock(&a, NULL);
        a = gjmp(a);

        gsym(b);
        prev_scope_s(&o);
        if (sw->nocode_wanted)
            goto skip_switch;
        case_sort(sw);
        sw->bsym = NULL;
        vpushv(&sw->sv);
        gv(RC_INT);
        d = gcase(sw->p, sw->n, 0);
        vpop();
        if (sw->def_sym)
            gsym_addr(d, sw->def_sym);
        else
            gsym(d);
    skip_switch:

        gsym(a);
        end_switch();

    } else if (t == TOK_CASE) {
        struct case_t *cr;
        if (!cur_switch)
            expect("switch");
        cr = tcc_malloc(sizeof(struct case_t));
        dynarray_add(&cur_switch->p, &cur_switch->n, cr);
        t = cur_switch->sv.type.t;
        cr->v1 = cr->v2 = value64(expr_const64(), t);
        if (tok == TOK_DOTS && gnu_ext) {
            next();
            cr->v2 = value64(expr_const64(), t);
            if (case_cmp(cr->v2, cr->v1) < 0)
                tcc_warning("empty case range");
        }

        if (!cur_switch->nocode_wanted)
            cr->ind = gind();
        cr->line = file->line_num;
        skip(':');
        goto block_after_label;

    } else if (t == TOK_DEFAULT) {
        if (!cur_switch)
            expect("switch");
        if (cur_switch->def_sym)
            tcc_error("too many 'default'");
        cur_switch->def_sym = cur_switch->nocode_wanted ? -1 : gind();
        skip(':');
        goto block_after_label;

    } else if (t == TOK_GOTO) {
        vla_restore(cur_scope->vla.locorig);
        if (tok == '*' && gnu_ext) {

            next();
            gexpr();
            if ((vtop->type.t & VT_BTYPE) != VT_PTR)
                expect("pointer");
            ggoto();

        } else if (tok >= TOK_UIDENT) {
            s = label_find(tok);

            if (!s)
                s = label_push(&global_label_stack, tok, LABEL_FORWARD);
            else if (s->r == LABEL_DECLARED)
                s->r = LABEL_FORWARD;

            if (s->r & LABEL_FORWARD) {

                if (cur_scope->cl.s && !nocode_wanted) {
                    sym_push2(&pending_gotos, SYM_FIELD, 0, cur_scope->cl.n);
                    pending_gotos->cleanup_label = s;
                    s = sym_push2(&s->next, SYM_FIELD, 0, 0);
                    pending_gotos->next = s;
                }
                s->jnext = gjmp(s->jnext);
            } else {
                try_call_cleanup_goto(s->cleanupstate);
                gjmp_addr(s->jind);
            }
            next();

        } else {
            expect("label identifier");
        }
        skip(';');

    } else if (t == TOK_ASM1 || t == TOK_ASM2 || t == TOK_ASM3) {
        asm_instr();

    } else {
        if (tok == ':' && t >= TOK_UIDENT) {

            next();
            s = label_find(t);
            if (s) {
                if (s->r == LABEL_DEFINED)
                    tcc_error("duplicate label '%s'", get_tok_str(s->v, NULL));
                s->r = LABEL_DEFINED;
                if (s->next) {
                    Sym *pcl;
                    for (pcl = s->next; pcl; pcl = pcl->prev)
                        gsym(pcl->jnext);
                    sym_pop(&s->next, NULL, 0);
                } else
                    gsym(s->jnext);
            } else {
                s = label_push(&global_label_stack, t, LABEL_DEFINED);
            }
            s->jind = gind();
            s->cleanupstate = cur_scope->cl.s;

        block_after_label:

            parse_attribute(NULL);

            if (debug_modes)
                tcc_tcov_reset_ind(tcc_state);
            vla_restore(cur_scope->vla.loc);

            if (tok != '}') {
                if (0 == (flags & STMT_COMPOUND))
                    goto again;

            } else {

                tcc_warning_c(warn_all)("deprecated use of label at end of compound statement");
            }
        } else {

            if (t != ';') {
                unget_tok(t);
            expr:
                if (flags & STMT_EXPR) {
                    vpop();
                    gexpr();
                } else {
                    gexpr();
                    vpop();
                }
                skip(';');
            }
        }
    }

    if (debug_modes)
        tcc_tcov_check_line(tcc_state, 0), tcc_tcov_block_end(tcc_state, 0);
}

static void skip_or_save_block(TokenString **str)
{
    int braces = tok == '{';
    int level = 0;
    if (str)
        *str = tok_str_alloc();

    while (1) {
        int t = tok;
        if (level == 0 && (t == ',' || t == ';' || t == '}' || t == ')' || t == ']'))
            break;
        if (t == TOK_EOF) {
            if (str || level > 0)
                tcc_error("unexpected end of file");
            else
                break;
        }
        if (str)
            tok_str_add_tok(*str);
        next();
        if (t == '{' || t == '(' || t == '[') {
            level++;
        } else if (t == '}' || t == ')' || t == ']') {
            level--;
            if (level == 0 && braces && t == '}')
                break;
        }
    }
    if (str)
        tok_str_add(*str, TOK_EOF);
}

#define EXPR_CONST 1
#define EXPR_ANY 2

static void parse_init_elem(int expr_type)
{
    int saved_global_expr;
    switch (expr_type) {
        case EXPR_CONST:

            saved_global_expr = global_expr;
            global_expr = 1;
            expr_const1();
            global_expr = saved_global_expr;

            if (((vtop->r & (VT_VALMASK | VT_LVAL)) != VT_CONST &&
                 ((vtop->r & (VT_SYM | VT_LVAL)) != (VT_SYM | VT_LVAL) ||
                  vtop->sym->v < SYM_FIRST_ANOM))
#ifdef TCC_TARGET_PE
                || ((vtop->r & VT_SYM) && vtop->sym->a.dllimport)
#endif
            )
                tcc_error("initializer element is not constant");
            break;
        case EXPR_ANY:
            expr_eq();
            break;
    }
}

#if 1
static void init_assert(init_params *p, int offset)
{
    if (p->sec ? !NODATA_WANTED && offset > p->sec->data_offset
               : !nocode_wanted && offset > p->local_offset)
        tcc_internal_error("initializer overflow");
}
#else
#define init_assert(sec, offset)
#endif

static void init_putz(init_params *p, unsigned long c, int size)
{
    init_assert(p, c + size);
    if (p->sec) {

    } else {
        vpush_helper_func(TOK_memset);
        vseti(VT_LOCAL, c);
        vpushi(0);
        vpushs(size);
#if defined TCC_TARGET_ARM && defined TCC_ARM_EABI
        vswap();
#endif
        gfunc_call(3);
    }
}

#define DIF_FIRST 1
#define DIF_SIZE_ONLY 2
#define DIF_HAVE_ELEM 4
#define DIF_CLEAR 8

static void decl_design_delrels(Section *sec, int c, int size)
{
    ElfW_Rel *rel, *rel2, *rel_end;
    if (!sec || !sec->reloc)
        return;
    rel = rel2 = (ElfW_Rel *)sec->reloc->data;
    rel_end = (ElfW_Rel *)(sec->reloc->data + sec->reloc->data_offset);
    while (rel < rel_end) {
        if (rel->r_offset >= c && rel->r_offset < c + size) {
            sec->reloc->data_offset -= sizeof *rel;
        } else {
            if (rel2 != rel)
                memcpy(rel2, rel, sizeof *rel);
            ++rel2;
        }
        ++rel;
    }
}

static void decl_design_flex(init_params *p, Sym *ref, int index)
{
    if (ref == p->flex_array_ref) {
        if (index >= ref->c)
            ref->c = index + 1;
    } else if (ref->c < 0 && index >= 0)
        tcc_error("flexible array has zero size in this context");
}

static int decl_designator(init_params *p, CType *type, unsigned long c, Sym **cur_field, int flags,
                           int al)
{
    Sym *s, *f;
    int index, index_last, align, l, nb_elems, elem_size;
    unsigned long corig = c;

    elem_size = 0;
    nb_elems = 1;

    if (flags & DIF_HAVE_ELEM)
        goto no_designator;

    if (gnu_ext && tok >= TOK_UIDENT) {
        l = tok, next();
        if (tok == ':')
            goto struct_field;
        unget_tok(l);
    }

    while (nb_elems == 1 && (tok == '[' || tok == '.')) {
        if (tok == '[') {
            if (!(type->t & VT_ARRAY))
                expect("array type");
            next();
            index = index_last = expr_const();
            if (tok == TOK_DOTS && gnu_ext) {
                next();
                index_last = expr_const();
            }
            skip(']');
            s = type->ref;
            decl_design_flex(p, s, index_last);
            if (index < 0 || index_last >= s->c || index_last < index)
                tcc_error("index exceeds array bounds or range is empty");
            if (cur_field)
                (*cur_field)->c = index_last;
            type = pointed_type(type);
            elem_size = type_size(type, &align);
            c += index * elem_size;
            nb_elems = index_last - index + 1;
        } else {
            int cumofs;
            next();
            l = tok;
        struct_field:
            next();
            f = find_field(type, l, &cumofs);
            if (cur_field)
                *cur_field = f;
            type = &f->type;
            c += cumofs;
        }
        cur_field = NULL;
    }
    if (!cur_field) {
        if (tok == '=') {
            next();
        } else if (!gnu_ext) {
            expect("=");
        }
    } else {
    no_designator:
        if (type->t & VT_ARRAY) {
            index = (*cur_field)->c;
            s = type->ref;
            decl_design_flex(p, s, index);
            if (index >= s->c)
                tcc_error("too many initializers");
            type = pointed_type(type);
            elem_size = type_size(type, &align);
            c += index * elem_size;
        } else {
            f = *cur_field;

            while (f && (f->v & SYM_FIRST_ANOM) && is_integer_btype(f->type.t & VT_BTYPE))
                *cur_field = f = f->next;
            if (!f)
                tcc_error("too many initializers");
            type = &f->type;
            c += f->c;
        }
    }

    if (!elem_size)
        elem_size = type_size(type, &align);

    if (!(flags & DIF_SIZE_ONLY) && c - corig < al) {
        decl_design_delrels(p->sec, c, elem_size * nb_elems);
        flags &= ~DIF_CLEAR;
    }

    decl_initializer(p, type, c, flags & ~DIF_FIRST);

    if (!(flags & DIF_SIZE_ONLY) && nb_elems > 1) {
        Sym aref = {0};
        CType t1;
        int i;
        if (p->sec || (type->t & VT_ARRAY)) {

            aref.c = elem_size;
            t1.t = VT_STRUCT, t1.ref = &aref;
            type = &t1;
        }
        if (p->sec)
            vpush_ref(type, p->sec, c, elem_size);
        else
            vset(type, VT_LOCAL | VT_LVAL, c);
        for (i = 1; i < nb_elems; i++) {
            vdup();
            init_putv(p, type, c + elem_size * i);
        }
        vpop();
    }

    c += nb_elems * elem_size;
    if (c - corig > al)
        al = c - corig;
    return al;
}

static void write_ldouble(unsigned char *d, void *s)
{

#ifdef TCC_CROSS_TEST
    if (LDOUBLE_SIZE >= 10) {
        double b = *(long double *)s;
        s = &b;
#else
    if (sizeof(long double) == 8 && LDOUBLE_SIZE >= 10) {
#endif

        uint64_t m = *(uint64_t *)s;
        int e = m >> 48;
        int f = e >> 4 & 0x7FF;
        m <<= 11;
        if (0 == f) {
            if (0 == m)
                goto set;
            for (f = 1; !(m & 1ULL << 63); --f)
                m <<= 1;
        }
        if (f == 0x7ff)
            f = 0x43FF;
        e = (e & 0x8000) | (f + 0x3C00);
        m |= 1ULL << 63;
    set:
#if (defined TCC_TARGET_I386 || defined TCC_TARGET_X86_64)

        write64le(d, m);
        write16le(d + 8, e);
#elif LDOUBLE_SIZE == 16

        write64le(d + 6, m << 1);
        write16le(d + 14, e);
#endif
        ;
    } else {
#if LDOUBLE_SIZE == 8

        double b = *(long double *)s;
        memcpy(d, &b, 8);
#elif (__i386__ || __x86_64__) && (defined TCC_TARGET_I386 || defined TCC_TARGET_X86_64)

        memcpy(d, s, 10);
#elif (__i386__ || __x86_64__) && (defined TCC_TARGET_ARM64 || defined TCC_TARGET_RISCV64)

    uint64_t m = *(uint64_t *)s;
    int e = *(uint16_t *)((char *)s + 8);
    write64le(d + 6, m << 1);
    write16le(d + 14, e);
#elif (__aarch64__ || __riscv) && (defined TCC_TARGET_I386 || defined TCC_TARGET_X86_64)

    uint64_t m = read64le((char *)s + 6);
    int e = read16le((char *)s + 14);
    (e & 0x7fff) && (m & 1) && 0 == ++m && ++e;
    write64le(d, m >> 1 | ((e & 0x7fff) ? 1ULL << 63 : 0));
    write16le(d + 8, e);
#else

    if (sizeof(long double) == LDOUBLE_SIZE)
        memcpy(d, s, LDOUBLE_SIZE);
#endif
    }
}

static void init_putv(init_params *p, CType *type, unsigned long c)
{
    int bt;
    void *ptr;
    CType dtype;
    int size, align;
    Section *sec = p->sec;
    uint64_t val;

    dtype = *type;
    dtype.t &= ~VT_CONSTANT;

    size = type_size(type, &align);
    if (type->t & VT_BITFIELD)
        size = (BIT_POS(type->t) + BIT_SIZE(type->t) + 7) / 8;
    init_assert(p, c + size);

    if (sec) {

        gen_assign_cast(&dtype);
        bt = type->t & VT_BTYPE;

        if ((vtop->r & VT_SYM) && bt != VT_PTR &&
            (bt != (PTR_SIZE == 8 ? VT_LLONG : VT_INT) || (type->t & VT_BITFIELD)) &&
            !((vtop->r & VT_CONST) && vtop->sym->v >= SYM_FIRST_ANOM))
            tcc_error("initializer element is not computable at load time");

        if (NODATA_WANTED) {
            vtop--;
            return;
        }

        ptr = sec->data + c;
        val = vtop->c.i;

        if ((vtop->r & (VT_SYM | VT_CONST)) == (VT_SYM | VT_CONST) &&
            vtop->sym->v >= SYM_FIRST_ANOM && ((vtop->r & VT_LVAL) || bt == VT_STRUCT)) {

            Section *ssec;
            ElfSym *esym;
            ElfW_Rel *rel;
            esym = elfsym(vtop->sym);
            ssec = tcc_state->sections[esym->st_shndx];
            memmove(ptr, ssec->data + esym->st_value + (int)vtop->c.i, size);
            if (ssec->reloc) {

                unsigned long relofs = ssec->reloc->data_offset;
                while (relofs >= sizeof(*rel)) {
                    relofs -= sizeof(*rel);
                    rel = (ElfW_Rel *)(ssec->reloc->data + relofs);
                    if (rel->r_offset >= esym->st_value + size)
                        continue;
                    if (rel->r_offset < esym->st_value)
                        break;
                    put_elf_reloca(symtab_section, sec, c + rel->r_offset - esym->st_value,
                                   ELFW(R_TYPE)(rel->r_info), ELFW(R_SYM)(rel->r_info),
#if PTR_SIZE == 8
                                   rel->r_addend
#else
                                   0
#endif
                    );
                }
            }
        } else {
            if (type->t & VT_BITFIELD) {
                int bit_pos, bit_size, bits, n;
                unsigned char *p, v, m;
                bit_pos = BIT_POS(vtop->type.t);
                bit_size = BIT_SIZE(vtop->type.t);
                p = (unsigned char *)ptr + (bit_pos >> 3);
                bit_pos &= 7, bits = 0;
                while (bit_size) {
                    n = 8 - bit_pos;
                    if (n > bit_size)
                        n = bit_size;
                    v = val >> bits << bit_pos;
                    m = ((1 << n) - 1) << bit_pos;
                    *p = (*p & ~m) | (v & m);
                    bits += n, bit_size -= n, bit_pos = 0, ++p;
                }
            } else
                switch (bt) {
                    case VT_BOOL:
                        *(char *)ptr = val != 0;
                        break;
                    case VT_BYTE:
                        *(char *)ptr = val;
                        break;
                    case VT_SHORT:
                        write16le(ptr, val);
                        break;
                    case VT_FLOAT:
                        write32le(ptr, val);
                        break;
                    case VT_DOUBLE:
                        write64le(ptr, val);
                        break;
                    case VT_LDOUBLE:
                        write_ldouble(ptr, &vtop->c.ld);
                        break;

#if PTR_SIZE == 8

                    case VT_LLONG:
                    case VT_PTR:
                        if (vtop->r & VT_SYM)
                            greloca(sec, vtop->sym, c, R_DATA_PTR, val);
                        else
                            write64le(ptr, val);
                        break;
                    case VT_INT:
                        write32le(ptr, val);
                        break;
#else
                    case VT_LLONG:
                        write64le(ptr, val);
                        break;
                    case VT_PTR:
                    case VT_INT:
                        if (vtop->r & VT_SYM)
                            greloc(sec, vtop->sym, c, R_DATA_PTR);
                        write32le(ptr, val);
                        break;
#endif
                    default:

                        break;
                }
        }
        vtop--;
    } else {
        vset(&dtype, VT_LOCAL | VT_LVAL, c);
        vswap();
        vstore();
        vpop();
    }
}

static void decl_initializer(init_params *p, CType *type, unsigned long c, int flags)
{
    int len, n, no_oblock, i;
    int size1, align1;
    Sym *s, *f;
    Sym indexsym;
    CType *t1;

    if (debug_modes && !(flags & DIF_SIZE_ONLY) && !p->sec)
        tcc_debug_line(tcc_state), tcc_tcov_check_line(tcc_state, 1);

    if (!(flags & DIF_HAVE_ELEM) && tok != '{' &&

        tok != TOK_LSTR && tok != TOK_STR &&
        (!(flags & DIF_SIZE_ONLY)

         || (type->t & VT_BTYPE) == VT_STRUCT)) {
        int ncw_prev = nocode_wanted;
        if ((flags & DIF_SIZE_ONLY) && !p->sec)
            ++nocode_wanted;
        parse_init_elem(!p->sec ? EXPR_ANY : EXPR_CONST);
        nocode_wanted = ncw_prev;
        flags |= DIF_HAVE_ELEM;
    }

    if (type->t & VT_ARRAY) {
        no_oblock = 1;
        if (((flags & DIF_FIRST) && tok != TOK_LSTR && tok != TOK_STR) || tok == '{') {
            skip('{');
            no_oblock = 0;
        }

        s = type->ref;
        n = s->c;
        t1 = pointed_type(type);
        size1 = type_size(t1, &align1);

        if ((tok == TOK_LSTR &&
#ifdef TCC_TARGET_PE
             (t1->t & VT_BTYPE) == VT_SHORT && (t1->t & VT_UNSIGNED)
#else
             (t1->t & VT_BTYPE) == VT_INT
#endif
                 ) ||
            (tok == TOK_STR && (t1->t & VT_BTYPE) == VT_BYTE)) {
            len = 0;
            cstr_reset(&initstr);
            if (size1 != (tok == TOK_STR ? 1 : sizeof(nwchar_t)))
                tcc_error("unhandled string literal merging");
            while (tok == TOK_STR || tok == TOK_LSTR) {
                if (initstr.size)
                    initstr.size -= size1;
                if (tok == TOK_STR)
                    len += tokc.str.size;
                else
                    len += tokc.str.size / sizeof(nwchar_t);
                len--;
                cstr_cat(&initstr, tokc.str.data, tokc.str.size);
                next();
            }
            if (tok != ')' && tok != '}' && tok != ',' && tok != ';' && tok != TOK_EOF) {

                unget_tok(size1 == 1 ? TOK_STR : TOK_LSTR);
                tokc.str.size = initstr.size;
                tokc.str.data = initstr.data;
                goto do_init_array;
            }

            decl_design_flex(p, s, len);
            if (!(flags & DIF_SIZE_ONLY)) {
                int nb = n, ch;
                if (len < nb)
                    nb = len;
                if (len > nb)
                    tcc_warning("initializer-string for array is too long");

                if (p->sec && size1 == 1) {
                    init_assert(p, c + nb);
                    if (!NODATA_WANTED)
                        memcpy(p->sec->data + c, initstr.data, nb);
                } else {
                    for (i = 0; i < n; i++) {
                        if (i >= nb) {

                            if (flags & DIF_CLEAR)
                                break;
                            if (n - i >= 4) {
                                init_putz(p, c + i * size1, (n - i) * size1);
                                break;
                            }
                            ch = 0;
                        } else if (size1 == 1)
                            ch = ((unsigned char *)initstr.data)[i];
                        else
                            ch = ((nwchar_t *)initstr.data)[i];
                        vpushi(ch);
                        init_putv(p, t1, c + i * size1);
                    }
                }
            }
            if (tok == ',' && !no_oblock)
                next();
        } else {

        do_init_array:
            indexsym.c = 0;
            f = &indexsym;

        do_init_list:

            if (!(flags & (DIF_CLEAR | DIF_SIZE_ONLY))) {
                init_putz(p, c, n * size1);
                flags |= DIF_CLEAR;
            }

            len = 0;

            decl_design_flex(p, s, len - 1);
            while (tok != '}' || (flags & DIF_HAVE_ELEM)) {
                len = decl_designator(p, type, c, &f, flags, len);
                flags &= ~DIF_HAVE_ELEM;
                if (type->t & VT_ARRAY) {
                    ++indexsym.c;

                    if (no_oblock && len >= n * size1)
                        break;
                } else {
                    if (s->type.t == VT_UNION)
                        f = NULL;
                    else
                        f = f->next;
                    if (no_oblock && f == NULL)
                        break;
                }

                if (tok == '}')
                    break;
                skip(',');
            }
        }
        if (!no_oblock)
            skip('}');

    } else if ((flags & DIF_HAVE_ELEM)

               && is_compatible_unqualified_types(type, &vtop->type)) {
        goto one_elem;

    } else if ((type->t & VT_BTYPE) == VT_STRUCT) {
        no_oblock = 1;
        if ((flags & DIF_FIRST) || tok == '{') {
            skip('{');
            no_oblock = 0;
        }
        s = type->ref;
        f = s->next;
        n = s->c;
        size1 = 1;
        goto do_init_list;

    } else if (tok == '{') {
        if (flags & DIF_HAVE_ELEM)
            skip(';');
        next();
        decl_initializer(p, type, c, flags & ~DIF_HAVE_ELEM);
        skip('}');

    } else
    one_elem:
        if ((flags & DIF_SIZE_ONLY)) {

            if (flags & DIF_HAVE_ELEM)
                vpop();
            else
                skip_or_save_block(NULL);

        } else {
            if (!(flags & DIF_HAVE_ELEM)) {

                if (tok != TOK_STR && tok != TOK_LSTR)
                    expect("string constant");
                parse_init_elem(!p->sec ? EXPR_ANY : EXPR_CONST);
            }
            if (!p->sec && (flags & DIF_CLEAR) &&
                (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && vtop->c.i == 0 &&
                btype_size(type->t & VT_BTYPE))
                vpop();
            else
                init_putv(p, type, c);
        }
}

static void decl_initializer_alloc(CType *type, AttributeDef *ad, int r, int has_init, int v,
                                   int scope)
{
    int size, align, addr;
    TokenString *init_str = NULL;

    Section *sec;
    Sym *flexible_array;
    Sym *sym = NULL;
    int saved_nocode_wanted = nocode_wanted;
#ifdef CONFIG_TCC_BCHECK
    int bcheck = tcc_state->do_bounds_check && !NODATA_WANTED;
#endif
    init_params p = {0};

    if (scope == VT_CONST) {

        sym = sym_find(v);
        if (sym) {
            patch_storage(sym, ad, type);

            if (!has_init && sym->c && elfsym(sym)->st_shndx != SHN_UNDEF)
                return;
            type = &sym->type;
        }
    }

    if (v && (r & VT_VALMASK) == VT_CONST)
        nocode_wanted |= DATA_ONLY_WANTED;

    flexible_array = NULL;
    size = type_size(type, &align);

    if (size < 0) {

        if (!(type->t & VT_ARRAY))
            tcc_error("initialization of incomplete type");

        if (IS_BT_ARRAY(type->t))
            type->ref = sym_push(SYM_FIELD, &type->ref->type, 0, type->ref->c);
        p.flex_array_ref = type->ref;

    } else if (has_init && (type->t & VT_BTYPE) == VT_STRUCT) {
        Sym *field = type->ref->next;
        if (field) {
            while (field->next)
                field = field->next;
            if (field->type.t & VT_ARRAY && field->type.ref->c < 0) {
                flexible_array = field;
                p.flex_array_ref = field->type.ref;
                size = -1;
            }
        }
    }

    if (size < 0) {

        if (!has_init)
            goto err_size;
        if (has_init == 2) {

            init_str = tok_str_alloc();
            while (tok == TOK_STR || tok == TOK_LSTR) {
                tok_str_add_tok(init_str);
                next();
            }
            tok_str_add(init_str, TOK_EOF);
        } else
            skip_or_save_block(&init_str);
        unget_tok(0);

        begin_macro(init_str, 1);
        next();
        decl_initializer(&p, type, 0, DIF_FIRST | DIF_SIZE_ONLY);

        macro_ptr = init_str->str;
        next();

        size = type_size(type, &align);
        if (size < 0)
        err_size:
            tcc_error("unknown type size");

        if (flexible_array && flexible_array->type.ref->c > 0)
            size += flexible_array->type.ref->c * pointed_size(&flexible_array->type);
    }

    if (ad->a.aligned) {
        int speca = 1 << (ad->a.aligned - 1);
        if (speca > align)
            align = speca;
    } else if (ad->a.packed) {
        align = 1;
    }

    if (!v && NODATA_WANTED)
        size = 0, align = 1;

    if ((r & VT_VALMASK) == VT_LOCAL) {
        sec = NULL;
#ifdef CONFIG_TCC_BCHECK
        if (bcheck && v) {

            loc -= align;
        }
#endif
        loc = (loc - size) & -align;
        addr = loc;
        p.local_offset = addr + size;
#ifdef CONFIG_TCC_BCHECK
        if (bcheck && v) {

            loc -= align;
        }
#endif
        if (v) {

#ifdef CONFIG_TCC_ASM
            if (ad->asm_label) {
                int reg = asm_parse_regvar(ad->asm_label);
                if (reg >= 0)
                    r = (r & ~VT_VALMASK) | reg;
            }
#endif
            sym = sym_push(v, type, r, addr);
            if (ad->cleanup_func) {
                Sym *cls = sym_push2(&all_cleanups, SYM_FIELD | ++cur_scope->cl.n, 0, 0);
                cls->cleanup_sym = sym;
                cls->cleanup_func = ad->cleanup_func;
                cls->next = cur_scope->cl.s;
                cur_scope->cl.s = cls;
            }

            sym->a = ad->a;
        } else {

            vset(type, r, addr);
        }
    } else {
        CType *tp = type;
        int is_const;

        while ((tp->t & (VT_BTYPE | VT_ARRAY)) == (VT_PTR | VT_ARRAY))
            tp = &tp->ref->type;
        is_const = tp->t & VT_CONSTANT;

        sec = ad->section;
        if (!sec) {
            if (type->t & VT_TLS) {
                sec = find_section(tcc_state, has_init ? ".tdata" : ".tbss");
                sec->sh_flags = SHF_ALLOC | SHF_WRITE | SHF_TLS;
                sec->sh_type = has_init ? SHT_PROGBITS : SHT_NOBITS;
            } else if (is_const) {
                sec = rodata_section;
            } else if (has_init) {
                sec = data_section;

            } else if (tcc_state->nocommon)
                sec = bss_section;
        } else if (ad->new_section) {

            if (!is_const) {
                sec->sh_flags |= SHF_WRITE;
                if (!has_init)
                    sec->sh_type = SHT_NOBITS;
            }
        }

#ifdef CONFIG_TCC_BCHECK
        if (bcheck)
            size = size + 1;
#endif
        if (sec) {
            addr = section_add(sec, size, align);
        } else {
            addr = align;
            sec = common_section;
        }

        if (v) {
            if (!sym) {
                sym = sym_push(v, type, r | VT_SYM, 0);
                patch_storage(sym, ad, NULL);
            }

            put_extern_sym(sym, sec, addr, size);
        } else {

            vpush_ref(type, sec, addr, size);
            sym = vtop->sym;
            vtop->r |= r;
        }

#ifdef CONFIG_TCC_BCHECK

        if (bcheck) {
            addr_t *bounds_ptr;

            greloca(bounds_section, sym, bounds_section->data_offset, R_DATA_PTR, 0);

            bounds_ptr = section_ptr_add(bounds_section, 2 * sizeof(addr_t));
            bounds_ptr[0] = 0;
            bounds_ptr[1] = size - 1;
        }
#endif
    }

    if (type->t & VT_VLA) {
        int a;

        if (has_init)
            tcc_error("variable length array cannot be initialized");

        if (NODATA_WANTED)
            goto no_alloc;

        if (cur_scope->vla.num == 0) {
            if (cur_scope->prev && cur_scope->prev->vla.num) {
                cur_scope->vla.locorig = cur_scope->prev->vla.loc;
            } else {
                gen_vla_sp_save(loc -= PTR_SIZE);
                cur_scope->vla.locorig = loc;
            }
        }

        vpush_type_size(type, &a);
        gen_vla_alloc(type, a);
#if defined TCC_TARGET_PE && defined TCC_TARGET_X86_64

        gen_vla_result(addr), addr = (loc -= PTR_SIZE);
#endif
        gen_vla_sp_save(addr);
        cur_scope->vla.loc = addr;
        cur_scope->vla.num++;
    } else if (has_init) {
        p.sec = sec;
        decl_initializer(&p, type, addr, DIF_FIRST);

        if (flexible_array)
            flexible_array->type.ref->c = -1;
    }

no_alloc:

    if (init_str) {
        end_macro();
        next();
    }

    nocode_wanted = saved_nocode_wanted;
}

static void func_vla_arg_code(Sym *arg)
{
    int align;
    TokenString *vla_array_tok = NULL;

    if (arg->type.ref)
        func_vla_arg_code(arg->type.ref);

    if ((arg->type.t & VT_VLA) && arg->type.ref->vla_array_str) {
        loc -= type_size(&int_type, &align);
        loc &= -align;
        arg->type.ref->c = loc;

        unget_tok(0);
        vla_array_tok = tok_str_alloc();
        vla_array_tok->str = arg->type.ref->vla_array_str;
        begin_macro(vla_array_tok, 1);
        next();
        gexpr();
        end_macro();
        next();
        vpush_type_size(&arg->type.ref->type, &align);
        gen_op('*');
        vset(&int_type, VT_LOCAL | VT_LVAL, arg->type.ref->c);
        vswap();
        vstore();
        vpop();
    }
}

static void func_vla_arg(Sym *sym)
{
    Sym *arg;

    for (arg = sym->type.ref->next; arg; arg = arg->next)
        if ((arg->type.t & VT_BTYPE) == VT_PTR && (arg->type.ref->type.t & VT_VLA))
            func_vla_arg_code(arg->type.ref);
}

ST_FUNC Sym *gfunc_set_param(Sym *s, int c, int byref)
{
    s = sym_find(s->v);
    if (!s)
        return NULL;
    s->c = c;
    if (byref)
        s->r = VT_LLOCAL | VT_LVAL;
    return s;
}

static void sym_push_params(Sym *ref)
{
    Sym *s = ref;
    while (s->next)
        s = s->next;
    while (s != ref) {
        if ((s->v & ~SYM_STRUCT) < SYM_FIRST_ANOM)
            sym_copy(s, &local_stack);
        s = s->prev;
    }
}

static void gen_function(Sym *sym)
{
    struct scope f = {0};

    cur_scope = root_scope = &f;
    nocode_wanted = 0;

    ind = cur_text_section->data_offset;
    if (sym->a.aligned) {
        size_t newoff = section_add(cur_text_section, 0, 1 << (sym->a.aligned - 1));
        gen_fill_nops(newoff - ind);
    }

    funcname = get_tok_str(sym->v, NULL);
    func_ind = ind;
    func_vt = sym->type.ref->type;
    func_var = sym->type.ref->f.func_type == FUNC_ELLIPSIS;
    func_old = sym->type.ref->f.func_type == FUNC_OLD;

    put_extern_sym(sym, cur_text_section, ind, 0);

    if (sym->type.ref->f.func_ctor)
        add_array(tcc_state, ".init_array", sym->c);
    if (sym->type.ref->f.func_dtor)
        add_array(tcc_state, ".fini_array", sym->c);

    tcc_debug_funcstart(tcc_state, sym);

    sym_push2(&local_stack, SYM_FIELD, 0, 0);

    local_scope = 1;
    sym_push_params(sym->type.ref);

    local_scope = 0;
    rsym = 0;
    nb_temp_local_vars = 0;

    gfunc_prolog(sym);
    tcc_debug_prolog_epilog(tcc_state, 0);
    func_vla_arg(sym);
    block(0);
    gsym(rsym);
    nocode_wanted = 0;
    tcc_debug_end_scope(NULL, !func_var);
    tcc_debug_prolog_epilog(tcc_state, 1);
    gfunc_epilog();

    tcc_debug_funcend(tcc_state, ind - func_ind);

    elfsym(sym)->st_size = ind - func_ind;
    cur_text_section->data_offset = ind;

    sym_pop(&local_stack, NULL, 0);
    label_pop(&global_label_stack, NULL, 0);
    sym_pop(&all_cleanups, NULL, 0);
    local_scope = 0;

    cur_text_section = NULL;
    funcname = "";
    func_vt.t = VT_VOID;
    func_var = 0;
    ind = 0;
    func_ind = -1;
    nocode_wanted = DATA_ONLY_WANTED;
    check_vstack();

    next();
}

static void gen_inline_functions(TCCState *s)
{
    Sym *sym;
    int inline_generated, i;
    struct InlineFunc *fn;

    tcc_open_bf(s, ":inline:", 0);

    do {
        inline_generated = 0;
        for (i = 0; i < s->nb_inline_fns; ++i) {
            fn = s->inline_fns[i];
            sym = fn->sym;
            if (sym && (sym->c || !(sym->type.t & VT_INLINE))) {

                fn->sym = NULL;
                tccpp_putfile(fn->filename);
                begin_macro(fn->func_str, 1);
                next();
                cur_text_section = text_section;
                gen_function(sym);
                end_macro();

                inline_generated = 1;
            }
        }
    } while (inline_generated);
    tcc_close();
}

static void free_inline_functions(TCCState *s)
{
    int i;

    for (i = 0; i < s->nb_inline_fns; ++i) {
        struct InlineFunc *fn = s->inline_fns[i];
        if (fn->sym)
            tok_str_free(fn->func_str);
    }
    dynarray_reset(&s->inline_fns, &s->nb_inline_fns);
}

static void do_Static_assert(void)
{
    int c;
    const char *msg;

    next();
    skip('(');
    c = expr_const();
    msg = "_Static_assert fail";
    if (tok == ',') {
        next();
        msg = parse_mult_str("string constant")->data;
    }
    skip(')');
    if (c == 0)
        tcc_error("%s", msg);
    skip(';');
}

#ifdef TCC_TARGET_PE
static void pe_check_linkage(CType *type, AttributeDef *ad)
{
    if (!ad->a.dllimport && !ad->a.dllexport)
        return;
    if (type->t & VT_STATIC)
        tcc_error("cannot have dll linkage with static");
    if (type->t & VT_TYPEDEF) {
        const char *m = ad->a.dllimport ? "im" : "ex";
        tcc_warning("'dll%sport' attribute ignored for typedef", m);
        ad->a.dllimport = 0;
        ad->a.dllexport = 0;
    } else if (ad->a.dllimport) {
        if ((type->t & VT_BTYPE) == VT_FUNC)
            ad->a.dllimport = 0;
        else
            type->t |= VT_EXTERN;
    }
}
#endif

static int decl(int l)
{
    int v, has_init, r, oldint;
    CType type, btype;
    Sym *sym, *sa;
    AttributeDef ad, adbase;
    ElfSym *esym;

    while (1) {

        oldint = 0;
        if (!parse_btype(&btype, &adbase, l == VT_LOCAL)) {
            if (l == VT_JMP)
                return 0;

            if (tok == ';' && l != VT_CMP) {
                next();
                continue;
            }
            if (tok == TOK_STATIC_ASSERT) {
                do_Static_assert();
                continue;
            }
            if (l != VT_CONST)
                break;
            if (tok == TOK_ASM1 || tok == TOK_ASM2 || tok == TOK_ASM3) {

                asm_global_instr();
                continue;
            }
            if (tok >= TOK_UIDENT) {

                btype.t = VT_INT;
                oldint = 1;
            } else {
                if (tok != TOK_EOF)
                    expect("declaration");
                break;
            }
        }

        if (tok == ';') {
            if ((btype.t & VT_BTYPE) == VT_STRUCT && (btype.ref->v & ~SYM_STRUCT) < SYM_FIRST_ANOM)
                ;
            else if (IS_ENUM(btype.t))
                ;
            else
                tcc_warning("useless type defines no instances");
            if (l == VT_JMP)
                return 1;
            next();
            continue;
        }

        while (1) {
            type = btype;
            ad = adbase;
            type_decl(&type, &ad, &v, l == VT_CMP ? TYPE_DIRECT | TYPE_PARAM : TYPE_DIRECT);

            if ((type.t & VT_BTYPE) == VT_FUNC) {
                if ((type.t & VT_STATIC) && (l != VT_CONST))
                    tcc_error("function without file scope cannot be static");

                sym = type.ref;
                if (sym->f.func_type == FUNC_OLD && l == VT_CONST) {
                    func_vt = type;
                    ++local_scope;
                    decl(VT_CMP);
                    --local_scope;
                }
                if ((type.t & (VT_EXTERN | VT_INLINE)) == (VT_EXTERN | VT_INLINE)) {

                    if (tcc_state->gnu89_inline || sym->f.func_alwinl)
                        type.t = (type.t & ~VT_EXTERN) | VT_STATIC;
                    else
                        type.t &= ~VT_INLINE;
                }

            } else if (oldint) {
                tcc_warning("type defaults to int");
            }

            if (gnu_ext && (tok == TOK_ASM1 || tok == TOK_ASM2 || tok == TOK_ASM3)) {
                ad.asm_label = asm_label_instr();

                parse_attribute(&ad);
#if 0

                if (tok == '{')
                    expect(";");
#endif
            }

#ifdef TCC_TARGET_PE
            pe_check_linkage(&type, &ad);
#endif
            if (tok == '{') {
                if (l != VT_CONST)
                    tcc_error("cannot use local functions");
                if ((type.t & VT_BTYPE) != VT_FUNC)
                    expect("function definition");

                merge_funcattr(&type.ref->f, &ad.f);

                type.t &= ~VT_EXTERN;
                sym = external_sym(v, &type, 0, &ad);

                for (sa = sym->type.ref; (sa = sa->next) != NULL;) {
                    if (!(sa->v & ~SYM_FIELD))
                        expect("identifier");
                    if (sa->type.t == VT_FLOAT && sym->type.ref->f.func_type == FUNC_OLD) {
                        sa->type.t = VT_DOUBLE;
                    }
                }

                if (sym->type.t & VT_INLINE) {
                    struct InlineFunc *fn;
                    fn = tcc_malloc(sizeof *fn + strlen(file->filename));
                    strcpy(fn->filename, file->filename);
                    fn->sym = sym;
                    dynarray_add(&tcc_state->inline_fns, &tcc_state->nb_inline_fns, fn);
                    skip_or_save_block(&fn->func_str);
                } else {

                    cur_text_section = text_section;
                    if (ad.section) {
                        cur_text_section = ad.section;
                        if (ad.new_section)
                            ad.section->sh_flags = text_section->sh_flags;
                    }
                    gen_function(sym);
                }
                break;
            } else {
                has_init = 0;
                if (l == VT_CMP) {

                    for (sym = func_vt.ref->next; sym; sym = sym->next)
                        if ((sym->v & ~SYM_FIELD) == v)
                            goto found;
                    tcc_error("declaration for parameter '%s' but no such parameter",
                              get_tok_str(v, NULL));
                found:
                    if (type.t & VT_STORAGE)
                        tcc_error("storage class specified for '%s'", get_tok_str(v, NULL));
                    if (!(sym->type.t & VT_EXTERN))
                        tcc_error("redefinition of parameter '%s'", get_tok_str(v, NULL));
                    convert_parameter_type(&type);
                    sym->type = type;
                } else if (type.t & VT_TYPEDEF) {

                    sym = sym_find(v);
                    if (sym && sym->sym_scope == local_scope) {
                        if (!is_compatible_types(&sym->type, &type) || !(sym->type.t & VT_TYPEDEF))
                            tcc_error("incompatible redefinition of '%s'", get_tok_str(v, NULL));
                        sym->type = type;
                    } else {
                        sym = sym_push(v, &type, 0, 0);
                    }
                    sym->a = ad.a;
                    if ((type.t & VT_BTYPE) == VT_FUNC)
                        merge_funcattr(&sym->type.ref->f, &ad.f);
                    if (debug_modes)
                        tcc_debug_typedef(tcc_state, sym);
                } else if ((type.t & VT_BTYPE) == VT_VOID && !(type.t & VT_EXTERN)) {
                    tcc_error("declaration of void object");
                } else {
                    r = 0;
                    if ((type.t & VT_BTYPE) == VT_FUNC) {

                        merge_funcattr(&type.ref->f, &ad.f);
                    } else if (!(type.t & VT_ARRAY)) {

                        r |= VT_LVAL;
                    }

                    if (tok == '=')
                        has_init = 1;

                    if (((type.t & VT_EXTERN) && (!has_init || l != VT_CONST)) ||
                        (type.t & VT_BTYPE) == VT_FUNC

                        || ((type.t & VT_ARRAY) && !has_init && l == VT_CONST && type.ref->c < 0)) {

                        type.t |= VT_EXTERN;
                        external_sym(v, &type, r, &ad);
                    } else {
                        if (l == VT_CONST || (type.t & VT_STATIC))
                            r |= VT_CONST;
                        else
                            r |= VT_LOCAL;
                        type.t &= ~VT_EXTERN;
                        if (has_init)
                            next();
                        else if (l == VT_CONST)

                            type.t |= VT_EXTERN;
                        decl_initializer_alloc(&type, &ad, r, has_init, v, l);
                    }

                    if (ad.alias_target && l == VT_CONST) {

                        esym = elfsym(sym_find(ad.alias_target));
                        if (!esym)
                            tcc_error("unsupported forward __alias__ attribute");
                        put_extern_sym2(sym_find(v), esym->st_shndx, esym->st_value, esym->st_size,
                                        1);
                    }
                }
                if (tok != ',') {
                    if (l == VT_JMP)
                        return has_init ? v : 1;
                    skip(';');
                    break;
                }
                next();
            }
        }
    }
    return 0;
}

#undef gjmp_addr
#undef gjmp
