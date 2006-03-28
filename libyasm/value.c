/*
 * Value handling
 *
 *  Copyright (C) 2006  Peter Johnson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND OTHER CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR OTHER CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#define YASM_LIB_INTERNAL
#include "util.h"
/*@unused@*/ RCSID("$Id$");

#include "coretype.h"
#include "bitvect.h"

#include "errwarn.h"
#include "intnum.h"
#include "floatnum.h"
#include "expr.h"
#include "value.h"
#include "symrec.h"

#include "bytecode.h"
#include "section.h"

#include "arch.h"

#include "expr-int.h"
#include "bc-int.h"

static int
value_finalize_scan(yasm_value *value, yasm_expr *e, int ssym_not_ok)
{
    int i;
    /*@dependent@*/ yasm_section *sect;
    /*@dependent@*/ /*@null@*/ yasm_bytecode *precbc;

    unsigned long shamt;    /* for SHR */

    /* Yes, this has a maximum upper bound on 32 terms, based on an
     * "insane number of terms" (and ease of implementation) WAG.
     * The right way to do this would be a stack-based alloca, but that's
     * not ISO C.  We really don't want to malloc here as this function is
     * hit a lot!
     *
     * This is a bitmask to keep things small, as this is a recursive
     * routine and we don't want to eat up stack space.
     */
    unsigned long used;	    /* for ADD */

    /* Thanks to this running after a simplify, we don't need to iterate
     * down through IDENTs or handle SUB.
     *
     * We scan for a single symrec, gathering info along the way.  After
     * we've found the symrec, we keep scanning but error if we find
     * another one.  We pull out the single symrec and any legal operations
     * performed on it.
     *
     * Also, if we find a float anywhere, we don't allow mixing of a single
     * symrec with it.
     */
    switch (e->op) {
	case YASM_EXPR_ADD:
	    /* Okay for single symrec anywhere in expr.
	     * Check for single symrec anywhere.
	     * Handle symrec-symrec by checking for (-1*symrec)
	     * and symrec term pairs (where both symrecs are in the same
	     * segment).
	     */
	    if (e->numterms > 32)
		yasm__fatal(N_("expression on line %d has too many add terms;"
			       " internal limit of 32"), e->line);

	    used = 0;

	    for (i=0; i<e->numterms; i++) {
		int j;
		yasm_expr *sube;
		yasm_intnum *intn;
		yasm_symrec *sym;
		/*@dependent@*/ yasm_section *sect2;
		/*@dependent@*/ /*@null@*/ yasm_bytecode *precbc2;

		/* First look for an (-1*symrec) term */
		if (e->terms[i].type != YASM_EXPR_EXPR)
		    continue;
		sube = e->terms[i].data.expn;

		if (sube->op != YASM_EXPR_MUL || sube->numterms != 2) {
		    /* recurse instead */
		    if (value_finalize_scan(value, sube, ssym_not_ok))
			return 1;
		    continue;
		}

		if (sube->terms[0].type == YASM_EXPR_INT &&
		    (sube->terms[1].type == YASM_EXPR_SYM ||
		     sube->terms[1].type == YASM_EXPR_SYMEXP)) {
		    intn = sube->terms[0].data.intn;
		    sym = sube->terms[1].data.sym;
		} else if ((sube->terms[0].type == YASM_EXPR_SYM ||
			    sube->terms[0].type == YASM_EXPR_SYMEXP) &&
			   sube->terms[1].type == YASM_EXPR_INT) {
		    sym = sube->terms[0].data.sym;
		    intn = sube->terms[1].data.intn;
		} else {
		    if (value_finalize_scan(value, sube, ssym_not_ok))
			return 1;
		    continue;
		}

		if (!yasm_intnum_is_neg1(intn)) {
		    if (value_finalize_scan(value, sube, ssym_not_ok))
			return 1;
		    continue;
		}

		if (!yasm_symrec_get_label(sym, &precbc)) {
		    if (value_finalize_scan(value, sube, ssym_not_ok))
			return 1;
		    continue;
		}
		sect2 = yasm_bc_get_section(precbc);

		/* Now look for a unused symrec term in the same segment */
		for (j=0; j<e->numterms; j++) {
		    if ((e->terms[j].type == YASM_EXPR_SYM
			 || e->terms[j].type == YASM_EXPR_SYMEXP)
			&& yasm_symrec_get_label(e->terms[j].data.sym,
						 &precbc2)
			&& (sect = yasm_bc_get_section(precbc2))
			&& sect == sect2
			&& (used & (1<<j)) == 0) {
			/* Mark as used */
			used |= 1<<j;
			break;	/* stop looking */
		    }
		}

		/* We didn't match in the same segment.  If the
		 * -1*symrec is actually -1*curpos, we can match
		 * unused symrec terms in other segments and generate
		 * a curpos-relative reloc.
		 *
		 * Don't do this if we've already become curpos-relative.
		 * The unmatched symrec will be caught below.
		 */
		if (j == e->numterms && yasm_symrec_is_curpos(sym)
		    && !value->curpos_rel) {
		    for (j=0; j<e->numterms; j++) {
			if ((e->terms[j].type == YASM_EXPR_SYM
			    || e->terms[j].type == YASM_EXPR_SYMEXP)
			    && yasm_symrec_get_label(e->terms[j].data.sym,
						     &precbc2)
			    && (used & (1<<j)) == 0) {
			    /* Mark as used */
			    used |= 1<<j;
			    /* Mark value as curpos-relative */
			    if (value->rel || ssym_not_ok)
				return 1;
			    value->rel = e->terms[j].data.sym;
			    value->curpos_rel = 1;
			    /* Replace both symrec portions with 0 */
			    yasm_expr_destroy(sube);
			    e->terms[i].type = YASM_EXPR_INT;
			    e->terms[i].data.intn = yasm_intnum_create_uint(0);
			    e->terms[j].type = YASM_EXPR_INT;
			    e->terms[j].data.intn = yasm_intnum_create_uint(0);
			    break;	/* stop looking */
			}
		    }
		}

		if (j == e->numterms)
		    return 1;	/* We didn't find a match! */
	    }

	    /* Look for unmatched symrecs.  If we've already found one or
	     * we don't WANT to find one, error out.
	     */
	    for (i=0; i<e->numterms; i++) {
		if ((e->terms[i].type == YASM_EXPR_SYM
		     || e->terms[i].type == YASM_EXPR_SYMEXP)
		    && (used & (1<<i)) == 0) {
		    if (value->rel || ssym_not_ok)
			return 1;
		    value->rel = e->terms[i].data.sym;
		    /* and replace with 0 */
		    e->terms[i].type = YASM_EXPR_INT;
		    e->terms[i].data.intn = yasm_intnum_create_uint(0);
		}
	    }
	    break;
	case YASM_EXPR_SHR:
	    /* Okay for single symrec in LHS and constant on RHS.
	     * Single symrecs are not okay on RHS.
	     * If RHS is non-constant, don't allow single symrec on LHS.
	     * XXX: should rshift be an expr instead??
	     */

	    /* Check for not allowed cases on RHS */
	    switch (e->terms[1].type) {
		case YASM_EXPR_REG:
		case YASM_EXPR_FLOAT:
		    return 1;		/* not legal */
		case YASM_EXPR_SYM:
		case YASM_EXPR_SYMEXP:
		    return 1;
		case YASM_EXPR_EXPR:
		    if (value_finalize_scan(value, e->terms[1].data.expn, 1))
			return 1;
		    break;
		default:
		    break;
	    }

	    /* Check for single sym and allowed cases on LHS */
	    switch (e->terms[0].type) {
		/*case YASM_EXPR_REG:   ????? should this be illegal ????? */
		case YASM_EXPR_FLOAT:
		    return 1;		/* not legal */
		case YASM_EXPR_SYM:
		case YASM_EXPR_SYMEXP:
		    if (value->rel || ssym_not_ok)
			return 1;
		    value->rel = e->terms[0].data.sym;
		    /* and replace with 0 */
		    e->terms[0].type = YASM_EXPR_INT;
		    e->terms[0].data.intn = yasm_intnum_create_uint(0);
		    break;
		case YASM_EXPR_EXPR:
		    /* recurse */
		    if (value_finalize_scan(value, e->terms[0].data.expn,
					    ssym_not_ok))
			return 1;
		    break;
		default:
		    break;	/* ignore */
	    }

	    /* Handle RHS */
	    if (!value->rel)
		break;		/* no handling needed */
	    if (e->terms[1].type != YASM_EXPR_INT)
		return 1;	/* can't shift sym by non-constant integer */
	    shamt = yasm_intnum_get_uint(e->terms[1].data.intn);
	    if ((shamt + value->rshift) > YASM_VALUE_RSHIFT_MAX)
		return 1;	/* total shift would be too large */
	    value->rshift += shamt;

	    /* Just leave SHR in place */
	    break;
	case YASM_EXPR_SEG:
	    /* Okay for single symrec (can only be done once).
	     * Not okay for anything BUT a single symrec as an immediate
	     * child.
	     */
	    if (e->terms[0].type != YASM_EXPR_SYM
		&& e->terms[0].type != YASM_EXPR_SYMEXP)
		return 1;

	    if (value->seg_of)
		return 1;	/* multiple SEG not legal */
	    value->seg_of = 1;

	    if (value->rel || ssym_not_ok)
		return 1;	/* got a relative portion somewhere else? */
	    value->rel = e->terms[0].data.sym;

	    /* replace with ident'ed 0 */
	    e->op = YASM_EXPR_IDENT;
	    e->terms[0].type = YASM_EXPR_INT;
	    e->terms[0].data.intn = yasm_intnum_create_uint(0);
	    break;
	case YASM_EXPR_WRT:
	    /* Okay for single symrec in LHS and either a register or single
	     * symrec (as an immediate child) on RHS.
	     * If a single symrec on RHS, can only be done once.
	     * WRT reg is left in expr for arch to look at.
	     */

	    /* Handle RHS */
	    switch (e->terms[1].type) {
		case YASM_EXPR_SYM:
		case YASM_EXPR_SYMEXP:
		    if (value->wrt)
			return 1;
		    value->wrt = e->terms[1].data.sym;
		    /* and drop the WRT portion */
		    e->op = YASM_EXPR_IDENT;
		    e->numterms = 1;
		    break;
		case YASM_EXPR_REG:
		    break;  /* ignore */
		default:
		    return 1;
	    }

	    /* Handle LHS */
	    switch (e->terms[0].type) {
		case YASM_EXPR_SYM:
		case YASM_EXPR_SYMEXP:
		    if (value->rel || ssym_not_ok)
			return 1;
		    value->rel = e->terms[0].data.sym;
		    /* and replace with 0 */
		    e->terms[0].type = YASM_EXPR_INT;
		    e->terms[0].data.intn = yasm_intnum_create_uint(0);
		    break;
		case YASM_EXPR_EXPR:
		    /* recurse */
		    return value_finalize_scan(value, e->terms[0].data.expn,
					       ssym_not_ok);
		default:
		    break;  /* ignore */
	    }

	    break;
	default:
	    /* Single symrec not allowed anywhere */
	    for (i=0; i<e->numterms; i++) {
		switch (e->terms[i].type) {
		    case YASM_EXPR_SYM:
		    case YASM_EXPR_SYMEXP:
			return 1;
		    case YASM_EXPR_EXPR:
			/* recurse */
			return value_finalize_scan(value,
						   e->terms[i].data.expn, 1);
		    default:
			break;
		}
	    }
	    break;
    }

    return 0;
}

