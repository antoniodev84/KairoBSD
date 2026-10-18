/*
 *  GAS like assembler for TCC
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
#ifdef CONFIG_TCC_ASM

static Section *last_text_section;
static int asmgoto_n;

static int tcc_assemble_internal(TCCState *s1, int do_preprocess, int global);
static Sym *asm_new_label(TCCState *s1, int label, int is_local);
static Sym *asm_new_label1(TCCState *s1, int label, int is_local, int sh_num, int value);

#if PTR_SIZE == 8

ST_FUNC void gen_addr64(int r, Sym *sym, int64_t c)
{
    if (r & VT_SYM)
        greloca(cur_text_section, sym, ind, R_DATA_PTR, c), c = 0;
    gen_le32(c);
    gen_le32(c >> 32);
}

ST_FUNC void gen_expr64(ExprValue *pe)
{
    gen_addr64(pe->sym ? VT_SYM : 0, pe->sym, pe->v);
}
#endif

static int asm_get_prefix_name(TCCState *s1, const char *prefix, unsigned int n)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%u", prefix, n);
    return tok_alloc_const(buf);
}

ST_FUNC int asm_get_local_label_name(TCCState *s1, unsigned int n)
{
    return asm_get_prefix_name(s1, "L..", n);
}

static int asm2cname(int v, int *addeddot)
{
    const char *name;
    *addeddot = 0;
    if (!tcc_state->leading_underscore)
        return v;
    name = get_tok_str(v, NULL);
    if (!name)
        return v;
    if (name[0] == '_') {
        v = tok_alloc_const(name + 1);
    } else if (!strchr(name, '.')) {
        char newname[256];
        snprintf(newname, sizeof newname, ".%s", name);
        v = tok_alloc_const(newname);
        *addeddot = 1;
    }
    return v;
}

static Sym *asm_label_find(int v)
{
    Sym *sym;
    int addeddot;
    v = asm2cname(v, &addeddot);
    sym = sym_find(v);
    while (sym && sym->sym_scope && !(sym->type.t & VT_STATIC))
        sym = sym->prev_tok;
    return sym;
}

static Sym *asm_label_push(int v)
{
    int addeddot, v2 = asm2cname(v, &addeddot);

    Sym *sym = global_identifier_push(v2, VT_ASM | VT_EXTERN | VT_STATIC, 0);
    if (addeddot)
        sym->asm_label = v;
    return sym;
}

ST_FUNC Sym *get_asm_sym(int name, Sym *csym)
{
    Sym *sym = asm_label_find(name);
    if (!sym) {
        sym = asm_label_push(name);
        if (csym)
            sym->c = csym->c;
    }
    return sym;
}

static Sym *asm_section_sym(TCCState *s1, Section *sec)
{
    char buf[100];
    int label;
    Sym *sym;
    snprintf(buf, sizeof buf, "L.%s", sec->name);
    label = tok_alloc_const(buf);
    sym = asm_label_find(label);
    return sym ? sym : asm_new_label1(s1, label, 1, sec->sh_num, 0);
}

static void asm_expr_unary(TCCState *s1, ExprValue *pe)
{
    Sym *sym;
    int op, label;
    uint64_t n;
    const char *p;

    switch (tok) {
        case TOK_PPNUM:
            p = tokc.str.data;
            n = strtoull(p, (char **)&p, 0);
            if (*p == 'b' || *p == 'f') {

                label = asm_get_local_label_name(s1, n);
                sym = asm_label_find(label);
                if (*p == 'b') {

                    if (sym && (!sym->c || elfsym(sym)->st_shndx == SHN_UNDEF))
                        sym = sym->prev_tok;
                    if (!sym)
                        tcc_error("local label '%d' not found backward", (int)n);
                } else {

                    if (!sym || (sym->c && elfsym(sym)->st_shndx != SHN_UNDEF)) {

                        sym = asm_label_push(label);
                    }
                }
                pe->v = 0;
                pe->sym = sym;
                pe->pcrel = 0;
            } else if (*p == '\0') {
                pe->v = n;
                pe->sym = NULL;
                pe->pcrel = 0;
            } else {
                tcc_error("invalid number syntax");
            }
            next();
            break;
        case '+':
            next();
            asm_expr_unary(s1, pe);
            break;
        case '-':
        case '~':
            op = tok;
            next();
            asm_expr_unary(s1, pe);
            if (pe->sym)
                tcc_error("invalid operation with label");
            if (op == '-')
                pe->v = -pe->v;
            else
                pe->v = ~pe->v;
            break;
        case TOK_CCHAR:
        case TOK_LCHAR:
            pe->v = tokc.i;
            pe->sym = NULL;
            pe->pcrel = 0;
            next();
            break;
        case '(':
            next();
            asm_expr(s1, pe);
            skip(')');
            break;
        case '.':
            pe->v = ind;
            pe->sym = asm_section_sym(s1, cur_text_section);
            pe->pcrel = 0;
            next();
            break;
        default:
            if (tok >= TOK_IDENT) {
                ElfSym *esym;

                sym = get_asm_sym(tok, NULL);
                esym = elfsym(sym);
                if (esym && esym->st_shndx == SHN_ABS) {

                    pe->v = esym->st_value;
                    pe->sym = NULL;
                    pe->pcrel = 0;
                } else {
                    pe->v = 0;
                    pe->sym = sym;
                    pe->pcrel = 0;
                }
                next();
            } else {
                tcc_error("bad expression syntax [%s]", get_tok_str(tok, &tokc));
            }
            break;
    }
}

static void asm_expr_prod(TCCState *s1, ExprValue *pe)
{
    int op;
    ExprValue e2;

    asm_expr_unary(s1, pe);
    for (;;) {
        op = tok;
        if (op != '*' && op != '/' && op != '%' && op != TOK_SHL && op != TOK_SAR)
            break;
        next();
        asm_expr_unary(s1, &e2);
        if (pe->sym || e2.sym)
            tcc_error("invalid operation with label");
        switch (op) {
            case '*':
                pe->v *= e2.v;
                break;
            case '/':
                if (e2.v == 0) {
                div_error:
                    tcc_error("division by zero");
                }
                pe->v /= e2.v;
                break;
            case '%':
                if (e2.v == 0)
                    goto div_error;
                pe->v %= e2.v;
                break;
            case TOK_SHL:
                pe->v <<= e2.v;
                break;
            default:
            case TOK_SAR:
                pe->v >>= e2.v;
                break;
        }
    }
}

static void asm_expr_logic(TCCState *s1, ExprValue *pe)
{
    int op;
    ExprValue e2;

    asm_expr_prod(s1, pe);
    for (;;) {
        op = tok;
        if (op != '&' && op != '|' && op != '^')
            break;
        next();
        asm_expr_prod(s1, &e2);
        if (pe->sym || e2.sym)
            tcc_error("invalid operation with label");
        switch (op) {
            case '&':
                pe->v &= e2.v;
                break;
            case '|':
                pe->v |= e2.v;
                break;
            default:
            case '^':
                pe->v ^= e2.v;
                break;
        }
    }
}

static inline void asm_expr_sum(TCCState *s1, ExprValue *pe)
{
    int op;
    ExprValue e2;

    asm_expr_logic(s1, pe);
    for (;;) {
        op = tok;
        if (op != '+' && op != '-')
            break;
        next();
        asm_expr_logic(s1, &e2);
        if (op == '+') {
            if (pe->sym != NULL && e2.sym != NULL)
                goto cannot_relocate;
            pe->v += e2.v;
            if (pe->sym == NULL && e2.sym != NULL)
                pe->sym = e2.sym;
        } else {
            pe->v -= e2.v;

            if (!e2.sym) {

            } else if (pe->sym == e2.sym) {

                pe->sym = NULL;
            } else {
                ElfSym *esym1, *esym2;
                esym1 = elfsym(pe->sym);
                esym2 = elfsym(e2.sym);
                if (!esym2)
                    goto cannot_relocate;
                if (esym1 && esym1->st_shndx == esym2->st_shndx && esym1->st_shndx != SHN_UNDEF) {

                    pe->v += (int)(esym1->st_value - esym2->st_value);
                    pe->sym = NULL;
                } else if (esym2->st_shndx == cur_text_section->sh_num) {

                    pe->v += (int)(0 - esym2->st_value);
                    pe->pcrel = 1;
                    e2.sym = NULL;
                } else {
                cannot_relocate:
                    tcc_error("invalid operation with label");
                }
            }
        }
    }
}

static inline void asm_expr_cmp(TCCState *s1, ExprValue *pe)
{
    int op;
    ExprValue e2;

    asm_expr_sum(s1, pe);
    for (;;) {
        op = tok;
        if (op != TOK_EQ && op != TOK_NE && (op > TOK_GT || op < TOK_ULE))
            break;
        next();
        asm_expr_sum(s1, &e2);
        if (pe->sym || e2.sym)
            tcc_error("invalid operation with label");
        switch (op) {
            case TOK_EQ:
                pe->v = pe->v == e2.v;
                break;
            case TOK_NE:
                pe->v = pe->v != e2.v;
                break;
            case TOK_LT:
                pe->v = (int64_t)pe->v < (int64_t)e2.v;
                break;
            case TOK_GE:
                pe->v = (int64_t)pe->v >= (int64_t)e2.v;
                break;
            case TOK_LE:
                pe->v = (int64_t)pe->v <= (int64_t)e2.v;
                break;
            case TOK_GT:
                pe->v = (int64_t)pe->v > (int64_t)e2.v;
                break;
            default:
                break;
        }

        pe->v = -(int64_t)pe->v;
    }
}

ST_FUNC void asm_expr(TCCState *s1, ExprValue *pe)
{
    asm_expr_cmp(s1, pe);
}

ST_FUNC int asm_int_expr(TCCState *s1)
{
    ExprValue e;
    asm_expr(s1, &e);
    if (e.sym)
        expect("constant");
    if ((int)e.v != e.v)
        tcc_error("integer out of range %lld", (long long)e.v);
    return e.v;
}

static Sym *asm_new_label1(TCCState *s1, int label, int is_local, int sh_num, int value)
{
    Sym *sym;
    ElfSym *esym;

    sym = asm_label_find(label);
    if (sym) {
        esym = elfsym(sym);

        if (esym && esym->st_shndx != SHN_UNDEF) {

            if (IS_ASM_SYM(sym) && (is_local == 1 || (sym->type.t & VT_EXTERN)))
                goto new_label;
            if (!(sym->type.t & VT_EXTERN))
                tcc_error("assembler label '%s' already defined", get_tok_str(label, NULL));
        }
    } else {
    new_label:
        sym = asm_label_push(label);
    }
    if (!sym->c)
        put_extern_sym2(sym, SHN_UNDEF, 0, 0, 1);
    esym = elfsym(sym);
    esym->st_shndx = sh_num;
    esym->st_value = value;
    if (is_local != 2)
        sym->type.t &= ~VT_EXTERN;
    return sym;
}

static Sym *asm_new_label(TCCState *s1, int label, int is_local)
{
    return asm_new_label1(s1, label, is_local, cur_text_section->sh_num, ind);
}

static Sym *set_symbol(TCCState *s1, int label)
{
    long n;
    ExprValue e;
    Sym *sym;
    ElfSym *esym;
    next();
    asm_expr(s1, &e);
    n = e.v;
    esym = elfsym(e.sym);
    if (esym)
        n += esym->st_value;
    sym = asm_new_label1(s1, label, 2, esym ? esym->st_shndx : SHN_ABS, n);
    elfsym(sym)->st_other |= ST_ASM_SET;
    return sym;
}

static void use_section1(TCCState *s1, Section *sec)
{
    cur_text_section->data_offset = ind;
    cur_text_section = sec;
    ind = cur_text_section->data_offset;
}

static void use_section(TCCState *s1, const char *name)
{
    Section *sec;
    sec = find_section(s1, name);
    use_section1(s1, sec);
}

static void push_section(TCCState *s1, const char *name)
{
    Section *sec = find_section(s1, name);
    sec->prev = cur_text_section;
    use_section1(s1, sec);
}

static void pop_section(TCCState *s1)
{
    Section *prev = cur_text_section->prev;
    if (!prev)
        tcc_error(".popsection without .pushsection");
    cur_text_section->prev = NULL;
    use_section1(s1, prev);
}

static void asm_parse_directive(TCCState *s1, int global)
{
    int n, offset, v, size, tok1, c;
    Section *sec;
    uint8_t *ptr;

    sec = cur_text_section;
    switch (tok) {
        case TOK_ASMDIR_align:
        case TOK_ASMDIR_balign:
        case TOK_ASMDIR_p2align:
        case TOK_ASMDIR_skip:
        case TOK_ASMDIR_space:
            tok1 = tok;
            next();
            n = asm_int_expr(s1);
            if (tok1 == TOK_ASMDIR_p2align) {
                if (n < 0 || n > 30)
                    tcc_error("invalid p2align, must be between 0 and 30");
                n = 1 << n;
                tok1 = TOK_ASMDIR_align;
            }
            if (tok1 == TOK_ASMDIR_align || tok1 == TOK_ASMDIR_balign) {
                if (n <= 0 || (n & (n - 1)) != 0)
                    tcc_error("alignment must be a positive power of two");
                offset = (ind + n - 1) & -n;
                size = offset - ind;

                if (sec->sh_addralign < n)
                    sec->sh_addralign = n;
                c = sec->sh_flags & SHF_EXECINSTR;
            } else {
                if (n < 0)
                    n = 0;
                size = n, c = 0;
            }
            v = 0;
            if (tok == ',') {
                next();
                v = asm_int_expr(s1), c = 0;
            }
        zero_pad:
            if ((uint64_t)ind + size >= 1 << 30)
                tcc_error("too much data");
            if (sec->sh_type != SHT_NOBITS) {
                if (c) {
                    gen_fill_nops(size);
                    break;
                }
                sec->data_offset = ind;
                ptr = section_ptr_add(sec, size);
                memset(ptr, v, size);
            }
            ind += size;
            break;
        case TOK_ASMDIR_quad:
#if PTR_SIZE == 8
            size = 8;
            goto asm_data;
#else
            next();
            for (;;) {
                uint64_t vl;
                const char *p;

                p = tokc.str.data;
                if (tok != TOK_PPNUM) {
                error_constant:
                    tcc_error("64 bit constant");
                }
                vl = strtoll(p, (char **)&p, 0);
                if (*p != '\0')
                    goto error_constant;
                next();
                if (sec->sh_type != SHT_NOBITS) {

                    gen_le32(vl);
                    gen_le32(vl >> 32);
                } else {
                    ind += 8;
                }
                if (tok != ',')
                    break;
                next();
            }
            break;
#endif
        case TOK_ASMDIR_byte:
            size = 1;
            goto asm_data;
        case TOK_ASMDIR_word:
        case TOK_ASMDIR_short:
            size = 2;
            goto asm_data;
        case TOK_ASMDIR_long:
        case TOK_ASMDIR_int:
            size = 4;
        asm_data:
            next();
            for (;;) {
                ExprValue e;
                asm_expr(s1, &e);
                if (sec->sh_type != SHT_NOBITS) {
                    if (size == 4) {
                        gen_expr32(&e);
#if PTR_SIZE == 8
                    } else if (size == 8) {
                        gen_expr64(&e);
#endif
                    } else {
                        if (e.sym)
                            expect("constant");
                        if (size == 1)
                            g(e.v);
                        else
                            gen_le16(e.v);
                    }
                } else {
                    ind += size;
                }
                if (tok != ',')
                    break;
                next();
            }
            break;
        case TOK_ASMDIR_fill: {
            int repeat, size, val, i, j;
            uint8_t repeat_buf[8];
            next();
            repeat = asm_int_expr(s1);
            if (repeat < 0) {
                tcc_error("repeat < 0; .fill ignored");
                break;
            }
            size = 1;
            val = 0;
            if (tok == ',') {
                next();
                size = asm_int_expr(s1);
                if (size < 0) {
                    tcc_error("size < 0; .fill ignored");
                    break;
                }
                if (size > 8)
                    size = 8;
                if (tok == ',') {
                    next();
                    val = asm_int_expr(s1);
                }
            }

            repeat_buf[0] = val;
            repeat_buf[1] = val >> 8;
            repeat_buf[2] = val >> 16;
            repeat_buf[3] = val >> 24;
            repeat_buf[4] = 0;
            repeat_buf[5] = 0;
            repeat_buf[6] = 0;
            repeat_buf[7] = 0;
            for (i = 0; i < repeat; i++) {
                for (j = 0; j < size; j++) {
                    g(repeat_buf[j]);
                }
            }
        } break;
        case TOK_ASMDIR_rept: {
            int repeat;
            TokenString *init_str;
            next();
            repeat = asm_int_expr(s1);
            init_str = tok_str_alloc();
            while (next(), tok != TOK_ASMDIR_endr) {
                if (tok == CH_EOF)
                    tcc_error("we at end of file, .endr not found");
                tok_str_add_tok(init_str);
            }
            tok_str_add(init_str, TOK_EOF);
            begin_macro(init_str, 1);
            while (repeat-- > 0) {
                tcc_assemble_internal(s1, (parse_flags & PARSE_FLAG_PREPROCESS), global);
                macro_ptr = init_str->str;
            }
            end_macro();
            next();
            break;
        }
        case TOK_ASMDIR_org: {
            ExprValue e;
            ElfSym *esym;
            next();
            asm_expr(s1, &e);
            n = e.v;
            esym = elfsym(e.sym);
            if (esym) {
                if (esym->st_shndx != cur_text_section->sh_num)
                    expect("constant or same-section symbol");
                n += esym->st_value;
            }
            if (n < ind)
                tcc_error("attempt to .org backwards");
            v = c = 0;
            size = n - ind;
            goto zero_pad;
        } break;
        case TOK_ASMDIR_set:
            next();
            tok1 = tok;
            next();

            if (tok == ',')
                set_symbol(s1, tok1);
            break;
        case TOK_ASMDIR_comm: {
            next();
            if (tok < TOK_IDENT)
                expect("identifier");
            Sym *sym = get_asm_sym(tok, NULL);
            next();
            skip(',');
            long size = asm_int_expr(s1), align = 1;
            if (tok == ',') {
                next();
                align = asm_int_expr(s1);
            }
            if (size < 0 || align <= 0 || (align & (align - 1)))
                tcc_error("invalid common symbol size or alignment");
            ElfSym *existing = elfsym(sym);
            if (existing && existing->st_shndx != SHN_UNDEF && existing->st_shndx != SHN_COMMON)
                tcc_error("common symbol already defined");
            if (existing && existing->st_shndx == SHN_COMMON) {
                if (existing->st_size > (unsigned long)size)
                    size = existing->st_size;
                if (existing->st_value > (unsigned long)align)
                    align = existing->st_value;
            }
            put_extern_sym2(sym, SHN_COMMON, align, size, 1);
        } break;
        case TOK_ASMDIR_local:
        case TOK_ASMDIR_globl:
        case TOK_ASMDIR_global:
        case TOK_ASMDIR_weak:
        case TOK_ASMDIR_hidden:
            tok1 = tok;
            do {
                Sym *sym;
                next();
                if (tok < TOK_IDENT)
                    expect("identifier");
                sym = get_asm_sym(tok, NULL);
                if (tok1 == TOK_ASMDIR_local)
                    sym->type.t |= VT_STATIC;
                else if (tok1 != TOK_ASMDIR_hidden)
                    sym->type.t &= ~VT_STATIC;
                if (tok1 == TOK_ASMDIR_weak)
                    sym->a.weak = 1;
                else if (tok1 == TOK_ASMDIR_hidden)
                    sym->a.visibility = STV_HIDDEN;
                update_storage(sym);
                next();
            } while (tok == ',');
            break;
        case TOK_ASMDIR_string:
        case TOK_ASMDIR_ascii:
        case TOK_ASMDIR_asciz: {
            const char *p;
            int i, size, t;

            t = tok;
            next();
            for (;;) {
                if (tok != TOK_STR)
                    expect("string constant");
                p = tokc.str.data;
                size = tokc.str.size;
                if (t == TOK_ASMDIR_ascii && size > 0)
                    size--;
                for (i = 0; i < size; i++)
                    g(p[i]);
                next();
                if (tok == ',') {
                    next();
                } else if (tok != TOK_STR) {
                    break;
                }
            }
        } break;
        case TOK_ASMDIR_text:
        case TOK_ASMDIR_data:
        case TOK_ASMDIR_bss: {
            char sname[64];
            tok1 = tok;
            n = 0;
            next();
            if (tok != ';' && tok != TOK_LINEFEED) {
                n = asm_int_expr(s1);
                next();
            }
            if (n)
                sprintf(sname, "%s%d", get_tok_str(tok1, NULL), n);
            else
                sprintf(sname, "%s", get_tok_str(tok1, NULL));
            use_section(s1, sname);
        } break;
        case TOK_ASMDIR_file: {
            const char *p;
            parse_flags &= ~PARSE_FLAG_TOK_STR;
            next();
            if (tok == TOK_PPNUM)
                next();
            if (tok == TOK_PPSTR && tokc.str.data[0] == '"') {
                tokc.str.data[tokc.str.size - 2] = 0;
                p = tokc.str.data + 1;
            } else if (tok >= TOK_IDENT) {
                p = get_tok_str(tok, &tokc);
            } else {
                skip_to_eol(0);
                break;
            }
            tccpp_putfile(p);
            next();
        } break;
        case TOK_ASMDIR_ident: {
            char ident[256];

            ident[0] = '\0';
            next();
            if (tok == TOK_STR)
                pstrcat(ident, sizeof(ident), tokc.str.data);
            else
                pstrcat(ident, sizeof(ident), get_tok_str(tok, &tokc));
            tcc_warning_c(warn_unsupported)("ignoring .ident %s", ident);
            next();
        } break;
        case TOK_ASMDIR_size: {
            Sym *sym;
            ElfSym *esym;

            next();
            if (tok < TOK_IDENT)
                expect("identifier");
            sym = asm_label_find(tok);
            if (!sym)
                tcc_error("label not found: %s", get_tok_str(tok, NULL));

            tcc_warning_c(warn_unsupported)("ignoring .size %s,*", get_tok_str(tok, NULL));
            next();
            skip(',');
            n = asm_int_expr(s1);
            esym = elfsym(sym);
            if (esym) {
                esym->st_size = n;
            }
        } break;
        case TOK_ASMDIR_type: {
            Sym *sym;
            const char *newtype;
            int st_type;

            next();
            if (tok < TOK_IDENT)
                expect("identifier");
            sym = get_asm_sym(tok, NULL);
            next();
            skip(',');
            if (tok == TOK_STR) {
                newtype = tokc.str.data;
            } else {
                if (tok == '@' || tok == '%')
                    next();
                newtype = get_tok_str(tok, NULL);
            }

            if (!strcmp(newtype, "function") || !strcmp(newtype, "STT_FUNC")) {
                if (IS_ASM_SYM(sym))
                    sym->type.t |= VT_ASM_FUNC;
                st_type = STT_FUNC;
            set_st_type:
                if (sym->c) {
                    ElfSym *esym = elfsym(sym);
                    esym->st_info = ELFW(ST_INFO)(ELFW(ST_BIND)(esym->st_info), st_type);
                }
            } else if (!strcmp(newtype, "object") || !strcmp(newtype, "STT_OBJECT")) {
                st_type = STT_OBJECT;
                goto set_st_type;
            } else
                tcc_warning_c(warn_unsupported)("change type of '%s' from 0x%x to '%s' ignored",
                                                get_tok_str(sym->v, NULL), sym->type.t, newtype);

            next();
        } break;
        case TOK_ASMDIR_pushsection:
        case TOK_ASMDIR_section: {
            char sname[256];
            int old_nb_section = s1->nb_sections;
            int flags = SHF_ALLOC;

            tok1 = tok;

            next();
            sname[0] = '\0';
            while (tok != ';' && tok != TOK_LINEFEED && tok != ',') {
                if (tok == TOK_STR)
                    pstrcat(sname, sizeof(sname), tokc.str.data);
                else
                    pstrcat(sname, sizeof(sname), get_tok_str(tok, NULL));
                next();
            }
            if (tok == ',') {
                const char *p;

                next();
                if (tok != TOK_STR)
                    expect("string constant");
                for (p = tokc.str.data; *p; ++p) {
                    if (*p == 'w')
                        flags |= SHF_WRITE;
                    if (*p == 'x')
                        flags |= SHF_EXECINSTR;
                }
                next();
                if (tok == ',') {
                    next();
                    if (tok == '@' || tok == '%')
                        next();
                    next();
                }
            }
            last_text_section = cur_text_section;
            if (tok1 == TOK_ASMDIR_section)
                use_section(s1, sname);
            else
                push_section(s1, sname);

            if (old_nb_section != s1->nb_sections) {
                cur_text_section->sh_addralign = 1;

                if (!strcmp(sname, ".init") || !strcmp(sname, ".fini"))
                    flags |= SHF_EXECINSTR;
                cur_text_section->sh_flags = flags;
            }
        } break;
        case TOK_ASMDIR_previous: {
            Section *sec;
            next();
            if (!last_text_section)
                tcc_error("no previous section referenced");
            sec = cur_text_section;
            use_section1(s1, last_text_section);
            last_text_section = sec;
        } break;
        case TOK_ASMDIR_popsection:
            next();
            pop_section(s1);
            break;
#if PTR_SIZE == 8

        case TOK_ASMDIR_code64:
            next();
            break;
#endif

        case TOK_ASMDIR_symver:
            next();
            next();
            skip(',');
            next();
            skip('@');
            next();
            break;
        case TOK_ASMDIR_reloc: {
            ExprValue e;
            const char *reloc_name;
            int reloc_type = -1;

            next();
            asm_expr(s1, &e);
            skip(',');
            reloc_name = get_tok_str(tok, NULL);
            if (reloc_type < 0)
                tcc_error("unimp: reloc '%s' unknown", reloc_name);
            next();
            skip(',');
            greloca(cur_text_section, get_asm_sym(tok, NULL), e.v, reloc_type, 0);
            next();
        } break;
        default:
            tcc_error("unknown assembler directive '.%s'", get_tok_str(tok, NULL));
            break;
    }
}

static int tcc_assemble_internal(TCCState *s1, int do_preprocess, int global)
{
    int opcode;
    int saved_parse_flags = parse_flags;

    parse_flags = PARSE_FLAG_ASM_FILE | PARSE_FLAG_TOK_STR;
    if (do_preprocess)
        parse_flags |= PARSE_FLAG_PREPROCESS;
    for (;;) {
        next();
        if (tok == TOK_EOF)
            break;
        tcc_debug_line(s1);
        parse_flags |= PARSE_FLAG_LINEFEED;
    redo:
        if (tok == '#') {

            while (tok != TOK_LINEFEED)
                next();
        } else
            if (tok >= TOK_ASMDIR_FIRST && tok <= TOK_ASMDIR_LAST) {
            asm_parse_directive(s1, global);
        } else if (tok == TOK_PPNUM) {
            const char *p;
            int n;
            p = tokc.str.data;
            n = strtoul(p, (char **)&p, 10);
            if (*p != '\0')
                expect("':'");

            asm_new_label(s1, asm_get_local_label_name(s1, n), 1);
            next();
            skip(':');
            goto redo;
        } else if (tok >= TOK_IDENT) {

            opcode = tok;
            next();
            if (tok == ':') {

                asm_new_label(s1, opcode, 0);
                next();
                goto redo;
            } else if (tok == '=') {
                set_symbol(s1, opcode);
                goto redo;
            } else {
                asm_opcode(s1, opcode);
            }
        }

        if (tok != ';' && tok != TOK_LINEFEED)
            expect("end of line");
        parse_flags &= ~PARSE_FLAG_LINEFEED;
    }

    parse_flags = saved_parse_flags;
    return 0;
}

ST_FUNC int tcc_assemble(TCCState *s1, int do_preprocess)
{
    int ret;
    tcc_debug_start(s1);

    cur_text_section = text_section;
    ind = cur_text_section->data_offset;
    nocode_wanted = 0;
    ret = tcc_assemble_internal(s1, do_preprocess, 1);
    cur_text_section->data_offset = ind;
    tcc_debug_end(s1);
    return ret;
}

static void tcc_assemble_inline(TCCState *s1, const char *str, int len, int global)
{
    const int *saved_macro_ptr = macro_ptr;
    int dotid = set_idnum('.', IS_ID);

    tcc_open_bf(s1, ":asm:", len);
    memcpy(file->buffer, str, len);
    macro_ptr = NULL;
    tcc_assemble_internal(s1, 0, global);
    tcc_close();

    set_idnum('.', dotid);
    macro_ptr = saved_macro_ptr;
}

ST_FUNC int find_constraint(ASMOperand *operands, int nb_operands, const char *name,
                            const char **pp)
{
    int index;
    TokenSym *ts;
    const char *p;

    if (isnum(*name)) {
        index = 0;
        while (isnum(*name)) {
            index = (index * 10) + (*name) - '0';
            name++;
        }
        if ((unsigned)index >= nb_operands)
            index = -1;
    } else if (*name == '[') {
        name++;
        p = strchr(name, ']');
        if (p) {
            ts = tok_alloc(name, p - name);
            for (index = 0; index < nb_operands; index++) {
                if (operands[index].id == ts->tok)
                    goto found;
            }
            index = -1;
        found:
            name = p + 1;
        } else {
            index = -1;
        }
    } else {
        index = -1;
    }
    if (pp)
        *pp = name;
    return index;
}

static void subst_asm_operands(ASMOperand *operands, int nb_operands, CString *out_str,
                               const char *str)
{
    int c, index, modifier;
    ASMOperand *op;
    SValue sv;

    for (;;) {
        c = *str++;
        if (c == '%') {
            if (*str == '%') {
                str++;
                goto add_char;
            }
            modifier = 0;
            if (*str == 'c' || *str == 'n' || *str == 'b' || *str == 'w' || *str == 'h' ||
                *str == 'k' || *str == 'q' || *str == 'l' ||

                *str == 'P')
                modifier = *str++;
            index = find_constraint(operands, nb_operands, str, &str);
            if (index < 0)
            error:
                tcc_error("invalid operand reference after %%");
            op = &operands[index];
            if (modifier == 'l') {
                cstr_cat(out_str, get_tok_str(op->is_label, NULL), -1);
            } else {
                if (op->vt == NULL)
                    goto error;
                sv = *op->vt;
                if (op->reg >= 0) {
                    sv.r = op->reg;
                    if (op->is_memory)
                        sv.r |= VT_LVAL;
                }
                subst_asm_operand(out_str, &sv, modifier);
            }
        } else {
        add_char:
            cstr_ccat(out_str, c);
            if (c == '\0')
                break;
        }
    }
}

static void parse_asm_operands(ASMOperand *operands, int *nb_operands_ptr, int is_output)
{
    ASMOperand *op;
    int nb_operands;
    char *astr;

    if (tok != ':') {
        nb_operands = *nb_operands_ptr;
        for (;;) {
            if (nb_operands >= MAX_ASM_OPERANDS)
                tcc_error("too many asm operands");
            op = &operands[nb_operands++];
            op->id = 0;
            if (tok == '[') {
                next();
                if (tok < TOK_IDENT)
                    expect("identifier");
                op->id = tok;
                next();
                skip(']');
            }
            astr = parse_mult_str("string constant")->data;
            pstrcpy(op->constraint, sizeof op->constraint, astr);
            skip('(');
            gexpr();
            if (is_output) {
                if (!(vtop->type.t & VT_ARRAY))
                    test_lvalue();
            } else {

                if ((vtop->r & VT_LVAL) &&
                    ((vtop->r & VT_VALMASK) == VT_LLOCAL || (vtop->r & VT_VALMASK) < VT_CONST) &&
                    !strchr(op->constraint, 'm')
                ) {
                    gv(RC_INT);
                }
            }
            op->vt = vtop;
            skip(')');
            if (tok == ',') {
                next();
            } else {
                break;
            }
        }
        *nb_operands_ptr = nb_operands;
    }
}

ST_FUNC void asm_instr(void)
{
    CString astr, *astr1;

    ASMOperand operands[MAX_ASM_OPERANDS];
    int nb_outputs, nb_operands, i, must_subst, out_reg, nb_labels;
    uint8_t clobber_regs[NB_ASM_REGS];
    Section *sec;

    while (tok == TOK_VOLATILE1 || tok == TOK_VOLATILE2 || tok == TOK_VOLATILE3 ||
           tok == TOK_GOTO) {
        next();
    }

    astr1 = parse_asm_str();
    cstr_new_s(&astr);
    cstr_cat(&astr, astr1->data, astr1->size);

    nb_operands = 0;
    nb_outputs = 0;
    nb_labels = 0;
    must_subst = 0;
    memset(clobber_regs, 0, sizeof(clobber_regs));
    if (tok == ':') {
        next();
        must_subst = 1;

        parse_asm_operands(operands, &nb_operands, 1);
        nb_outputs = nb_operands;
        if (tok == ':') {
            next();
            if (tok != ')') {

                parse_asm_operands(operands, &nb_operands, 0);
                if (tok == ':') {

                    next();
                    for (;;) {
                        if (tok == ':')
                            break;
                        if (tok != TOK_STR)
                            expect("string constant");
                        asm_clobber(clobber_regs, tokc.str.data);
                        next();
                        if (tok == ',') {
                            next();
                        } else {
                            break;
                        }
                    }
                }
                if (tok == ':') {

                    next();
                    for (;;) {
                        Sym *csym;
                        int asmname;
                        if (nb_operands + nb_labels >= MAX_ASM_OPERANDS)
                            tcc_error("too many asm operands");
                        if (tok < TOK_UIDENT)
                            expect("label identifier");
                        memset(operands + nb_operands + nb_labels, 0, sizeof(operands[0]));
                        operands[nb_operands + nb_labels++].id = tok;

                        csym = label_find(tok);
                        if (!csym) {
                            csym = label_push(&global_label_stack, tok, LABEL_FORWARD);
                        } else {
                            if (csym->r == LABEL_DECLARED)
                                csym->r = LABEL_FORWARD;
                        }
                        next();
                        asmname = asm_get_prefix_name(tcc_state, "LG.", ++asmgoto_n);
                        if (!csym->c)
                            put_extern_sym2(csym, SHN_UNDEF, 0, 0, 1);
                        get_asm_sym(asmname, csym);
                        operands[nb_operands + nb_labels - 1].is_label = asmname;

                        if (tok != ',')
                            break;
                        next();
                    }
                }
            }
        }
    }
    skip(')');

    if (tok != ';')
        expect("';'");

    save_regs(0);

    asm_compute_constraints(operands, nb_operands, nb_outputs, clobber_regs, &out_reg);

#ifdef ASM_DEBUG
    printf("asm: \"%s\"\n", (char *)astr.data);
#endif
    if (must_subst) {
        cstr_reset(astr1);
        cstr_cat(astr1, astr.data, astr.size);
        cstr_reset(&astr);
        subst_asm_operands(operands, nb_operands + nb_labels, &astr, astr1->data);
    }

#ifdef ASM_DEBUG
    printf("subst_asm: \"%s\"\n", (char *)astr.data);
#endif

    asm_gen_code(operands, nb_operands, nb_outputs, 0, clobber_regs, out_reg);

    sec = cur_text_section;

    tcc_assemble_inline(tcc_state, astr.data, astr.size - 1, 0);
    cstr_free_s(&astr);
    if (sec != cur_text_section) {
        tcc_warning("inline asm tries to change current section");
        use_section1(tcc_state, sec);
    }

    next();

    asm_gen_code(operands, nb_operands, nb_outputs, 1, clobber_regs, out_reg);

    for (i = 0; i < nb_operands; i++) {
        vpop();
    }
}

ST_FUNC void asm_global_instr(void)
{
    CString *astr;
    int saved_nocode_wanted = nocode_wanted;

    nocode_wanted = 0;
    next();
    astr = parse_asm_str();
    skip(')');

    if (tok != ';')
        expect("';'");

#ifdef ASM_DEBUG
    printf("asm_global: \"%s\"\n", (char *)astr->data);
#endif
    cur_text_section = text_section;
    ind = cur_text_section->data_offset;

    tcc_assemble_inline(tcc_state, astr->data, astr->size - 1, 1);

    cur_text_section->data_offset = ind;

    next();

    nocode_wanted = saved_nocode_wanted;
}

#else
ST_FUNC int tcc_assemble(TCCState *s1, int do_preprocess)
{
    tcc_error("asm not supported");
}

ST_FUNC void asm_instr(void)
{
    tcc_error("inline asm() not supported");
}

ST_FUNC void asm_global_instr(void)
{
    tcc_error("inline asm() not supported");
}
#endif
