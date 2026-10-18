/*
 *  x86-64 code generator for TCC
 *
 *  Copyright (c) 2008 Shinichiro Hamaji
 *
 *  Based on i386-gen.c by Fabrice Bellard
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

#ifdef TARGET_DEFS_ONLY

#define NB_REGS 25
#define NB_ASM_REGS 16
#define CONFIG_TCC_ASM

#define RC_INT 0x0001
#define RC_FLOAT 0x0002
#define RC_RAX 0x0004
#define RC_RDX 0x0008
#define RC_RCX 0x0010
#define RC_RSI 0x0020
#define RC_RDI 0x0040
#define RC_ST0 0x0080
#define RC_R8 0x0100
#define RC_R9 0x0200
#define RC_R10 0x0400
#define RC_R11 0x0800
#define RC_XMM0 0x1000
#define RC_XMM1 0x2000
#define RC_XMM2 0x4000
#define RC_XMM3 0x8000
#define RC_XMM4 0x10000
#define RC_XMM5 0x20000
#define RC_XMM6 0x40000
#define RC_XMM7 0x80000
#define RC_IRET RC_RAX
#define RC_IRE2 RC_RDX
#define RC_FRET RC_XMM0
#define RC_FRE2 RC_XMM1

enum {
    TREG_RAX = 0,
    TREG_RCX = 1,
    TREG_RDX = 2,
    TREG_RSP = 4,
    TREG_RSI = 6,
    TREG_RDI = 7,

    TREG_R8 = 8,
    TREG_R9 = 9,
    TREG_R10 = 10,
    TREG_R11 = 11,

    TREG_XMM0 = 16,
    TREG_XMM1 = 17,
    TREG_XMM2 = 18,
    TREG_XMM3 = 19,
    TREG_XMM4 = 20,
    TREG_XMM5 = 21,
    TREG_XMM6 = 22,
    TREG_XMM7 = 23,

    TREG_ST0 = 24,

    TREG_MEM = 0x20
};

#define REX_BASE(reg) (((reg) >> 3) & 1)
#define REG_VALUE(reg) ((reg) & 7)

#define REG_IRET TREG_RAX
#define REG_IRE2 TREG_RDX
#define REG_FRET TREG_XMM0
#define REG_FRE2 TREG_XMM1

#define INVERT_FUNC_PARAMS

#define PTR_SIZE 8

#define LDOUBLE_SIZE 16
#define LDOUBLE_ALIGN 16

#define MAX_ALIGN 16

#define PROMOTE_RET

#define TCC_TARGET_NATIVE_STRUCT_COPY
ST_FUNC void gen_struct_copy(int size);

#else

#define USING_GLOBALS
#include "tcc.h"
#include <assert.h>

ST_DATA const char *const target_machine_defs = "__x86_64__\0"
                                                "__x86_64\0"
                                                "__amd64__\0";

ST_DATA const int reg_classes[NB_REGS] = {RC_INT | RC_RAX,
                                          RC_INT | RC_RCX,
                                          RC_INT | RC_RDX,
                                          0,
                                          0,
                                          0,
                                          RC_RSI,
                                          RC_RDI,
                                          RC_R8,
                                          RC_R9,
                                          RC_R10,
                                          RC_R11,
                                          0,
                                          0,
                                          0,
                                          0,
                                          RC_FLOAT | RC_XMM0,
                                          RC_FLOAT | RC_XMM1,
                                          RC_FLOAT | RC_XMM2,
                                          RC_FLOAT | RC_XMM3,
                                          RC_FLOAT | RC_XMM4,
                                          RC_FLOAT | RC_XMM5,

                                          RC_XMM6,
                                          RC_XMM7,
                                          RC_ST0};

static unsigned long func_sub_sp_offset;
static int func_ret_sub;

#if defined(CONFIG_TCC_BCHECK)
static addr_t func_bound_offset;
static unsigned long func_bound_ind;
ST_DATA int func_bound_add_epilog;
#endif


ST_FUNC void g(int c)
{
    int ind1;
    if (nocode_wanted)
        return;
    ind1 = ind + 1;
    if (ind1 > cur_text_section->data_allocated)
        section_realloc(cur_text_section, ind1);
    cur_text_section->data[ind] = c;
    ind = ind1;
}

ST_FUNC void o(unsigned int c)
{
    while (c) {
        g(c);
        c = c >> 8;
    }
}

ST_FUNC void gen_le16(int v)
{
    g(v);
    g(v >> 8);
}

ST_FUNC void gen_le32(int c)
{
    g(c);
    g(c >> 8);
    g(c >> 16);
    g(c >> 24);
}

ST_FUNC void gen_le64(int64_t c)
{
    g(c);
    g(c >> 8);
    g(c >> 16);
    g(c >> 24);
    g(c >> 32);
    g(c >> 40);
    g(c >> 48);
    g(c >> 56);
}

static void orex(int ll, int r, int r2, int b)
{
    if ((r & VT_VALMASK) >= VT_CONST)
        r = 0;
    if ((r2 & VT_VALMASK) >= VT_CONST)
        r2 = 0;
    if (ll || REX_BASE(r) || REX_BASE(r2)) {
        if ((b & 0xff) == 0x66)
            o(0x66), b >>= 8;
        o(0x40 | REX_BASE(r) | (REX_BASE(r2) << 2) | (ll << 3));
    }
    o(b);
}

ST_FUNC void gsym_addr(int t, int a)
{
    while (t) {
        unsigned char *ptr = cur_text_section->data + t;
        uint32_t n = read32le(ptr);
        write32le(ptr, a < 0 ? -a : a - t - 4);
        t = n;
    }
}

static int is64_type(int t)
{
    return ((t & VT_BTYPE) == VT_PTR || (t & VT_BTYPE) == VT_FUNC || (t & VT_BTYPE) == VT_LLONG);
}

static int oad(int c, int s)
{
    int t;
    if (nocode_wanted)
        return s;
    o(c);
    t = ind;
    gen_le32(s);
    return t;
}

#define gjmp2(instr, lbl) oad(instr, lbl)

ST_FUNC void gen_addr32(int r, Sym *sym, int c)
{
    if (r & VT_SYM)
        greloca(cur_text_section, sym, ind, R_X86_64_32S, c), c = 0;
    gen_le32(c);
}

ST_FUNC void gen_addrpc32(int r, Sym *sym, int c)
{
    if (r & VT_SYM)
        greloca(cur_text_section, sym, ind, R_X86_64_PC32, c - 4), c = 4;
    gen_le32(c - 4);
}

static void gen_gotpcrel(int r, Sym *sym, int c)
{
    greloca(cur_text_section, sym, ind, R_X86_64_GOTPCREL, -4);
    gen_le32(0);
    if (c) {

        orex(1, r, 0, 0x81);
        o(0xc0 + REG_VALUE(r));
        gen_le32(c);
    }
}

static void gen_modrm_impl(int opcode, int ll, int op_reg_0, int r, Sym *sym, int c)
{
    int op_reg = REG_VALUE(op_reg_0) << 3;

    if ((r & VT_SYM) && (sym->type.t & VT_TLS)) {
        o(0x64);
        orex(ll, r, op_reg_0, opcode);
        o(0x04 | op_reg);
        oad(0x25, 0);
        greloca(cur_text_section, sym, ind - 4, R_X86_64_TPOFF32, c);
        return;
    }

    orex(ll, r, op_reg_0, opcode);

    if ((r & VT_VALMASK) == VT_CONST) {

        if (!(r & VT_SYM)) {

            o(0x04 | op_reg);
            oad(0x25, c);
        } else {
            o(0x05 | op_reg);
            if (op_reg_0 & TREG_MEM) {
                gen_gotpcrel(r, sym, c);
            } else {
                gen_addrpc32(r, sym, c);
            }
        }
    } else if ((r & VT_VALMASK) == VT_LOCAL) {

        if (c == (signed char)c) {

            o(0x45 | op_reg);
            g(c);
        } else {
            oad(0x85 | op_reg, c);
        }

    } else if ((r & TREG_MEM) && c) {
        g(0x80 | op_reg | REG_VALUE(r));
        gen_le32(c);
    } else if (r & VT_LVAL) {
        g(0x00 | op_reg | REG_VALUE(r));
    } else {
        g(0xc0 | op_reg | REG_VALUE(r));
    }
}

static void gen_modrm64(int opcode, int op_reg, int r, Sym *sym, int c)
{
    gen_modrm_impl(opcode, 1, op_reg, r, sym, c);
}

static void gen_modrm32(int opcode, int op_reg, int r, Sym *sym, int c)
{
    gen_modrm_impl(opcode, 0, op_reg, r, sym, c);
}

void load(int r, SValue *sv)
{
    int v, t, ft, fc, fr;
    SValue v1;

    fr = sv->r;
    ft = sv->type.t & ~(VT_DEFSIGN | VT_QUAL);
    fc = sv->c.i;

    if (fc != sv->c.i && (fr & VT_SYM))
        tcc_error("64 bit addend in load");


    if ((fr & (VT_VALMASK | VT_SYM | VT_LVAL)) == (VT_CONST | VT_SYM | VT_LVAL) &&
        !(sv->sym->type.t & (VT_STATIC | VT_TLS))) {

        int tr = r | TREG_MEM;
        if (is_float(ft)) {

            tr = get_reg(RC_INT) | TREG_MEM;
        }
        gen_modrm64(0x8b, tr, fr, sv->sym, 0);

        fr = tr | VT_LVAL;
    }

    v = fr & VT_VALMASK;
    if (fr & VT_LVAL) {
        int b, ll;
        if (v == VT_LLOCAL) {
            v1.type.t = VT_PTR;
            v1.r = VT_LOCAL | VT_LVAL;
            v1.c.i = fc;
            v1.sym = NULL;
            fr = r;
            if (!(reg_classes[fr] & (RC_INT | RC_R11)))
                fr = get_reg(RC_INT);
            load(fr, &v1);
            fr |= VT_LVAL;
        }
        if (fc != sv->c.i) {

            v1.type.t = VT_LLONG;
            v1.r = VT_CONST;
            v1.c.i = sv->c.i;
            v1.sym = NULL;
            fr = r;
            if (!(reg_classes[fr] & (RC_INT | RC_R11)))
                fr = get_reg(RC_INT);
            load(fr, &v1);
            fc = 0;
        }
        ll = 0;

        if ((ft & VT_BTYPE) == VT_STRUCT) {
            int align;
            switch (type_size(&sv->type, &align)) {
                case 1:
                    ft = VT_BYTE;
                    break;
                case 2:
                    ft = VT_SHORT;
                    break;
                case 4:
                    ft = VT_INT;
                    break;
                case 8:
                    ft = VT_LLONG;
                    break;
                default:
                    tcc_error("invalid aggregate type for register load");
                    break;
            }
        }
        if ((ft & VT_BTYPE) == VT_FLOAT) {
            b = 0x6e0f66;
            r = REG_VALUE(r);
        } else if ((ft & VT_BTYPE) == VT_DOUBLE) {
            b = 0x7e0ff3;
            r = REG_VALUE(r);
        } else if ((ft & VT_BTYPE) == VT_LDOUBLE) {
            b = 0xdb, r = 5;
        } else if ((ft & VT_TYPE) == VT_BYTE || (ft & VT_TYPE) == VT_BOOL) {
            b = 0xbe0f;
        } else if ((ft & VT_TYPE) == (VT_BYTE | VT_UNSIGNED)) {
            b = 0xb60f;
        } else if ((ft & VT_TYPE) == VT_SHORT) {
            b = 0xbf0f;
        } else if ((ft & VT_TYPE) == (VT_SHORT | VT_UNSIGNED)) {
            b = 0xb70f;
        } else if ((ft & VT_TYPE) == (VT_VOID)) {

            return;
        } else {
            assert(((ft & VT_BTYPE) == VT_INT) || ((ft & VT_BTYPE) == VT_LLONG) ||
                   ((ft & VT_BTYPE) == VT_PTR) || ((ft & VT_BTYPE) == VT_FUNC));
            ll = is64_type(ft);
            b = 0x8b;
        }
        gen_modrm_impl(b, ll, r, fr, sv->sym, fc);
    } else {
        if (v == VT_CONST) {
            if (fr & VT_SYM) {
                if (sv->sym->type.t & VT_TLS) {

                    o(0x64), gen_modrm64(0x8b, r, VT_CONST, NULL, 0);

                    orex(1, 0, r, 0x81), oad(0xC0 | REG_VALUE(r), 0);
                    greloca(cur_text_section, sv->sym, ind - 4, R_X86_64_TPOFF32, fc);
                } else if (sv->sym->type.t & VT_STATIC) {
                    orex(1, 0, r, 0x8d);
                    o(0x05 + REG_VALUE(r) * 8);
                    gen_addrpc32(fr, sv->sym, fc);
                } else {
                    orex(1, 0, r, 0x8b);
                    o(0x05 + REG_VALUE(r) * 8);
                    gen_gotpcrel(r, sv->sym, fc);
                }
            } else if (is64_type(ft)) {
                if (sv->c.i >> 32) {
                    orex(1, r, 0, 0xb8 + REG_VALUE(r));
                    gen_le64(sv->c.i);
                } else if (sv->c.i > 0) {
                    orex(0, r, 0, 0xb8 + REG_VALUE(r));
                    gen_le32(sv->c.i);
                } else {
                    orex(0, r, r, 0x31);
                    o(0xc0 + REG_VALUE(r) * 9);
                }
            } else {
                orex(0, r, 0, 0xb8 + REG_VALUE(r));
                gen_le32(fc);
            }
        } else if (v == VT_LOCAL) {
            gen_modrm64(0x8d, r, VT_LOCAL, sv->sym, fc);
        } else if (v == VT_CMP) {
            if (fc & 0x100) {
                v = vtop->cmp_r;
                fc &= ~0x100;

                orex(0, r, 0, 0xb0 + REG_VALUE(r));
                g(v ^ fc ^ (v == TOK_NE));
                o(0x037a + (REX_BASE(r) << 8));
            }
            orex(0, r, 0, 0x0f);
            o(fc);
            o(0xc0 + REG_VALUE(r));
            orex(0, r, 0, 0x0f);
            o(0xc0b6 + REG_VALUE(r) * 0x900);
        } else if (v == VT_JMP || v == VT_JMPI) {
            t = v & 1;
            orex(0, r, 0, 0);
            oad(0xb8 + REG_VALUE(r), t);
            o(0x05eb + (REX_BASE(r) << 8));
            gsym(fc);
            orex(0, r, 0, 0);
            oad(0xb8 + REG_VALUE(r), t ^ 1);
        } else if (v != r) {
            if ((r >= TREG_XMM0) && (r <= TREG_XMM7)) {
                if (v == TREG_ST0) {

                    o(0xf0245cdd);

                    o(0x100ff2);
                    o(0x44 + REG_VALUE(r) * 8);
                    o(0xf024);
                } else {
                    assert((v >= TREG_XMM0) && (v <= TREG_XMM7));
                    if ((ft & VT_BTYPE) == VT_FLOAT) {
                        o(0x100ff3);
                    } else {
                        assert((ft & VT_BTYPE) == VT_DOUBLE);
                        o(0x100ff2);
                    }
                    o(0xc0 + REG_VALUE(v) + REG_VALUE(r) * 8);
                }
            } else if (r == TREG_ST0) {
                assert((v >= TREG_XMM0) && (v <= TREG_XMM7));

                o(0x110ff2);
                o(0x44 + REG_VALUE(r) * 8);
                o(0xf024);
                o(0xf02444dd);
            } else {
                orex(is64_type(ft), r, v, 0x89);
                o(0xc0 + REG_VALUE(r) + REG_VALUE(v) * 8);
            }
        }
    }
}

void store(int r, SValue *v)
{
    int fr, bt, fc;

    int opc = 0, ll = 0;

    fr = v->r;
    fc = v->c.i;
    if (fc != v->c.i && (fr & VT_SYM))
        tcc_error("64 bit addend in store");
    bt = v->type.t & VT_BTYPE;


    if ((fr & (VT_VALMASK | VT_SYM)) == (VT_CONST | VT_SYM) &&
        !(v->sym->type.t & (VT_STATIC | VT_TLS))) {

        o(0x1d8b4c);
        gen_gotpcrel(TREG_R11, v->sym, v->c.i);
        fr = TREG_R11 | VT_LVAL;
    }

    if (bt == VT_FLOAT) {
        opc = 0x7e0f66;
        r = REG_VALUE(r);
    } else if (bt == VT_DOUBLE) {
        opc = 0xd60f66;
        r = REG_VALUE(r);
    } else if (bt == VT_LDOUBLE) {
        o(0xc0d9);
        opc = 0xdb;
        r = 7;
    } else if (bt == VT_BYTE || bt == VT_BOOL) {
        opc = 0x88;
    } else {
        opc = 0x89;
        if (bt == VT_SHORT)
            opc = 0x8966;
        else if (is64_type(bt))
            ll = 1;
    }
    gen_modrm_impl(opc, ll, r, fr, v->sym, fc);
}

static void gcall_or_jmp(int is_jmp)
{
    int r;
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST &&
        ((vtop->r & VT_SYM) && (vtop->c.i - 4) == (int)(vtop->c.i - 4))) {

        greloca(cur_text_section, vtop->sym, ind + 1, R_X86_64_PLT32, (int)(vtop->c.i - 4));
        oad(0xe8 + is_jmp, 0);
    } else {

        r = TREG_R11;
        load(r, vtop);
        o(0x41);
        o(0xff);
        o(0xd0 + REG_VALUE(r) + (is_jmp << 4));
    }
}

#if defined(CONFIG_TCC_BCHECK)

static void gen_bounds_call(int v)
{
    Sym *sym = external_helper_sym(v);
    oad(0xe8, 0);
    greloca(cur_text_section, sym, ind - 4, R_X86_64_PLT32, -4);
}

#define TREG_FASTCALL_1 TREG_RDI

static void gen_bounds_prolog(void)
{

    func_bound_offset = lbounds_section->data_offset;
    func_bound_ind = ind;
    func_bound_add_epilog = 0;
    o(0x0d8d48 + ((TREG_FASTCALL_1 == TREG_RDI) * 0x300000));
    gen_le32(0);
    oad(0xb8, 0);
}

static void gen_bounds_epilog(void)
{
    addr_t saved_ind;
    addr_t *bounds_ptr;
    Sym *sym_data;
    int offset_modified = func_bound_offset != lbounds_section->data_offset;

    if (!offset_modified && !func_bound_add_epilog)
        return;

    bounds_ptr = section_ptr_add(lbounds_section, sizeof(addr_t));
    *bounds_ptr = 0;

    sym_data = get_sym_ref(&char_pointer_type, lbounds_section, func_bound_offset, PTR_SIZE);

    if (offset_modified) {
        saved_ind = ind;
        ind = func_bound_ind;
        greloca(cur_text_section, sym_data, ind + 3, R_X86_64_PC32, -4);
        ind = ind + 7;
        gen_bounds_call(TOK___bound_local_new);
        ind = saved_ind;
    }

    o(0x5250);
    o(0x20ec8348);
    o(0x290f);
    o(0x102444);
    o(0x240c290f);
    greloca(cur_text_section, sym_data, ind + 3, R_X86_64_PC32, -4);
    o(0x0d8d48 + ((TREG_FASTCALL_1 == TREG_RDI) * 0x300000));
    gen_le32(0);
    gen_bounds_call(TOK___bound_local_delete);
    o(0x280f);
    o(0x102444);
    o(0x240c280f);
    o(0x20c48348);
    o(0x585a);
}
#endif


static void gadd_sp(int val)
{
    if (val == (signed char)val) {
        o(0xc48348);
        g(val);
    } else {
        oad(0xc48148, val);
    }
}

typedef enum X86_64_Mode {
    x86_64_mode_none,
    x86_64_mode_memory,
    x86_64_mode_integer,
    x86_64_mode_sse,
    x86_64_mode_x87
} X86_64_Mode;

static X86_64_Mode classify_x86_64_merge(X86_64_Mode a, X86_64_Mode b)
{
    if (a == b)
        return a;
    else if (a == x86_64_mode_none)
        return b;
    else if (b == x86_64_mode_none)
        return a;
    else if ((a == x86_64_mode_memory) || (b == x86_64_mode_memory))
        return x86_64_mode_memory;
    else if ((a == x86_64_mode_integer) || (b == x86_64_mode_integer))
        return x86_64_mode_integer;
    else if ((a == x86_64_mode_x87) || (b == x86_64_mode_x87))
        return x86_64_mode_memory;
    else
        return x86_64_mode_sse;
}

static X86_64_Mode classify_x86_64_inner(CType *ty)
{
    X86_64_Mode mode;
    Sym *f;

    switch (ty->t & VT_BTYPE) {
        case VT_VOID:
            return x86_64_mode_none;

        case VT_INT:
        case VT_BYTE:
        case VT_SHORT:
        case VT_LLONG:
        case VT_BOOL:
        case VT_PTR:
        case VT_FUNC:
            return x86_64_mode_integer;

        case VT_FLOAT:
        case VT_DOUBLE:
            return x86_64_mode_sse;

        case VT_LDOUBLE:
            return x86_64_mode_x87;

        case VT_STRUCT:
            f = ty->ref;

            mode = x86_64_mode_none;
            for (f = f->next; f; f = f->next)
                mode = classify_x86_64_merge(mode, classify_x86_64_inner(&f->type));

            return mode;
    }
    assert(0);
    return 0;
}

static X86_64_Mode classify_x86_64_arg(CType *ty, CType *ret, int *psize, int *palign,
                                       int *reg_count)
{
    X86_64_Mode mode;
    int size, align, ret_t = 0;

    if (ty->t & (VT_BITFIELD | VT_ARRAY)) {
        *psize = 8;
        *palign = 8;
        *reg_count = 1;
        ret_t = ty->t;
        mode = x86_64_mode_integer;
    } else {
        size = type_size(ty, &align);
        *psize = (size + 7) & ~7;
        *palign = (align + 7) & ~7;
        *reg_count = 0;

        if (size > 16) {
            mode = x86_64_mode_memory;
        } else {
            mode = classify_x86_64_inner(ty);
            switch (mode) {
                case x86_64_mode_integer:
                    if (size > 8) {
                        *reg_count = 2;
                        ret_t = VT_QLONG;
                    } else {
                        *reg_count = 1;
                        if (size > 4)
                            ret_t = VT_LLONG;
                        else if (size > 2)
                            ret_t = VT_INT;
                        else if (size > 1)
                            ret_t = VT_SHORT;
                        else
                            ret_t = VT_BYTE;
                        if ((ty->t & VT_BTYPE) == VT_STRUCT || (ty->t & VT_UNSIGNED))
                            ret_t |= VT_UNSIGNED;
                    }
                    break;

                case x86_64_mode_x87:
                    *reg_count = 1;
                    ret_t = VT_LDOUBLE;
                    break;

                case x86_64_mode_sse:
                    if (size > 8) {
                        *reg_count = 2;
                        ret_t = VT_QFLOAT;
                    } else {
                        *reg_count = 1;
                        ret_t = (size > 4) ? VT_DOUBLE : VT_FLOAT;
                    }
                    break;
                default:
                    break;
            }
        }
    }

    if (ret) {
        ret->ref = NULL;
        ret->t = ret_t;
    }

    return mode;
}

ST_FUNC int classify_x86_64_va_arg(CType *ty)
{

    enum __va_arg_type { __va_gen_reg, __va_float_reg, __va_stack };
    int size, align, reg_count;
    X86_64_Mode mode = classify_x86_64_arg(ty, NULL, &size, &align, &reg_count);
    switch (mode) {
        default:
            return __va_stack;
        case x86_64_mode_integer:
            return __va_gen_reg;
        case x86_64_mode_sse:
            return __va_float_reg;
    }
}

ST_FUNC int gfunc_sret(CType *vt, int variadic, CType *ret, int *ret_align, int *regsize)
{
    int size, align, reg_count;
    if (classify_x86_64_arg(vt, ret, &size, &align, &reg_count) == x86_64_mode_memory)
        return 0;
    *ret_align = 1;
    *regsize = 8 * reg_count;
    return 1;
}

#define REGN 6
static const uint8_t arg_regs[REGN] = {TREG_RDI, TREG_RSI, TREG_RDX, TREG_RCX, TREG_R8, TREG_R9};

static int arg_prepare_reg(int idx)
{
    if (idx == 2 || idx == 3)

        return idx + 8;
    else
        return idx >= 0 && idx < REGN ? arg_regs[idx] : 0;
}

void gfunc_call(int nb_args)
{
    X86_64_Mode mode;
    CType type;
    int size, align, r, args_size, stack_adjust, i, reg_count, k;
    int nb_reg_args = 0;
    int nb_sse_args = 0;
    int sse_reg, gen_reg;
    char *onstack = tcc_malloc((nb_args + 1) * sizeof(char));

#ifdef CONFIG_TCC_BCHECK
    if (tcc_state->do_bounds_check)
        gbound_args(nb_args);
#endif

    save_regs(nb_args);

    stack_adjust = 0;
    for (i = nb_args - 1; i >= 0; i--) {
        mode = classify_x86_64_arg(&vtop[-i].type, NULL, &size, &align, &reg_count);
        if (size == 0)
            continue;
        if (mode == x86_64_mode_sse && nb_sse_args + reg_count <= 8) {
            nb_sse_args += reg_count;
            onstack[i] = 0;
        } else if (mode == x86_64_mode_integer && nb_reg_args + reg_count <= REGN) {
            nb_reg_args += reg_count;
            onstack[i] = 0;
        } else if (mode == x86_64_mode_none) {
            onstack[i] = 0;
        } else {
            if (align == 16 && (stack_adjust &= 15)) {
                onstack[i] = 2;
                stack_adjust = 0;
            } else
                onstack[i] = 1;
            stack_adjust += size;
        }
    }

    if (nb_sse_args && tcc_state->nosse)
        tcc_error("SSE disabled but floating point arguments passed");

    gen_reg = nb_reg_args;
    sse_reg = nb_sse_args;
    args_size = 0;
    stack_adjust &= 15;
    for (i = k = 0; i < nb_args;) {
        mode = classify_x86_64_arg(&vtop[-i].type, NULL, &size, &align, &reg_count);
        if (size) {
            if (!onstack[i + k]) {
                ++i;
                continue;
            }

            if (stack_adjust) {
                o(0x50);
                args_size += 8;
                stack_adjust = 0;
            }
            if (onstack[i + k] == 2)
                stack_adjust = 1;
        }

        vrotb(i + 1);

        switch (vtop->type.t & VT_BTYPE) {
            case VT_STRUCT:

                o(0x48);
                oad(0xec81, size);

                r = get_reg(RC_INT);
                orex(1, r, 0, 0x89);
                o(0xe0 + REG_VALUE(r));
                vset(&vtop->type, r | VT_LVAL, 0);
                vswap();

                o(0x10ec8348);
                o(0xf0e48348);
                orex(0, r, 0, 0x50 + REG_VALUE(r));
                o(0x08ec8348);
                vstore();
                o(0x08c48348);
                o(0x5c);
                break;

            case VT_LDOUBLE:
                gv(RC_ST0);
                oad(0xec8148, size);
                o(0x7cdb);
                g(0x24);
                g(0x00);
                break;

            case VT_FLOAT:
            case VT_DOUBLE:
                assert(mode == x86_64_mode_sse);
                r = gv(RC_FLOAT);
                o(0x50);

                o(0xd60f66);
                o(0x04 + REG_VALUE(r) * 8);
                o(0x24);
                break;

            default:
                assert(mode == x86_64_mode_integer);

                r = gv(RC_INT);
                orex(0, r, 0, 0x50 + REG_VALUE(r));
                break;
        }
        args_size += size;

        vpop();
        --nb_args;
        k++;
    }

    tcc_free(onstack);

    assert(gen_reg <= REGN);
    assert(sse_reg <= 8);
    for (i = 0; i < nb_args; i++) {
        mode = classify_x86_64_arg(&vtop->type, &type, &size, &align, &reg_count);
        if (size == 0)
            continue;

        vtop->type = type;
        if (mode == x86_64_mode_sse) {
            if (reg_count == 2) {
                sse_reg -= 2;
                gv(RC_FRET);
                if (sse_reg) {

                    o(0x280f);
                    o(0xc1 + ((sse_reg + 1) << 3));

                    o(0x280f);
                    o(0xc0 + (sse_reg << 3));
                }
            } else {
                assert(reg_count == 1);
                --sse_reg;

                gv(RC_XMM0 << sse_reg);
            }
        } else if (mode == x86_64_mode_integer) {

            int d;
            gen_reg -= reg_count;
            r = gv(RC_INT);
            d = arg_prepare_reg(gen_reg);
            orex(1, d, r, 0x89);
            o(0xc0 + REG_VALUE(r) * 8 + REG_VALUE(d));
            if (reg_count == 2) {
                d = arg_prepare_reg(gen_reg + 1);
                orex(1, d, vtop->r2, 0x89);
                o(0xc0 + REG_VALUE(vtop->r2) * 8 + REG_VALUE(d));
            }
        }
        vtop--;
    }
    assert(gen_reg == 0);
    assert(sse_reg == 0);

    if (nb_reg_args > 2) {
        o(0xd2894c);
        if (nb_reg_args > 3) {
            o(0xd9894c);
        }
    }

    if (vtop->type.ref->f.func_type != FUNC_NEW)
        oad(0xb8, nb_sse_args < 8 ? nb_sse_args : 8);
    gcall_or_jmp(0);
    if (args_size)
        gadd_sp(args_size);
    vtop--;
}

#define FUNC_PROLOG_SIZE 11

static void push_arg_reg(int i)
{
    loc -= 8;
    gen_modrm64(0x89, arg_regs[i], VT_LOCAL, NULL, loc);
}

void gfunc_prolog(Sym *func_sym)
{
    CType *func_type = &func_sym->type;
    X86_64_Mode mode, ret_mode;
    int i, addr, align, size, reg_count;
    int param_addr = 0, reg_param_index, sse_param_index;
    Sym *sym;
    CType *type;

    sym = func_type->ref;
    addr = PTR_SIZE * 2;
    loc = 0;
    ind += FUNC_PROLOG_SIZE;
    func_sub_sp_offset = ind;
    func_ret_sub = 0;
    ret_mode = classify_x86_64_arg(&func_vt, NULL, &size, &align, &reg_count);

    if (func_var) {
        int seen_reg_num, seen_sse_num, seen_stack_size;
        seen_reg_num = ret_mode == x86_64_mode_memory;
        seen_sse_num = 0;

        seen_stack_size = PTR_SIZE * 2;

        sym = func_type->ref;
        while ((sym = sym->next) != NULL) {
            type = &sym->type;
            mode = classify_x86_64_arg(type, NULL, &size, &align, &reg_count);
            switch (mode) {
                default:
                stack_arg:
                    seen_stack_size = ((seen_stack_size + align - 1) & -align) + size;
                    break;

                case x86_64_mode_integer:
                    if (seen_reg_num + reg_count > REGN)
                        goto stack_arg;
                    seen_reg_num += reg_count;
                    break;

                case x86_64_mode_sse:
                    if (seen_sse_num + reg_count > 8)
                        goto stack_arg;
                    seen_sse_num += reg_count;
                    break;
            }
        }

        loc -= 24;

        o(0xe845c7);
        gen_le32(seen_reg_num * 8);

        o(0xec45c7);
        gen_le32(seen_sse_num * 16 + 48);

        o(0x9d8d4c);
        gen_le32(seen_stack_size);

        o(0xf05d894c);

        o(0x9d8d4c);
        gen_le32(-176 - 24);

        o(0xf85d894c);

        for (i = 0; i < 8; i++) {
            loc -= 16;
            if (!tcc_state->nosse) {

                gen_modrm32(0xd60f66, 7 - i, VT_LOCAL, NULL, loc);
            }

            o(0x85c748);
            gen_le32(loc + 8);
            gen_le32(0);
        }
        for (i = 0; i < REGN; i++) {
            push_arg_reg(REGN - 1 - i);
        }
    }

    sym = func_type->ref;
    reg_param_index = 0;
    sse_param_index = 0;

    if (ret_mode == x86_64_mode_memory) {
        push_arg_reg(reg_param_index);
        func_vc = loc;
        reg_param_index++;
    }

    while ((sym = sym->next) != NULL) {
        type = &sym->type;
        mode = classify_x86_64_arg(type, NULL, &size, &align, &reg_count);
        switch (mode) {
            case x86_64_mode_sse:
                if (tcc_state->nosse)
                    tcc_error("SSE disabled but floating point arguments used");
                if (sse_param_index + reg_count <= 8) {

                    loc -= reg_count * 8;
                    param_addr = loc;
                    for (i = 0; i < reg_count; ++i) {
                        gen_modrm32(0xd60f66, sse_param_index, VT_LOCAL, NULL, param_addr + i * 8);
                        ++sse_param_index;
                    }
                } else {
                    addr = (addr + align - 1) & -align;
                    param_addr = addr;
                    addr += size;
                }
                break;

            case x86_64_mode_memory:
            case x86_64_mode_x87:
                addr = (addr + align - 1) & -align;
                param_addr = addr;
                addr += size;
                break;

            case x86_64_mode_integer: {
                if (reg_param_index + reg_count <= REGN) {

                    loc -= reg_count * 8;
                    param_addr = loc;
                    for (i = 0; i < reg_count; ++i) {
                        gen_modrm64(0x89, arg_regs[reg_param_index], VT_LOCAL, NULL,
                                    param_addr + i * 8);
                        ++reg_param_index;
                    }
                } else {
                    addr = (addr + align - 1) & -align;
                    param_addr = addr;
                    addr += size;
                }
                break;
            }
            default:
                break;
        }
        gfunc_set_param(sym, param_addr, 0);
    }

#ifdef CONFIG_TCC_BCHECK
    if (tcc_state->do_bounds_check)
        gen_bounds_prolog();
#endif
}

void gfunc_epilog(void)
{
    int v, saved_ind;

#ifdef CONFIG_TCC_BCHECK
    if (tcc_state->do_bounds_check)
        gen_bounds_epilog();
#endif
    o(0xc9);
    if (func_ret_sub == 0) {
        o(0xc3);
    } else {
        o(0xc2);
        g(func_ret_sub);
        g(func_ret_sub >> 8);
    }

    v = (-loc + 15) & -16;
    saved_ind = ind;
    ind = func_sub_sp_offset - FUNC_PROLOG_SIZE;
    o(0xe5894855);
    o(0xec8148);
    gen_le32(v);
    ind = saved_ind;
}


ST_FUNC void gen_fill_nops(int bytes)
{
    while (bytes--)
        g(0x90);
}

int gjmp(int t)
{
    return gjmp2(0xe9, t);
}

void gjmp_addr(int a)
{
    int r;
    r = a - ind - 2;
    if (r == (signed char)r) {
        g(0xeb);
        g(r);
    } else {
        oad(0xe9, a - ind - 5);
    }
}

ST_FUNC int gjmp_append(int n, int t)
{
    void *p;

    if (n) {
        uint32_t n1 = n, n2;
        while ((n2 = read32le(p = cur_text_section->data + n1)))
            n1 = n2;
        write32le(p, t);
        t = n;
    }
    return t;
}

ST_FUNC int gjmp_cond(int op, int t)
{
    if (op & 0x100) {

        int v = vtop->cmp_r;
        op &= ~0x100;
        if (op ^ v ^ (v != TOK_NE))
            o(0x067a);
        else {
            g(0x0f);
            t = gjmp2(0x8a, t);
        }
    }
    g(0x0f);
    t = gjmp2(op - 16, t);
    return t;
}

void gen_opi(int op)
{
    int r, fr, opc, c;
    int ll, uu, cc;

    ll = is64_type(vtop[-1].type.t);
    uu = (vtop[-1].type.t & VT_UNSIGNED) != 0;
    cc = (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;

    switch (op) {
        case '+':
        case TOK_ADDC1:
            opc = 0;
        gen_op8:
            if (cc && (!ll || (int)vtop->c.i == vtop->c.i)) {

                vswap();
                r = gv(RC_INT);
                vswap();
                c = vtop->c.i;
                if (c == (signed char)c) {

                    orex(ll, r, 0, 0x83);
                    o(0xc0 | (opc << 3) | REG_VALUE(r));
                    g(c);
                } else {
                    orex(ll, r, 0, 0x81);
                    oad(0xc0 | (opc << 3) | REG_VALUE(r), c);
                }
            } else {
                gv2(RC_INT, RC_INT);
                r = vtop[-1].r;
                fr = vtop[0].r;
                orex(ll, r, fr, (opc << 3) | 0x01);
                o(0xc0 + REG_VALUE(r) + REG_VALUE(fr) * 8);
            }
            vtop--;
            if (op >= TOK_ULT && op <= TOK_GT)
                vset_VT_CMP(op);
            break;
        case '-':
        case TOK_SUBC1:
            opc = 5;
            goto gen_op8;
        case TOK_ADDC2:
            opc = 2;
            goto gen_op8;
        case TOK_SUBC2:
            opc = 3;
            goto gen_op8;
        case '&':
            opc = 4;
            goto gen_op8;
        case '^':
            opc = 6;
            goto gen_op8;
        case '|':
            opc = 1;
            goto gen_op8;
        case '*':
            gv2(RC_INT, RC_INT);
            r = vtop[-1].r;
            fr = vtop[0].r;
            orex(ll, fr, r, 0xaf0f);
            o(0xc0 + REG_VALUE(fr) + REG_VALUE(r) * 8);
            vtop--;
            break;
        case TOK_SHL:
            opc = 4;
            goto gen_shift;
        case TOK_SHR:
            opc = 5;
            goto gen_shift;
        case TOK_SAR:
            opc = 7;
        gen_shift:
            opc = 0xc0 | (opc << 3);
            if (cc) {

                vswap();
                r = gv(RC_INT);
                vswap();
                orex(ll, r, 0, 0xc1);
                o(opc | REG_VALUE(r));
                g(vtop->c.i & (ll ? 63 : 31));
            } else {

                gv2(RC_INT, RC_RCX);
                r = vtop[-1].r;
                orex(ll, r, 0, 0xd3);
                o(opc | REG_VALUE(r));
            }
            vtop--;
            break;
        case TOK_UDIV:
        case TOK_UMOD:
            uu = 1;
            goto divmod;
        case '/':
        case '%':
        case TOK_PDIV:
            uu = 0;
        divmod:

            gv2(RC_RAX, RC_RCX);
            r = vtop[-1].r;
            fr = vtop[0].r;
            vtop--;
            save_reg(TREG_RDX);
            orex(ll, 0, 0, uu ? 0xd231 : 0x99);
            orex(ll, fr, 0, 0xf7);
            o((uu ? 0xf0 : 0xf8) + REG_VALUE(fr));
            if (op == '%' || op == TOK_UMOD)
                r = TREG_RDX;
            else
                r = TREG_RAX;
            vtop->r = r;
            break;
        default:
            opc = 7;
            goto gen_op8;
    }
}

void gen_opl(int op)
{
    gen_opi(op);
}

void gen_opf(int op)
{
    int a, ft, fc, swapped, r, opc;
    int bt = vtop->type.t & VT_BTYPE;
    int float_type = bt == VT_LDOUBLE ? RC_ST0 : RC_FLOAT;

    if (op == TOK_NEG) {
        gv(float_type);
        if (float_type == RC_ST0) {
            o(0xe0d9);
        } else {
            save_reg(vtop->r);

            gen_modrm32(0x80, 6, vtop->r, NULL, vtop->c.i + (bt == VT_DOUBLE ? 7 : 3));
            o(0x80);
            gv(float_type);
        }
        return;
    }

    if ((vtop[-1].r & (VT_VALMASK | VT_LVAL)) == VT_CONST) {
        vswap();
        gv(float_type);
        vswap();
    }
    if ((vtop[0].r & (VT_VALMASK | VT_LVAL)) == VT_CONST)
        gv(float_type);

    if ((vtop[-1].r & VT_LVAL) && (vtop[0].r & VT_LVAL)) {
        vswap();
        gv(float_type);
        vswap();
    }
    swapped = 0;

    if (vtop[-1].r & VT_LVAL) {
        vswap();
        swapped = 1;
    }
    if ((vtop->type.t & VT_BTYPE) == VT_LDOUBLE) {
        if (op >= TOK_ULT && op <= TOK_GT) {

            load(TREG_ST0, vtop);
            save_reg(TREG_RAX);
            if (op == TOK_GE || op == TOK_GT)
                swapped = !swapped;
            else if (op == TOK_EQ || op == TOK_NE)
                swapped = 0;
            if (swapped)
                o(0xc9d9);
            if (op == TOK_EQ || op == TOK_NE)
                o(0xe9da);
            else
                o(0xd9de);
            o(0xe0df);
            if (op == TOK_EQ) {
                o(0x45e480);
                o(0x40fC80);
            } else if (op == TOK_NE) {
                o(0x45e480);
                o(0x40f480);
                op = TOK_NE;
            } else if (op == TOK_GE || op == TOK_LE) {
                o(0x05c4f6);
                op = TOK_EQ;
            } else {
                o(0x45c4f6);
                op = TOK_EQ;
            }
            vtop--;
            vset_VT_CMP(op);
        } else {

            load(TREG_ST0, vtop);
            swapped = !swapped;

            switch (op) {
                default:
                case '+':
                    a = 0;
                    break;
                case '-':
                    a = 4;
                    if (swapped)
                        a++;
                    break;
                case '*':
                    a = 1;
                    break;
                case '/':
                    a = 6;
                    if (swapped)
                        a++;
                    break;
            }
            ft = vtop->type.t;
            fc = vtop->c.i;
            o(0xde);
            o(0xc1 + (a << 3));
            vtop--;
        }
    } else {
        if (op >= TOK_ULT && op <= TOK_GT) {

            r = vtop->r;
            fc = vtop->c.i;
            if ((r & VT_VALMASK) == VT_LLOCAL) {
                SValue v1;
                r = get_reg(RC_INT);
                v1.type.t = VT_PTR;
                v1.r = VT_LOCAL | VT_LVAL;
                v1.c.i = fc;
                v1.sym = NULL;
                load(r, &v1);
                fc = 0;
                vtop->r = r = r | VT_LVAL;
            }

            if (op == TOK_EQ || op == TOK_NE) {
                swapped = 0;
            } else {
                if (op == TOK_LE || op == TOK_LT)
                    swapped = !swapped;
                if (op == TOK_LE || op == TOK_GE) {
                    op = 0x93;
                } else {
                    op = 0x97;
                }
            }

            if (swapped) {
                gv(RC_FLOAT);
                vswap();
            }
            assert(!(vtop[-1].r & VT_LVAL));

            if (op == TOK_EQ || op == TOK_NE)
                opc = 0x2e0f;
            else
                opc = 0x2f0f;
            if ((vtop->type.t & VT_BTYPE) == VT_DOUBLE)
                opc = opc << 8 | 0x66;

            gen_modrm32(opc, vtop[-1].r, vtop->r, vtop->sym, fc);
            vtop--;
            vset_VT_CMP(op | 0x100);
            vtop->cmp_r = op;
        } else {
            assert((vtop->type.t & VT_BTYPE) != VT_LDOUBLE);
            switch (op) {
                default:
                case '+':
                    a = 0;
                    break;
                case '-':
                    a = 4;
                    break;
                case '*':
                    a = 1;
                    break;
                case '/':
                    a = 6;
                    break;
            }
            ft = vtop->type.t;
            fc = vtop->c.i;
            assert((ft & VT_BTYPE) != VT_LDOUBLE);

            if ((vtop->r & VT_VALMASK) == VT_LLOCAL) {
                SValue v1;
                r = get_reg(RC_INT);
                v1.type.t = VT_PTR;
                v1.r = VT_LOCAL | VT_LVAL;
                v1.c.i = fc;
                v1.sym = NULL;
                load(r, &v1);
                fc = 0;
                vtop->r = r | VT_LVAL;
            }

            assert(!(vtop[-1].r & VT_LVAL));
            if (swapped) {
                assert(vtop->r & VT_LVAL);
                gv(RC_FLOAT);
                vswap();
                fc = vtop->c.i;
            }

            if ((ft & VT_BTYPE) == VT_DOUBLE) {
                o(0xf2);
            } else {
                o(0xf3);
            }
            o(0x0f);
            opc = 0x58 + a;

            gen_modrm32(opc, vtop[-1].r, vtop->r, vtop->sym, fc);
            vtop--;
        }
    }
}

void gen_cvt_itof(int t)
{
    if ((t & VT_BTYPE) == VT_LDOUBLE) {
        save_reg(TREG_ST0);
        gv(RC_INT);
        if ((vtop->type.t & VT_BTYPE) == VT_LLONG) {

            o(0x50 + (vtop->r & VT_VALMASK));
            o(0x242cdf);
            o(0x08c48348);
        } else if ((vtop->type.t & (VT_BTYPE | VT_UNSIGNED)) == (VT_INT | VT_UNSIGNED)) {

            o(0x6a);
            g(0x00);
            o(0x50 + (vtop->r & VT_VALMASK));
            o(0x242cdf);
            o(0x10c48348);
        } else {

            o(0x50 + (vtop->r & VT_VALMASK));
            o(0x2404db);
            o(0x08c48348);
        }
        vtop->r = TREG_ST0;
    } else {
        int r = get_reg(RC_FLOAT);
        gv(RC_INT);
        o(0xf2 + ((t & VT_BTYPE) == VT_FLOAT ? 1 : 0));
        if ((vtop->type.t & (VT_BTYPE | VT_UNSIGNED)) == (VT_INT | VT_UNSIGNED) ||
            (vtop->type.t & VT_BTYPE) == VT_LLONG) {
            o(0x48);
        }
        o(0x2a0f);
        o(0xc0 + (vtop->r & VT_VALMASK) + REG_VALUE(r) * 8);
        vtop->r = r;
    }
}

void gen_cvt_ftof(int t)
{
    int ft, bt, tbt;

    ft = vtop->type.t;
    bt = ft & VT_BTYPE;
    tbt = t & VT_BTYPE;

    if (bt == VT_FLOAT) {
        gv(RC_FLOAT);
        if (tbt == VT_DOUBLE) {
            o(0x140f);
            o(0xc0 + REG_VALUE(vtop->r) * 9);
            o(0x5a0f);
            o(0xc0 + REG_VALUE(vtop->r) * 9);
        } else if (tbt == VT_LDOUBLE) {
            save_reg(RC_ST0);

            o(0x110ff3);
            o(0x44 + REG_VALUE(vtop->r) * 8);
            o(0xf024);
            o(0xf02444d9);
            vtop->r = TREG_ST0;
        }
    } else if (bt == VT_DOUBLE) {
        gv(RC_FLOAT);
        if (tbt == VT_FLOAT) {
            o(0x140f66);
            o(0xc0 + REG_VALUE(vtop->r) * 9);
            o(0x5a0f66);
            o(0xc0 + REG_VALUE(vtop->r) * 9);
        } else if (tbt == VT_LDOUBLE) {
            save_reg(RC_ST0);

            o(0x110ff2);
            o(0x44 + REG_VALUE(vtop->r) * 8);
            o(0xf024);
            o(0xf02444dd);
            vtop->r = TREG_ST0;
        }
    } else {
        int r;
        gv(RC_ST0);
        r = get_reg(RC_FLOAT);
        if (tbt == VT_DOUBLE) {
            o(0xf0245cdd);

            o(0x100ff2);
            o(0x44 + REG_VALUE(r) * 8);
            o(0xf024);
            vtop->r = r;
        } else if (tbt == VT_FLOAT) {
            o(0xf0245cd9);

            o(0x100ff3);
            o(0x44 + REG_VALUE(r) * 8);
            o(0xf024);
            vtop->r = r;
        }
    }
}

void gen_cvt_ftoi(int t)
{
    int ft, bt, size, r;
    ft = vtop->type.t;
    bt = ft & VT_BTYPE;
    if (bt == VT_LDOUBLE) {
        if (t != VT_INT) {
            vpush_helper_func(TOK___fixxfdi);
            vswap();
            gfunc_call(1);
            vpushi(0);
            vtop->r = REG_IRET;
            vtop->r2 = REG_IRE2;
            return;
        }
        gen_cvt_ftof(VT_DOUBLE);
        bt = VT_DOUBLE;
    }

    gv(RC_FLOAT);
    if (t != VT_INT)
        size = 8;
    else
        size = 4;

    r = get_reg(RC_INT);
    if (bt == VT_FLOAT) {
        o(0xf3);
    } else if (bt == VT_DOUBLE) {
        o(0xf2);
    } else {
        assert(0);
    }
    orex(size == 8, r, 0, 0x2c0f);
    o(0xc0 + REG_VALUE(vtop->r) + REG_VALUE(r) * 8);
    vtop->r = r;
}

ST_FUNC void gen_cvt_sxtw(void)
{
    int r = gv(RC_INT);

    o(0x6348);
    o(0xc0 + (REG_VALUE(r) << 3) + REG_VALUE(r));
}

ST_FUNC void gen_cvt_csti(int t)
{
    int r, sz, xl, ll;
    r = gv(RC_INT);
    sz = !(t & VT_UNSIGNED);
    xl = (t & VT_BTYPE) == VT_SHORT;
    ll = (vtop->type.t & VT_BTYPE) == VT_LLONG;
    orex(ll, r, 0, 0xc0b60f | (sz << 3 | xl) << 8 | (REG_VALUE(r) << 3 | REG_VALUE(r)) << 16);
}

ST_FUNC void gen_increment_tcov(SValue *sv)
{
    o(0x058348);
    greloca(cur_text_section, sv->sym, ind, R_X86_64_PC32, -5);
    gen_le32(0);
    o(1);
}

ST_FUNC void ggoto(void)
{
    gcall_or_jmp(1);
    vtop--;
}

ST_FUNC void gen_vla_sp_save(int addr)
{

    gen_modrm64(0x89, TREG_RSP, VT_LOCAL, NULL, addr);
}

ST_FUNC void gen_vla_sp_restore(int addr)
{
    gen_modrm64(0x8b, TREG_RSP, VT_LOCAL, NULL, addr);
}


ST_FUNC void gen_vla_alloc(CType *type, int align)
{
    int use_call = 0;

#if defined(CONFIG_TCC_BCHECK)
    use_call = tcc_state->do_bounds_check;
#endif
    if (use_call) {
        vpush_helper_func(TOK_alloca);
        vswap();
        gfunc_call(1);
    } else {
        int r;
        r = gv(RC_INT);

        o(0x2b48);
        o(0xe0 | REG_VALUE(r));

        o(0xf0e48348);
        vpop();
    }
}

ST_FUNC void gen_struct_copy(int size)
{
    int n = size / PTR_SIZE;
    gv2(RC_RDI, RC_RSI);
    if (n <= 4) {
        while (n)
            o(0xa548), --n;
    } else {
        vpushi(n);
        gv(RC_RCX);
        o(0xa548f3);
        vpop();
    }
    if (size & 0x04)
        o(0xa5);
    if (size & 0x02)
        o(0xa566);
    if (size & 0x01)
        o(0xa4);
    vpop();
    vpop();
}

#endif