int
yasm_value_finalize_expr(yasm_value *value, yasm_expr *e)
{
    int error = 0;

    if (!e) {
	yasm_value_initialize(value, NULL);
	return 0;
    }

    yasm_value_initialize(value, yasm_expr__level_tree
			  (e, 1, 1, 0, NULL, NULL, NULL, NULL, &error));

    /* quit early if there was an issue in simplify() */
    if (error)
	return 1;

    /* Handle trivial (IDENT) cases immediately */
    if (value->abs->op == YASM_EXPR_IDENT) {
	switch (value->abs->terms[0].type) {
	    case YASM_EXPR_INT:
		if (yasm_intnum_is_zero(value->abs->terms[0].data.intn)) {
		    yasm_expr_destroy(value->abs);
		    value->abs = NULL;
		}
		return 0;
	    case YASM_EXPR_REG:
	    case YASM_EXPR_FLOAT:
		return 0;
	    case YASM_EXPR_SYM:
	    case YASM_EXPR_SYMEXP:
		value->rel = value->abs->terms[0].data.sym;
		yasm_expr_destroy(value->abs);
		value->abs = NULL;
		return 0;
	    case YASM_EXPR_EXPR:
		/* Bring up lower values. */
		while (value->abs->op == YASM_EXPR_IDENT
		       && value->abs->terms[0].type == YASM_EXPR_EXPR) {
		    yasm_expr *sube = value->abs->terms[0].data.expn;
		    yasm_xfree(value->abs);
		    value->abs = sube;
		}
		break;
	    default:
		yasm_internal_error(N_("unexpected expr term type"));
	}
    }

    if (value_finalize_scan(value, value->abs, 0))
	return 1;

    value->abs = yasm_expr__level_tree(value->abs, 1, 1, 0, NULL, NULL, NULL,
				       NULL, NULL);

    /* Simplify 0 in abs to NULL */
    if (value->abs->op == YASM_EXPR_IDENT
	&& value->abs->terms[0].type == YASM_EXPR_INT
	&& yasm_intnum_is_zero(value->abs->terms[0].data.intn)) {
	yasm_expr_destroy(value->abs);
	value->abs = NULL;
    }
    return 0;
}

int
yasm_value_output_basic(yasm_value *value, /*@out@*/ unsigned char *buf,
			size_t destsize, size_t valsize, int shift,
			yasm_bytecode *bc, int warn, yasm_arch *arch,
			yasm_calc_bc_dist_func calc_bc_dist)
{
    /*@dependent@*/ /*@null@*/ yasm_intnum *intn = NULL;
    /*@only@*/ yasm_intnum *outval;
    int sym_local;
    int retval = 1;

    if (value->abs) {
	/* Handle floating point expressions */
	if (!value->rel && value->abs->op == YASM_EXPR_IDENT
	    && value->abs->terms[0].type == YASM_EXPR_FLOAT) {
	    if (shift < 0)
		yasm_internal_error(N_("attempting to negative shift a float"));
	    if (yasm_arch_floatnum_tobytes(arch, value->abs->terms[0].data.flt,
					   buf, destsize, valsize,
					   (unsigned int)shift, warn,
					   bc->line))
		return -1;
	    else
		return 1;
	}

	/* Check for complex float expressions */
	if (yasm_expr__contains(value->abs, YASM_EXPR_FLOAT)) {
	    yasm__error(bc->line, N_("floating point expression too complex"));
	    return -1;
	}

	/* Handle integer expressions */
	intn = yasm_expr_get_intnum(&value->abs, calc_bc_dist);
	if (!intn) {
	    yasm__error(bc->line, N_("expression too complex"));
	    return -1;
	}
    }

    if (value->rel) {
	/* If relative portion is not in bc section, don't try to handle it
	 * here.  Otherwise get the relative portion's offset.
	 */
	/*@dependent@*/ yasm_bytecode *rel_prevbc;
	unsigned long dist;

	sym_local = yasm_symrec_get_label(value->rel, &rel_prevbc);
	if (value->wrt || value->seg_of || value->section_rel || !sym_local)
	    return 0;	    /* we can't handle SEG, WRT, or external symbols */
	if (rel_prevbc->section != bc->section)
	    return 0;	    /* not in this section */
	if (!value->curpos_rel)
	    return 0;	    /* not PC-relative */

	/* Calculate value relative to current assembly position */
	dist = rel_prevbc->offset + rel_prevbc->len;
	if (dist < bc->offset) {
	    outval = yasm_intnum_create_uint(bc->offset - dist);
	    yasm_intnum_calc(outval, YASM_EXPR_NEG, NULL, bc->line);
	} else {
	    dist -= bc->offset;
	    outval = yasm_intnum_create_uint(dist);
	}

	if (value->rshift > 0) {
	    /*@only@*/ yasm_intnum *shamt =
		yasm_intnum_create_uint((unsigned long)value->rshift);
	    yasm_intnum_calc(outval, YASM_EXPR_SHR, shamt, bc->line);
	    yasm_intnum_destroy(shamt);
	}
	/* Add in absolute portion */
	if (intn)
	    yasm_intnum_calc(outval, YASM_EXPR_ADD, intn, bc->line);
	/* Output! */
	if (yasm_arch_intnum_tobytes(arch, outval, buf, destsize, valsize,
				     shift, bc, warn, bc->line))
	    retval = -1;
	yasm_intnum_destroy(outval);
    } else if (intn) {
	/* Output just absolute portion */
	if (yasm_arch_intnum_tobytes(arch, intn, buf, destsize, valsize,
				     shift, bc, warn, bc->line))
	    retval = -1;
    } else {
	/* No absolute or relative portions: output 0 */
	outval = yasm_intnum_create_uint(0);
	if (yasm_arch_intnum_tobytes(arch, outval, buf, destsize, valsize,
				     shift, bc, warn, bc->line))
	    retval = -1;
	yasm_intnum_destroy(outval);
    }
    return retval;
}

void
yasm_value_print(const yasm_value *value, FILE *f, int indent_level)
{
    fprintf(f, "%*sAbsolute portion=", indent_level, "");
    yasm_expr_print(value->abs, f);
    fprintf(f, "\n");
    if (value->rel) {
	fprintf(f, "%*sRelative to=%s%s\n", indent_level, "",
		value->seg_of ? "SEG " : "",
		yasm_symrec_get_name(value->rel));
	if (value->wrt)
	    fprintf(f, "%*s(With respect to=%s)\n", indent_level, "",
		    yasm_symrec_get_name(value->wrt));
	if (value->rshift > 0)
	    fprintf(f, "%*s(Right shifted by=%u)\n", indent_level, "",
		    value->rshift);
	if (value->curpos_rel)
	    fprintf(f, "%*s(Relative to current position)\n", indent_level,
		    "");
    }
}