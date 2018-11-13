/*
 * Oracle Linux DTrace.
 * Copyright (c) 2005, 2018, Oracle and/or its affiliates. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at
 * http://oss.oracle.com/licenses/upl.
 */

#include <sys/types.h>
#include <sys/dtrace_bpf.h>

#include <linux/bpf.h>

#include <strings.h>
#include <stdlib.h>
#include <setjmp.h>
#include <stdarg.h>
#include <assert.h>
#include <errno.h>

#include <dt_impl.h>
#include <dt_grammar.h>
#include <dt_parser.h>
#include <dt_provider.h>

const char *bpf_protos[] = {
	"rri",					/* dtrace_copys */
	"i",					/* dtrace_sets */
	"rr",					/* dtrace_strlen */
	"ir",					/* dtrace_set_global */
	"ir",					/* dtrace_set_thread */
	"ir",					/* dtrace_set_local */
	"irdi",					/* dtrace_set_global_assoc */
	"irdi",					/* dtrace_set_thread_assoc */
	"i",					/* dtrace_get_global */
	"i",					/* dtrace_get_thread */
	"i",					/* dtrace_get_local */
	"idi",					/* dtrace_get_global_assoc */
	"idi",					/* dtrace_get_thread_assoc */
	"ir",					/* dtrace_get_global_array */
	"ir",					/* dtrace_get_thread_array */
	"rr",					/* dtrace_strcmp */
	"r",					/* dtrace_alloc_scratch */
	"idi",					/* dtrace_subr */
}

static void dt_cg_node(dt_node_t *, dt_irlist_t *, dt_regset_t *);

static dt_irnode_t *
dt_cg_node_alloc_labelled(struct bpf_insn instr, uint_t label)
{
	dt_irnode_t *dip = malloc(sizeof (dt_irnode_t));

	if (dip == NULL)
		longjmp(yypcb->pcb_jmpbuf, EDT_NOMEM);

	dip->di_label = label;
	dip->di_instr = instr;
	dip->di_extern = NULL;
	dip->di_next = NULL;

	return (dip);
}

static dt_irnode_t *
dt_cg_node_alloc(struct bpf_insn instr)
{
	return dt_cg_node_labelled(DT_LBL_NONE, instr);
}

/*
 * Code generator wrapper function for ctf_member_info.  If we are given a
 * reference to a forward declaration tag, search the entire type space for
 * the actual definition and then call ctf_member_info on the result.
 */
static ctf_file_t *
dt_cg_membinfo(ctf_file_t *fp, ctf_id_t type, const char *s, ctf_membinfo_t *mp)
{
	while (ctf_type_kind(fp, type) == CTF_K_FORWARD) {
		char n[DT_TYPE_NAMELEN];
		dtrace_typeinfo_t dtt;

		if (ctf_type_name(fp, type, n, sizeof (n)) == NULL ||
		    dt_type_lookup(n, &dtt) == -1 || (
		    dtt.dtt_ctfp == fp && dtt.dtt_type == type))
			break; /* unable to improve our position */

		fp = dtt.dtt_ctfp;
		type = ctf_type_resolve(fp, dtt.dtt_type);
	}

	if (ctf_member_info(fp, type, s, mp) == CTF_ERR)
		return (NULL); /* ctf_errno is set for us */

	return (fp);
}

/*
 * Register-to-register moves between two allocated registers.
 */
static void
dt_cg_mov(dt_irlist_t *dlp, int to, int from)
{
	struct bpf_insn instr;

	BPF_MOV64_REG(to, from);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));
}


/*
 * Spill regs to the stack, and unspill them later.
 */
static int
dt_spill(int reg, void *d)
{
	dt_irlist_t *dlp = d;
	yypcb->stackdepth += 8;

	dt_irlist_append(dlp, dt_cg_node_alloc(
		    BPF_STX_MEM(BPF_DW, BPF_REG_FP,
			reg, -yypcb->stackkdepth)));
	return 0;
}

static int
dt_unspill(int reg, void *d)
{
	dt_irlist_t *dlp = d;

	dt_irlist_append(dlp, dt_cg_node_alloc(
		    BPF_LDX_MEM(BPF_DW, reg, BPF_REG_FP,
			-yypcb->stackdepth)));
	yypcb->stackdepth -= 8;
	return 0;
}

/*
 * A helper function call.  This happens a *lot*.  Even variable allocation/
 * lookup is a helper call.
 *
 * The args are up to five ints denoting a BPF register number, an immediate
 * value, or a stackdepth indicator (according to the corresponding letter in
 * bpf_protos[] for this helper).  There is no validation (yet) that the right
 * number of args are used in the call.
 *
 * 'd', for the stackdepth indicator, is substituted with the address of the
 * stack pointer adjusted by the pcb_stackdepth, to indicate how far back to
 * look to see the stack of an arglist.
 *
 * Used regs below the BPF_NCLOBBERED bound will be spilled to the stack and
 * restored on function return.  r0 is clobbered with the function return value:
 * an error will be returned if it is in use on function entry.
 *
 * Note: the return register is not preserved in the regset, and must be
 * explicitly moved away if needed across more than one instruction.
 */
static int
dt_cg_call(dt_irlist_t *dlp, dt_regset_t *drp, uint32_t helper,
    ...)
{
	va_list ap;
	uint_t reg = 1;
	char *argstr;
	int needs_stackdepth = 0;

	if (BT_TEST(drp->dr_bitmap, 0) != 0)
		longjmp(yypcb->pcb_jmpbuf, EDT_RESERVEDREG);
	if (helper < FIRST_BPF_HELPER ||
	    helper - FIRST_BPF_HELPER > sizeof(bpf_protos))
		longjmp(yypcb->pcb_jmpbuf, EDT_INVALIDBPFHELPER);

	/*
	 * Compute the stack depth, if needed.  Keep it stuffed in r0, which we
	 * know is clobbered regardless.
	 */
	for (argstr = bpf_protos[helper - FIRST_BPF_HELPER];
	     argstr != '\0'; argstr++, reg++)
		if (*argstr == 'd') {
			needs_stackdepth = 1;
			break;
		}

	if (needs_stackdepth) {
		dt_cg_mov(dlp, BPF_REG_0, BPF_REG_FP);
		dt_irlist_append(dlp, dt_cg_node_alloc(
			    BPF_ALU64_REG(BPF_SUB, BPF_REG_0, yypcb->pcb_stackdepth)));
	}

	dt_regset_iter(drp, 1, BPF_NCLOBBERED, dt_spill, &dlp);

	va = va_start(ap, helper);
	for (argstr = bpf_protos[helper - FIRST_BPF_HELPER];
	     argstr != '\0'; argstr++, reg++) {
		uint32_t arg = va_arg(ap, int);

		switch (*argstr) {
		case 'r': dt_cg_mov(dlp, reg, arg); break;
		case 'i': dt_cg_setx(dlp, reg, arg); break;
		/* dt_cg_call() arg not used at all for 'd' (and skipped). */
		case 'd': dt_cg_mov(dlp, reg, BPF_REG_0); break;
		default: longjmp(yypcb->pcb_jmpbuf, EDT_INVALIDBPFHELPER);
		}
	}
	va_end(ap);

	dt_irlist_append(dlp, dt_cg_node_alloc(
		    BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, helper)));

	dt_regset_iter(drp, BPF_NCLOBBERED, 1, dt_unspill, &dlp);

	/*
	 * Return is always in r0: BPF ABI.
	 */
	return BPF_REG_0;
}

static void
dt_cg_xsetx(dt_irlist_t *dlp, dt_ident_t *idp, uint_t lbl, int reg, uint64_t x)
{
	struct bpf_insn instr[2];

	if (intoff == -1)
		longjmp(yypcb->pcb_jmpbuf, EDT_NOMEM);

	if (x < (unsigned int) -1) {
		instr[0] = BPF_MOV32_IMM(reg, x);
		dt_irlist_append(dlp, dt_cg_node_alloc_labelled(lbl, instr[0]));
	} else {
		instr = BPF_LD_IMM64(reg, x);
		dt_irlist_append(dlp, dt_cg_node_alloc_labelled(lbl, instr[0]));
		dt_irlist_append(dlp, dt_cg_node_alloc(instr[1]));
	}

	if (idp != NULL)
		dlp->dl_last->di_extern = idp;
}

static void
dt_cg_setx(dt_irlist_t *dlp, int reg, uint64_t x)
{
	dt_cg_xsetx(dlp, NULL, DT_LBL_NONE, reg, x);
}

/*
 * When loading bit-fields, we want to convert a byte count in the range
 * 1-8 to the closest power of 2 (e.g. 3->4, 5->8, etc).  The clp2() function
 * is a clever implementation from "Hacker's Delight" by Henry Warren, Jr.
 */
static size_t
clp2(size_t x)
{
	x--;

	x |= (x >> 1);
	x |= (x >> 2);
	x |= (x >> 4);
	x |= (x >> 8);
	x |= (x >> 16);

	return (x + 1);
}

/*
 * Lookup the correct load opcode to use for the specified node and CTF type.
 * We determine the size and convert it to a 3-bit index.  Our lookup table
 * is constructed to use a 6-bit index, consisting of the 4-bit size 0-15, a
 * bit for the sign, and a bit for userland address.  For example, a 4-byte
 * signed load from userland would be at the following table index:
 * user=1 sign=1 size=4 => binary index 110011 = decimal index 51
 */
static uint_t
dt_cg_load(dt_node_t *dnp, ctf_file_t *ctfp, ctf_id_t type)
{
	/* XXX signs; userland */

	static const uint_t ops[] = {
		BPF_B,	BPF_H,	0,	BPF_W,	BPF_DW,
		BPF_B,	BPF_H,	0,	BPF_W,	BPF_DW,
		BPF_B,	BPF_H,	0,	BPF_W,  BPF_DW,
		BPF_B,	BPF_H,	0,	BPF_W,	BPF_DW,
/*		DIF_OP_ULDUB,	DIF_OP_ULDUH,	0,	DIF_OP_ULDUW,
		0,		0,		0,	DIF_OP_ULDX,
		DIF_OP_ULDSB,	DIF_OP_ULDSH,	0,	DIF_OP_ULDSW,
		0,		0,		0,	DIF_OP_ULDX, */
	};

	ctf_encoding_t e;
	ssize_t size;

	/*
	 * If we're loading a bit-field, the size of our load is found by
	 * rounding cte_bits up to a byte boundary and then finding the
	 * nearest power of two to this value (see clp2(), above).
	 */
	if ((dnp->dn_flags & DT_NF_BITFIELD) &&
	    ctf_type_encoding(ctfp, type, &e) != CTF_ERR)
		size = clp2(P2ROUNDUP(e.cte_bits, NBBY) / NBBY);
	else
		size = ctf_type_size(ctfp, type);

	if (size < 1 || size > 16 || (size & (size - 1)) != 0) {
		xyerror(D_UNKNOWN, "internal error -- cg cannot load "
		    "size %ld when passed by value\n", (long)size);
	}

	size--; /* convert size to 4-bit index */

/*	if (dnp->dn_flags & DT_NF_SIGNED)
		size |= 0x10; */
	if (dnp->dn_flags & DT_NF_USERLAND) {
		xyerror(D_UNKNOWN, "internal error -- no userland loads in bpf yet\n");
	}

	return (ops[size]);
}

static void
dt_cg_ptrsize(dt_node_t *dnp, dt_irlist_t *dlp, dt_regset_t *drp,
    uint_t op, int dreg)
{
	ctf_file_t *ctfp = dnp->dn_ctfp;
	ctf_arinfo_t r;
	struct bpf_insn instr;
	ctf_id_t type;
	uint_t kind;
	ssize_t size;
	int sreg;

	if ((sreg = dt_regset_alloc(drp)) == -1)
		longjmp(yypcb->pcb_jmpbuf, EDT_NOREG);

	type = ctf_type_resolve(ctfp, dnp->dn_type);
	kind = ctf_type_kind(ctfp, type);
	assert(kind == CTF_K_POINTER || kind == CTF_K_ARRAY);

	if (kind == CTF_K_ARRAY) {
		if (ctf_array_info(ctfp, type, &r) != 0) {
			yypcb->pcb_hdl->dt_ctferr = ctf_errno(ctfp);
			longjmp(yypcb->pcb_jmpbuf, EDT_CTF);
		}
		type = r.ctr_contents;
	} else
		type = ctf_type_reference(ctfp, type);

	if ((size = ctf_type_size(ctfp, type)) == 1)
		return; /* multiply or divide by one can be omitted */

	dt_cg_setx(dlp, sreg, size);
	instr = BPF_ALU64_REG(op, dreg, sreg);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));
	dt_regset_free(drp, sreg);
}

/*
 * If the result of a "." or "->" operation is a bit-field, we use this routine
 * to generate an epilogue to the load instruction that extracts the value.  In
 * the diagrams below the "ld??" is the load instruction that is generated to
 * load the containing word that is generating prior to calling this function.
 *
 * XXX rewrite comment!!
 * 
 * Epilogue for unsigned fields:	Epilogue for signed fields:
 *
 * ldu?	[r1], r1			lds? [r1], r1
 * setx	USHIFT, r2			setx 64 - SSHIFT, r2
 * srl	r1, r2, r1			sll  r1, r2, r1
 * setx	(1 << bits) - 1, r2		setx 64 - bits, r2
 * and	r1, r2, r1			sra  r1, r2, r1
 *
 * The *SHIFT constants above changes value depending on the endian-ness of our
 * target architecture.  Refer to the comments below for more details.
 */
static void
dt_cg_field_get(dt_node_t *dnp, dt_irlist_t *dlp, dt_regset_t *drp,
    ctf_file_t *fp, const ctf_membinfo_t *mp)
{
	ctf_encoding_t e;
	struct bpf_insn instr;
	uint64_t shift;
	int r1, r2;

	/*
	 * XXX determination of bitfields is wrong: fix trivial.
	 */

	if (ctf_type_encoding(fp, mp->ctm_type, &e) != 0 || e.cte_bits > 64) {
		xyerror(D_UNKNOWN, "cg: bad field: off %lu type <%ld> "
		    "bits %u\n", mp->ctm_offset, mp->ctm_type, e.cte_bits);
	}

	assert(dnp->dn_op == DT_TOK_PTR || dnp->dn_op == DT_TOK_DOT);
	r1 = dnp->dn_left->dn_reg;

	if ((r2 = dt_regset_alloc(drp)) == -1)
		longjmp(yypcb->pcb_jmpbuf, EDT_NOREG);

	/*
	 * On little-endian architectures, ctm_offset counts from the right so
	 * ctm_offset % NBBY itself is the amount we want to shift right to
	 * move the value bits to the little end of the register to mask them.
	 * On big-endian architectures, ctm_offset counts from the left so we
	 * must subtract (ctm_offset % NBBY + cte_bits) from the size in bits
	 * we used for the load.  The size of our load in turn is found by
	 * rounding cte_bits up to a byte boundary and then finding the
	 * nearest power of two to this value (see clp2(), above).  These
	 * properties are used to compute shift as USHIFT or SSHIFT, below.
	 */
	/* XXX check signs: SLL/SRL versus LSH? prob ARSH, used that */
	if (dnp->dn_flags & DT_NF_SIGNED) {
#ifdef _BIG_ENDIAN
		shift = clp2(P2ROUNDUP(e.cte_bits, NBBY) / NBBY) * NBBY -
		    mp->ctm_offset % NBBY;
#else
		shift = mp->ctm_offset % NBBY + e.cte_bits;
#endif
		dt_cg_setx(dlp, r2, 64 - shift);
		instr = BPF_ALU64_REG(BPF_LSH, r2, r1);
		dt_irlist_append(dlp, dt_cg_node_alloc(instr));

		dt_cg_setx(dlp, r2, 64 - e.cte_bits);
		instr = BPF_ALU64_REG(BPF_ARSH, r1, r2);
		dt_irlist_append(dlp, dt_cg_node_alloc(instr));
	} else {
#ifdef _BIG_ENDIAN
		shift = clp2(P2ROUNDUP(e.cte_bits, NBBY) / NBBY) * NBBY -
		    (mp->ctm_offset % NBBY + e.cte_bits);
#else
		shift = mp->ctm_offset % NBBY;
#endif
		dt_cg_setx(dlp, r2, shift);
		instr = BPF_ALU64_REG(BPF_LSH, r2, r1);
		dt_irlist_append(dlp, dt_cg_node_alloc(instr));

		dt_cg_setx(dlp, r2, (1ULL << e.cte_bits) - 1);
		instr = BPF_ALU64_REG(BPF_AND, r2, r1);
		dt_irlist_append(dlp, dt_cg_node_alloc(instr));
	}

	dt_regset_free(drp, r2);
}

/*
 * If the destination of a store operation is a bit-field, we use this routine
 * to generate a prologue to the store instruction that loads the surrounding
 * bits, clears the destination field, and ORs in the new value of the field.
 * In the diagram below the "st?" is the store instruction that is generated to
 * store the containing word that is generating after calling this function.
 *
 * XXX rewrite comment
 * 
 * ld	[dst->dn_reg], r1
 * setx	~(((1 << cte_bits) - 1) << (ctm_offset % NBBY)), r2
 * and	r1, r2, r1
 *
 * setx	(1 << cte_bits) - 1, r2
 * and	src->dn_reg, r2, r2
 * setx ctm_offset % NBBY, r3
 * sll	r2, r3, r2
 *
 * or	r1, r2, r1
 * st?	r1, [dst->dn_reg]
 *
 * This routine allocates a new register to hold the value to be stored and
 * returns it.  The caller is responsible for freeing this register later.
 */
static int
dt_cg_field_set(dt_node_t *src, dt_irlist_t *dlp,
    dt_regset_t *drp, dt_node_t *dst)
{
	uint64_t cmask, fmask, shift;
	struct bpf_insn instr;
	int r1, r2, r3;

	ctf_membinfo_t m;
	ctf_encoding_t e;
	ctf_file_t *fp, *ofp;
	ctf_id_t type;

	assert(dst->dn_op == DT_TOK_PTR || dst->dn_op == DT_TOK_DOT);
	assert(dst->dn_right->dn_kind == DT_NODE_IDENT);

	fp = dst->dn_left->dn_ctfp;
	type = ctf_type_resolve(fp, dst->dn_left->dn_type);

	if (dst->dn_op == DT_TOK_PTR) {
		type = ctf_type_reference(fp, type);
		type = ctf_type_resolve(fp, type);
	}

	if ((fp = dt_cg_membinfo(ofp = fp, type,
	    dst->dn_right->dn_string, &m)) == NULL) {
		yypcb->pcb_hdl->dt_ctferr = ctf_errno(ofp);
		longjmp(yypcb->pcb_jmpbuf, EDT_CTF);
	}

	if (ctf_type_encoding(fp, m.ctm_type, &e) != 0 || e.cte_bits > 64) {
		xyerror(D_UNKNOWN, "cg: bad field: off %lu type <%ld> "
		    "bits %u\n", m.ctm_offset, m.ctm_type, e.cte_bits);
	}

	if ((r1 = dt_regset_alloc(drp)) == -1 ||
	    (r2 = dt_regset_alloc(drp)) == -1 ||
	    (r3 = dt_regset_alloc(drp)) == -1)
		longjmp(yypcb->pcb_jmpbuf, EDT_NOREG);

	/*
	 * Compute shifts and masks.  We need to compute "shift" as the amount
	 * we need to shift left to position our field in the containing word.
	 * Refer to the comments in dt_cg_field_get(), above, for more info.
	 * We then compute fmask as the mask that truncates the value in the
	 * input register to width cte_bits, and cmask as the mask used to
	 * pass through the containing bits and zero the field bits.
	 */
#ifdef _BIG_ENDIAN
	shift = clp2(P2ROUNDUP(e.cte_bits, NBBY) / NBBY) * NBBY -
	    (m.ctm_offset % NBBY + e.cte_bits);
#else
	shift = m.ctm_offset % NBBY;
#endif
	fmask = (1ULL << e.cte_bits) - 1;
	cmask = ~(fmask << shift);

	dt_cg_mov(dlp, r1, dst->dn_reg);

	dt_cg_setx(dlp, r2, cmask);
	/* XXX check dest? */
	instr = BPF_ALU64_REG(BPF_AND, r1, r2);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));

	dt_cg_setx(dlp, r2, fmask);
	instr = BPF_ALU64_REG(BPF_AND, r2, src->dn_reg);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));

	dt_cg_setx(dlp, r3, shift);
	instr = BPF_ALU64_REG(BPF_LSH, r3, r2);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));

	instr = BPF_ALU64_REG(BPF_OR, r1, r2);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));

	dt_regset_free(drp, r3);
	dt_regset_free(drp, r2);

	return (r1);
}

static void
dt_cg_store(dt_node_t *src, dt_irlist_t *dlp, dt_regset_t *drp, dt_node_t *dst)
{
	ctf_encoding_t e;
	struct bpf_insn instr;
	size_t size;

	/*
	 * If we're loading a bit-field, the size of our store is found by
	 * rounding dst's cte_bits up to a byte boundary and then finding the
	 * nearest power of two to this value (see clp2(), above).
	 */
	if ((dst->dn_flags & DT_NF_BITFIELD) &&
	    ctf_type_encoding(dst->dn_ctfp, dst->dn_type, &e) != CTF_ERR)
		size = clp2(P2ROUNDUP(e.cte_bits, NBBY) / NBBY);
	else
		size = dt_node_type_size(src);

	if (src->dn_flags & DT_NF_REF) {
		/* XXX turn into inlined loop */
		dt_cg_call(dlp, drp, BPF_FUNC_dtrace_copys,
		    src->dn_reg, dst->dn_reg, size);
	} else {
		/* XXX this is ugly. Find a trick in Warren instead. :) */
		int reg;
		if (dst->dn_flags & DT_NF_BITFIELD)
			reg = dt_cg_field_set(src, dlp, drp, dst);
		else
			reg = src->dn_reg;

		switch (size) {
		case 1:
			size = BPF_B;
			break;
		case 2:
			size = BPF_H;
			break;
		case 4:
			size = BPF_W;
			break;
		case 8:
			size = BPF_DW;
			break;
		default:
			xyerror(D_UNKNOWN, "internal error -- cg cannot store "
			    "size %lu when passed by value\n", (ulong_t)size);
		}
		/* XXX can probably use the off more for array derefs */
		instr = BPF_STX_MEM(size, dst->dn_reg, reg, 0);
		dt_irlist_append(dlp, dt_cg_node_alloc(instr));

		if (dst->dn_flags & DT_NF_BITFIELD)
			dt_regset_free(drp, reg);
	}
}

/*
 * Generate code for a typecast or for argument promotion from the type of the
 * actual to the type of the formal.  We need to generate code for casts when
 * a scalar type is being narrowed or changing signed-ness.  We first shift the
 * desired bits high (losing excess bits if narrowing) and then shift them down
 * using logical shift (unsigned result) or arithmetic shift (signed result).
 */
static void
dt_cg_typecast(const dt_node_t *src, const dt_node_t *dst,
    dt_irlist_t *dlp, dt_regset_t *drp)
{
	size_t srcsize = dt_node_type_size(src);
	size_t dstsize = dt_node_type_size(dst);

	struct bpf_insn instr;

	if (dt_node_is_scalar(dst) && (dstsize < srcsize ||
	    (src->dn_flags & DT_NF_SIGNED) ^ (dst->dn_flags & DT_NF_SIGNED))) {
		int n;

		if (dstsize < srcsize)
			n = sizeof (uint64_t) * NBBY - dstsize * NBBY;
		else
			n = sizeof (uint64_t) * NBBY - srcsize * NBBY;

		instr = BPF_ALU64_REG(BPF_LSH, dst->dn_reg, src->dn_reg);
		dt_irlist_append(dlp, dt_cg_node_alloc(instr));

		instr = BPF_ALU64_IMM((dst->dn_flags & DT_NF_SIGNED) ?
		    BPF_ARSH : BPF_RSH, dst->dn_reg, n);

		dt_irlist_append(dlp, dt_cg_node_alloc(instr));
	}
}

/*
 * Generate code to push the specified argument list onto the stack.  We use
 * this routine for handling subroutine calls and associative arrays.  We must
 * first generate code for all subexpressions before loading the stack because
 * any subexpression could itself require the use of the stack.  (Caveat: in
 * future, it may be more efficient to spill to the callable registers first:
 * but this is probably very marginal.)
 *
 * Returns the number of args pushed.
 */
static int
dt_cg_arglist(dt_ident_t *idp, dt_node_t *args,
    dt_irlist_t *dlp, dt_regset_t *drp)
{
	const dt_idsig_t *isp = idp->di_data;
	dt_node_t *dnp;
	int i = 0;
	int argcount = 0;
	int curarg = yypcb->pcb_stackdepth;

	for (dnp = args; dnp != NULL; dnp = dnp->dn_list, argcount++)
		dt_cg_node(dnp, dlp, drp);

	yypcb->pcb_stackdepth += (argcount * 2) * 8;

	for (dnp = args; dnp != NULL; dnp = dnp->dn_list, i++) {
		dtrace_diftype_t t;
		dif_instr_t instr;
		uint_t op;
		int size_reg;

		dt_node_diftype(yypcb->pcb_hdl, dnp, &t);

		isp->dis_args[i].dn_reg = dnp->dn_reg; /* re-use register */
		dt_cg_typecast(dnp, &isp->dis_args[i], dlp, drp);
		isp->dis_args[i].dn_reg = -1;

		if (t.dtdt_flags & DIF_TF_BYREF) {
			if (t.dtdt_kind == DIF_TYPE_STRING)
				size_reg = dt_cg_call(dlp, drp,
				    BPF_FUNC_dtrace_strlen, dnp->dn_reg,
				    t.dtdt_size);
			else {
				dt_cg_setx(dlp, BPF_REG_R0, t.dtdt_size);
				size_reg = BPF_REG_R0;
			}
		}
		/* value */
		dt_irlist_append(dlp, dt_cg_node_alloc(
			    BPF_STX_MEM(BPF_DW, BPF_REG_FP, dnp->dn_reg,
				-curarg)));
		curarg += 8;

		/* size, 0 for non-byref */
		if (t.dtdt_flags & DIF_TF_BYREF) {
			dt_irlist_append(dlp, dt_cg_node_alloc(
				    BPF_ST_MEM(BPF_DW, BPF_REG_FP, -curarg, 0)));
		} else {
			dt_irlist_append(dlp, dt_cg_node_alloc(
				    BPF_STX_MEM(BPF_DW, BPF_REG_FP, size_reg,
					-curarg)));
		}
		curarg += 8;
	}

	return argcount;
}

static void
dt_cg_arithmetic_op(dt_node_t *dnp, dt_irlist_t *dlp,
    dt_regset_t *drp, uint_t op)
{
	int is_ptr_op = (dnp->dn_op == DT_TOK_ADD || dnp->dn_op == DT_TOK_SUB ||
	    dnp->dn_op == DT_TOK_ADD_EQ || dnp->dn_op == DT_TOK_SUB_EQ);

	int lp_is_ptr = dt_node_is_pointer(dnp->dn_left);
	int rp_is_ptr = dt_node_is_pointer(dnp->dn_right);

	struct bpf_insn instr;

	if (lp_is_ptr && rp_is_ptr) {
		assert(dnp->dn_op == DT_TOK_SUB);
		is_ptr_op = 0;
	}

	dt_cg_node(dnp->dn_left, dlp, drp);
	if (is_ptr_op && rp_is_ptr)
		dt_cg_ptrsize(dnp, dlp, drp, BPF_MUL, dnp->dn_left->dn_reg);

	dt_cg_node(dnp->dn_right, dlp, drp);
	if (is_ptr_op && lp_is_ptr)
		dt_cg_ptrsize(dnp, dlp, drp, BPF_MUL, dnp->dn_right->dn_reg);

	instr = BPF_ALU64_REG(op, dnp->dn_left->dn_reg,
	    dnp->dn_right->dn_reg);

	dt_irlist_append(dlp, dt_cg_node_alloc(instr));
	dt_regset_free(drp, dnp->dn_right->dn_reg);
	dnp->dn_reg = dnp->dn_left->dn_reg;

	if (lp_is_ptr && rp_is_ptr)
		dt_cg_ptrsize(dnp->dn_right, dlp, drp, BP_DIV, dnp->dn_reg);
}

static uint_t
dt_cg_stvar(const dt_ident_t *idp)
{
	static const uint_t aops[] = { BPF_FUNC_dtrace_set_global_assoc,
				       BPF_FUNC_dtrace_set_thread_assoc, 0 };
	static const uint_t sops[] = { BPF_FUNC_dtrace_set_global,
				       BPF_FUNC_dtrace_set_thread,
				       BPF_FUNC_dtrace_set_local };

	uint_t i = (((idp->di_flags & DT_IDFLG_LOCAL) != 0) << 1) |
	    ((idp->di_flags & DT_IDFLG_TLS) != 0);

	return (idp->di_kind == DT_IDENT_ARRAY ? aops[i] : sops[i]);
}

static void
dt_cg_prearith_op(dt_node_t *dnp, dt_irlist_t *dlp, dt_regset_t *drp, uint_t op)
{
	ctf_file_t *ctfp = dnp->dn_ctfp;
	struct bpf_insn instr;
	ctf_id_t type;
	ssize_t size = 1;
	int reg;

	if (dt_node_is_pointer(dnp)) {
		type = ctf_type_resolve(ctfp, dnp->dn_type);
		assert(ctf_type_kind(ctfp, type) == CTF_K_POINTER);
		size = ctf_type_size(ctfp, ctf_type_reference(ctfp, type));
	}

	dt_cg_node(dnp->dn_child, dlp, drp);
	dnp->dn_reg = dnp->dn_child->dn_reg;

	if ((reg = dt_regset_alloc(drp)) == -1)
		longjmp(yypcb->pcb_jmpbuf, EDT_NOREG);

	dt_cg_setx(dlp, reg, size);

	instr = BPF_ALU64_REG(op, dnp->dn_reg, reg);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));
	dt_regset_free(drp, reg);

	/*
	 * If we are modifying a variable, generate an stv instruction from
	 * the variable specified by the identifier.  If we are storing to a
	 * memory address, generate code again for the left-hand side using
	 * DT_NF_REF to get the address, and then generate a store to it.
	 * In both paths, we store the value in dnp->dn_reg (the new value).
	 */
	if (dnp->dn_child->dn_kind == DT_NODE_VAR) {
		dt_ident_t *idp = dt_ident_resolve(dnp->dn_child->dn_ident);

		idp->di_flags |= DT_IDFLG_DIFW;
		dt_cg_call(dlp, drp, dt_cg_stvar(idp), idp->di_id, dnp->dn_reg);
	} else {
		uint_t rbit = dnp->dn_child->dn_flags & DT_NF_REF;

		assert(dnp->dn_child->dn_flags & DT_NF_WRITABLE);
		assert(dnp->dn_child->dn_flags & DT_NF_LVALUE);

		dnp->dn_child->dn_flags |= DT_NF_REF; /* force pass-by-ref */
		dt_cg_node(dnp->dn_child, dlp, drp);

		dt_cg_store(dnp, dlp, drp, dnp->dn_child);
		dt_regset_free(drp, dnp->dn_child->dn_reg);

		dnp->dn_left->dn_flags &= ~DT_NF_REF;
		dnp->dn_left->dn_flags |= rbit;
	}
}

static void
dt_cg_postarith_op(dt_node_t *dnp, dt_irlist_t *dlp,
    dt_regset_t *drp, uint_t op)
{
	ctf_file_t *ctfp = dnp->dn_ctfp;
	struct bpf_insn instr;
	ctf_id_t type;
	ssize_t size = 1;
	int nreg;

	if (dt_node_is_pointer(dnp)) {
		type = ctf_type_resolve(ctfp, dnp->dn_type);
		assert(ctf_type_kind(ctfp, type) == CTF_K_POINTER);
		size = ctf_type_size(ctfp, ctf_type_reference(ctfp, type));
	}

	dt_cg_node(dnp->dn_child, dlp, drp);
	dnp->dn_reg = dnp->dn_child->dn_reg;

	if ((nreg = dt_regset_alloc(drp)) == -1)
		longjmp(yypcb->pcb_jmpbuf, EDT_NOREG);

	dt_cg_setx(dlp, nreg, size);
	instr = BPF_ALU64_REG(op, nreg, dnp->dn_reg);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));

	/*
	 * If we are modifying a variable, generate an stv instruction from
	 * the variable specified by the identifier.  If we are storing to a
	 * memory address, generate code again for the left-hand side using
	 * DT_NF_REF to get the address, and then generate a store to it.
	 * In both paths, we store the value from 'nreg' (the new value).
	 */
	if (dnp->dn_child->dn_kind == DT_NODE_VAR) {
		dt_ident_t *idp = dt_ident_resolve(dnp->dn_child->dn_ident);

		idp->di_flags |= DT_IDFLG_DIFW;
		dt_cg_call(dlp, drp, dt_cg_stvar(idp), idp->di_id, nreg);
	} else {
		uint_t rbit = dnp->dn_child->dn_flags & DT_NF_REF;
		int oreg = dnp->dn_reg;

		assert(dnp->dn_child->dn_flags & DT_NF_WRITABLE);
		assert(dnp->dn_child->dn_flags & DT_NF_LVALUE);

		dnp->dn_child->dn_flags |= DT_NF_REF; /* force pass-by-ref */
		dt_cg_node(dnp->dn_child, dlp, drp);

		dnp->dn_reg = nreg;
		dt_cg_store(dnp, dlp, drp, dnp->dn_child);
		dnp->dn_reg = oreg;

		dt_regset_free(drp, dnp->dn_child->dn_reg);
		dnp->dn_left->dn_flags &= ~DT_NF_REF;
		dnp->dn_left->dn_flags |= rbit;
	}

	dt_regset_free(drp, nreg);
}

/*
 * Determine if we should perform signed or unsigned comparison for an OP2.
 * If both operands are of arithmetic type, perform the usual arithmetic
 * conversions to determine the common real type for comparison [ISOC 6.5.8.3].
 */
static int
dt_cg_compare_signed(dt_node_t *dnp)
{
	dt_node_t dn;

	/* XXX signedness: need to do the usual conversions by hand */

	if (dt_node_is_string(dnp->dn_left) ||
	    dt_node_is_string(dnp->dn_right))
		return (1); /* strings always compare signed */
	else if (!dt_node_is_arith(dnp->dn_left) ||
	    !dt_node_is_arith(dnp->dn_right))
		return (0); /* non-arithmetic types always compare unsigned */

	bzero(&dn, sizeof (dn));
	dt_node_promote(dnp->dn_left, dnp->dn_right, &dn);
	return (dn.dn_flags & DT_NF_SIGNED);
}

static void
dt_cg_compare_op(dt_node_t *dnp, dt_irlist_t *dlp, dt_regset_t *drp, uint_t op)
{
	uint_t lbl_true = dt_irlist_label(dlp);
	uint_t lbl_post = dt_irlist_label(dlp);

	struct bpf_insn instr;

	dt_cg_node(dnp->dn_left, dlp, drp);
	dt_cg_node(dnp->dn_right, dlp, drp);

	if (dt_node_is_string(dnp->dn_left) || dt_node_is_string(dnp->dn_right)) {
		int reg;

		/* XXX turn into inlined loop. */
		reg = dt_cg_call(dlp, drp, BPF_FUNC_dtrace_strcmp,
		    dnp->dn_left->dn_reg, dnp->dn_right->dn_reg);
		dt_cg_mov(dlp, dst->dn_left->dn_reg, reg);

	} else {
		instr = BPF_ALU64_REG(BPF_SUB, dnp->dn_left->dn_reg,
		    dnp->dn_right->dn_reg);
		dt_irlist_append(dlp, dt_cg_node_alloc(instr));
	}
	dt_regset_free(drp, dnp->dn_right->dn_reg);
	dnp->dn_reg = dnp->dn_left->dn_reg;

	instr = BPF_JMP_IMM(op, dnp->dn_reg, 0, lbl_true);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));

	dt_cg_setx(dlp, dnp->dn_reg, 0);

	instr = BPF_JMP_IMM(BPF_JA, 0, 0, lbl_post);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));

	dt_cg_xsetx(dlp, NULL, lbl_true, dnp->dn_reg, 1);
	dt_irlist_append(dlp, dt_cg_node_alloc_labelled(lbl_post, BPF_NOP));
}

/*
 * Code generation for the ternary op requires some trickery with the assembler
 * in order to conserve registers.  We generate code for dn_expr and dn_left
 * and free their registers so they do not have be consumed across codegen for
 * dn_right.  We insert a dummy MOV at the end of dn_left into the destination
 * register, which is not yet known because we haven't done dn_right yet, and
 * save the pointer to this instruction node.  We then generate code for
 * dn_right and use its register as our output.  Finally, we reach back and
 * patch the instruction for dn_left to move its output into this register.
 */
static void
dt_cg_ternary_op(dt_node_t *dnp, dt_irlist_t *dlp, dt_regset_t *drp)
{
	uint_t lbl_false = dt_irlist_label(dlp);
	uint_t lbl_post = dt_irlist_label(dlp);

	dif_instr_t instr;
	dt_irnode_t *dip;

	dt_cg_node(dnp->dn_expr, dlp, drp);
	instr = BPF_JMP_IMM(BPF_JE, dnp->dn_expr->dn_reg, 0, lbl_false);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));
	dt_regset_free(drp, dnp->dn_expr->dn_reg);

	dt_cg_node(dnp->dn_left, dlp, drp);
	instr = BPF_MOV64_REG(dnp->dn_left->dn_reg, DIF_REG_R0);
	dip = dt_cg_node_alloc(instr); /* save dip for below */
	dt_irlist_append(dlp, dip);
	dt_regset_free(drp, dnp->dn_left->dn_reg);

	instr = BPF_JMP_IMM(BPF_JA, 0, 0, lbl_post);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));

	dt_irlist_append(dlp, dt_cg_node_alloc_labelled(lbl_false, BPF_NOP));
	dt_cg_node(dnp->dn_right, dlp, drp);
	dnp->dn_reg = dnp->dn_right->dn_reg;

	/*
	 * Now that dn_reg is assigned, reach back and patch the correct MOV
	 * instruction into the tail of dn_left.  We know dn_reg was unused
	 * at that point because otherwise dn_right couldn't have allocated it.
	 */
	dip->di_instr = BPF_MOV64_REG(dnp->dn_left->dn_reg, dnp->dn_reg);
	dt_irlist_append(dlp, dt_cg_node_alloc_labelled(lbl_post, BPF_NOP));
}

static void
dt_cg_logical_and(dt_node_t *dnp, dt_irlist_t *dlp, dt_regset_t *drp)
{
	uint_t lbl_false = dt_irlist_label(dlp);
	uint_t lbl_post = dt_irlist_label(dlp);

	struct bpf_insn instr;

	dt_cg_node(dnp->dn_left, dlp, drp);
	instr = BPF_JMP_IMM(BPF_JE, dnp->dn_left->dn_reg, 0, lbl_false);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));
	dt_regset_free(drp, dnp->dn_left->dn_reg);

	dt_cg_node(dnp->dn_right, dlp, drp);
	instr = BPF_JMP_IMM(BPF_JE, dnp->dn_right->dn_reg, 0, lbl_false);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));
	dnp->dn_reg = dnp->dn_right->dn_reg;

	dt_cg_setx(dlp, dnp->dn_reg, 1);

	instr = BPF_JMP_IMM(BPF_JA, 0, 0, lbl_post);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));

	dt_cg_xsetx(dlp, NULL, lbl_false, dnp->dn_reg, 0);
	dt_irlist_append(dlp, dt_cg_node_alloc_labelled(lbl_post, BPF_NOP));
}

static void
dt_cg_logical_xor(dt_node_t *dnp, dt_irlist_t *dlp, dt_regset_t *drp)
{
	uint_t lbl_next = dt_irlist_label(dlp);
	uint_t lbl_tail = dt_irlist_label(dlp);

	struct bpf_insn instr;

	dt_cg_node(dnp->dn_left, dlp, drp);

	instr = BPF_JMP_IMM(BPF_JE, dnp->dn_left->dn_reg, 0, lbl_next);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));
	dt_cg_setx(dlp, dnp->dn_left->dn_reg, 1);

	dt_irlist_append(dlp, dt_cg_node_alloc_labelled(lbl_next, BPF_NOP));
	dt_cg_node(dnp->dn_right, dlp, drp);

	instr = BPF_JMP_IMM(BPF_JE, dnp->dn_right->dn_reg, 0, lbl_tail);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));
	dt_cg_setx(dlp, dnp->dn_right->dn_reg, 1);

	instr = BPF_ALU64_REG(BPF_XOR, dnp->dn_left->dn_reg,
	    dnp->dn_right->dn_reg);

	dt_irlist_append(dlp, dt_cg_node_alloc_labelled(lbl_tail, instr));

	dt_regset_free(drp, dnp->dn_right->dn_reg);
	dnp->dn_reg = dnp->dn_left->dn_reg;
}

static void
dt_cg_logical_or(dt_node_t *dnp, dt_irlist_t *dlp, dt_regset_t *drp)
{
	uint_t lbl_true = dt_irlist_label(dlp);
	uint_t lbl_false = dt_irlist_label(dlp);
	uint_t lbl_post = dt_irlist_label(dlp);

	struct bpf_insn instr;

	dt_cg_node(dnp->dn_left, dlp, drp);
	instr = BPF_JMP_IMM(BPF_JNE, dnp->dn_left->dn_reg, 0, lbl_true);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));
	dt_regset_free(drp, dnp->dn_left->dn_reg);

	dt_cg_node(dnp->dn_right, dlp, drp);
	instr = BPF_JMP_IMM(BPF_JE, dnp->dn_right->dn_reg, 0, lbl_false);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));
	dnp->dn_reg = dnp->dn_right->dn_reg;

	dt_cg_xsetx(dlp, NULL, lbl_true, dnp->dn_reg, 1);

	instr = BPF_JMP_IMM(BPF_JA, 0, 0, lbl_post);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));

	dt_cg_xsetx(dlp, NULL, lbl_false, dnp->dn_reg, 0);

	dt_irlist_append(dlp, dt_cg_node_alloc_labelled(lbl_post, BPF_NOP));
}

static void
dt_cg_logical_neg(dt_node_t *dnp, dt_irlist_t *dlp, dt_regset_t *drp)
{
	uint_t lbl_zero = dt_irlist_label(dlp);
	uint_t lbl_post = dt_irlist_label(dlp);

	struct bpf_insn instr;

	dt_cg_node(dnp->dn_child, dlp, drp);
	dnp->dn_reg = dnp->dn_child->dn_reg;

	instr = BPF_JMP_IMM(BPF_JEQ, dnp->dn_reg, 0, lbl_zero);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));

	dt_cg_setx(dlp, dnp->dn_reg, 0);

	instr = BPF_JMP_IMM(BPF_JA, 0, 0, lbl_post);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));

	dt_cg_xsetx(dlp, NULL, lbl_zero, dnp->dn_reg, 1);
	dt_irlist_append(dlp, dt_cg_node_alloc_labelled(lbl_post, BPF_NOP));
}

static void
dt_cg_asgn_op(dt_node_t *dnp, dt_irlist_t *dlp, dt_regset_t *drp)
{
	struct bpf_insn instr;
	dt_ident_t *idp;

	/*
	 * If we are performing a structure assignment of a translated type,
	 * we must instantiate all members and create a snapshot of the object
	 * in scratch space.  We alloc a chunk of memory, generate code for
	 * each member, and then set dnp->dn_reg to the scratch object address.
	 */
	if ((idp = dt_node_resolve(dnp->dn_right, DT_IDENT_XLSOU)) != NULL) {
		ctf_membinfo_t ctm;
		dt_xlator_t *dxp = idp->di_data;
		dt_node_t *mnp, dn, mn;
		int r1, ret;

		/*
		 * Create two fake dt_node_t's representing operator "." and a
		 * right-hand identifier child node.  These will be repeatedly
		 * modified according to each instantiated member so that we
		 * can pass them to dt_cg_store() and effect a member store.
		 */
		bzero(&dn, sizeof (dt_node_t));
		dn.dn_kind = DT_NODE_OP2;
		dn.dn_op = DT_TOK_DOT;
		dn.dn_left = dnp;
		dn.dn_right = &mn;

		bzero(&mn, sizeof (dt_node_t));
		mn.dn_kind = DT_NODE_IDENT;
		mn.dn_op = DT_TOK_IDENT;

		/*
		 * Allocate a register for our scratch data pointer.  First we
		 * set it to the size of our data structure, and then replace
		 * it with the result of an allocs of the specified size.
		 */
		if ((r1 = dt_regset_alloc(drp)) == -1)
			longjmp(yypcb->pcb_jmpbuf, EDT_NOREG);

		dt_cg_setx(dlp, r1,
		    ctf_type_size(dxp->dx_dst_ctfp, dxp->dx_dst_base));

		/*
		 * XXX what about failure? Guess it can't fail without ERROR.
		 * (The DIF_OP_ALLOCS code suggests otherwise!)
		 */
		ret = dt_cg_call(dlp, drp, BPF_FUNC_dtrace_alloc_scratch, r1);
		dt_cg_mov(dlp, r1, ret);

		/*
		 * When dt_cg_asgn_op() is called, we have already generated
		 * code for dnp->dn_right, which is the translator input.  We
		 * now associate this register with the translator's input
		 * identifier so it can be referenced during our member loop.
		 */
		dxp->dx_ident->di_flags |= DT_IDFLG_CGREG;
		dxp->dx_ident->di_id = dnp->dn_right->dn_reg;

		for (mnp = dxp->dx_members; mnp != NULL; mnp = mnp->dn_list) {
			/*
			 * Generate code for the translator member expression,
			 * and then cast the result to the member type.
			 */
			dt_cg_node(mnp->dn_membexpr, dlp, drp);
			mnp->dn_reg = mnp->dn_membexpr->dn_reg;
			dt_cg_typecast(mnp->dn_membexpr, mnp, dlp, drp);

			/*
			 * Ask CTF for the offset of the member so we can store
			 * to the appropriate offset.  This call has already
			 * been done once by the parser, so it should succeed.
			 */
			if (ctf_member_info(dxp->dx_dst_ctfp, dxp->dx_dst_base,
			    mnp->dn_membname, &ctm) == CTF_ERR) {
				yypcb->pcb_hdl->dt_ctferr =
				    ctf_errno(dxp->dx_dst_ctfp);
				longjmp(yypcb->pcb_jmpbuf, EDT_CTF);
			}

			/*
			 * Store the result to r1, possibly taking the offset
			 * into account (optimize by not even generating code to
			 * add the offset if it is zero).
			 *
			 * Round the offset down to the nearest byte.  If the
			 * offset was not aligned on a byte boundary, this
			 * member is a bit-field and dt_cg_store() will handle
			 * masking.
			 */
			if (ctm.ctm_offset != 0) {
				instr = BPF_ALU64_IMM(BPF_ADD, r1,
				    ctm.ctm_offset / NBBY);
				dt_irlist_append(dlp, dt_cg_node_alloc(instr));
			}
			dt_node_type_propagate(mnp, &dn);
			dn.dn_right->dn_string = mnp->dn_membname;
			dn.dn_reg = r1;

			dt_cg_store(mnp, dlp, drp, &dn);
			dt_regset_free(drp, mnp->dn_reg);
		}

		dxp->dx_ident->di_flags &= ~DT_IDFLG_CGREG;
		dxp->dx_ident->di_id = 0;

		if (dnp->dn_right->dn_reg != -1)
			dt_regset_free(drp, dnp->dn_right->dn_reg);

		assert(dnp->dn_reg == dnp->dn_right->dn_reg);
		dnp->dn_reg = r1;
	}

	/*
	 * If we are storing to a variable, generate an stv instruction from
	 * the variable specified by the identifier.  If we are storing to a
	 * memory address, generate code again for the left-hand side using
	 * DT_NF_REF to get the address, and then generate a store to it.
	 * In both paths, we assume dnp->dn_reg already has the new value.
	 */
	if (dnp->dn_left->dn_kind == DT_NODE_VAR) {
		int is_thread = (idp->di_flags & DT_IDFLG_TLS);

		idp = dt_ident_resolve(dnp->dn_left->dn_ident);
		idp->di_flags |= DT_IDFLG_DIFW;

		if (idp->di_kind == DT_IDENT_ARRAY) {
			int prev_depth = yypcb->pcb_stackdepth;
			int argcount;

			argcount = dt_cg_arglist(idp, dnp->dn_left->dn_args,
			    dlp, drp);

			dt_cg_call(dlp, drp, dt_cg_stvar(idp), idp->di_id,
			    dnp->dn_reg, 0, argcount);
			yypcb->pcb_stackdepth = prev_depth;
		} else /* non-associative */
			dt_cg_call(dlp, drp, dt_cg_stvar(idp), idp->di_id,
			    dnp->dn_reg);
	} else {
		uint_t rbit = dnp->dn_left->dn_flags & DT_NF_REF;

		assert(dnp->dn_left->dn_flags & DT_NF_WRITABLE);
		assert(dnp->dn_left->dn_flags & DT_NF_LVALUE);

		dnp->dn_left->dn_flags |= DT_NF_REF; /* force pass-by-ref */

		dt_cg_node(dnp->dn_left, dlp, drp);
		dt_cg_store(dnp, dlp, drp, dnp->dn_left);
		dt_regset_free(drp, dnp->dn_left->dn_reg);

		dnp->dn_left->dn_flags &= ~DT_NF_REF;
		dnp->dn_left->dn_flags |= rbit;
	}
}

static void
dt_cg_assoc_op(dt_node_t *dnp, dt_irlist_t *dlp, dt_regset_t *drp)
{
	struct bpf_insn instr;
	int prev_depth = yypcb->pcb_stackdepth;
	uint_t op;
	int ret;
	int argcount;

	assert(dnp->dn_kind == DT_NODE_VAR);
	assert(!(dnp->dn_ident->di_flags & DT_IDFLG_LOCAL));
	assert(dnp->dn_args != NULL);

	argcount = dt_cg_arglist(dnp->dn_ident, dnp->dn_args, dlp, drp);

	if ((dnp->dn_reg = dt_regset_alloc(drp)) == -1)
		longjmp(yypcb->pcb_jmpbuf, EDT_NOREG);

	if (dnp->dn_ident->di_flags & DT_IDFLG_TLS)
		op = BPF_FUNC_dtrace_get_thread_assoc;
	else
		op = BPF_FUNC_dtrace_get_global_assoc;

	dnp->dn_ident->di_flags |= DT_IDFLG_DIFR;
	ret = dt_cg_call(dlp, drp, op, idp->di_id, 0, argcount);
	dt_cg_mov(dlp, dnp->dn_reg, ret);

	/*
	 * If the associative array is a pass-by-reference type, then we are
	 * loading its value as a pointer to either load or store through it.
	 * The array element in question may not have been faulted in yet, in
	 * which case DIF_OP_LD*AA will return zero.  We append an epilogue
	 * of instructions similar to the following:
	 *
	 *XXX revise comment
	 *
	 *	  ld?aa	 id, %r1	! base ld?aa instruction above
	 *	  tst	 %r1		! start of epilogue
	 *   +--- bne	 label
	 *   |    setx	 size, %r1
	 *   |    allocs %r1, %r1
	 *   |    st?aa	 id, %r1
	 *   |    ld?aa	 id, %r1
	 *   v
	 * label: < rest of code >
	 *
	 * The idea is that we allocs a zero-filled chunk of scratch space and
	 * do a DIF_OP_ST*AA to fault in and initialize the array element, and
	 * then reload it to get the faulted-in address of the new variable
	 * storage.  This isn't cheap, but pass-by-ref associative array values
	 * are (thus far) uncommon and the allocs cost only occurs once.  If
	 * this path becomes important to DTrace users, we can improve things
	 * by adding a new DIF opcode to fault in associative array elements.
	 */
	if (dnp->dn_flags & DT_NF_REF) {
		uint_t stvop = op == BPF_FUNC_dtrace_get_thread_assoc ?
		    BPF_FUNC_dtrace_set_thread_assoc :
		    BPF_FUNC_dtrace_set_global_assoc;
		uint_t label = dt_irlist_label(dlp);
		int ret;

		instr = BPF_JMP_IMM(BPF_JE, dnp->dn_reg, 0, label);
		dt_irlist_append(dlp, dt_cg_node_alloc(instr));

		dt_cg_setx(dlp, dnp->dn_reg, dt_node_type_size(dnp));
		ret = dt_cg_call(dlp, drp, BPF_FUNC_dtrace_alloc_scratch,
		    dnp->dn_reg);
		dt_cg_mov(dlp, dnp->dn_reg, ret);

		dnp->dn_ident->di_flags |= DT_IDFLG_DIFW;
		dt_cg_call(dlp, drp, stvop, dnp->dn_ident->di_id, dnp->dn_reg);

		ret = dt_cg_call(dlp, drp, op, idp->dn_ident->di_id);
		dt_cg_mov(dlp, dnp->dn_reg, ret);

		dt_irlist_append(dlp, dt_cg_node_alloc_labelled(label, BPF_NOP));
	}
	yypcb->pcb_stackdepth = prev_depth;
}

static void
dt_cg_array_op(dt_node_t *dnp, dt_irlist_t *dlp, dt_regset_t *drp)
{
	dt_probe_t *prp = yypcb->pcb_probe;
	uintmax_t saved = dnp->dn_args->dn_value;
	dt_ident_t *idp = dnp->dn_ident;

	struct bpf_insn instr;
	uint_t op;
	size_t size;
	int reg, n, ret;

	assert(dnp->dn_kind == DT_NODE_VAR);
	assert(!(idp->di_flags & DT_IDFLG_LOCAL));

	assert(dnp->dn_args->dn_kind == DT_NODE_INT);
	assert(dnp->dn_args->dn_list == NULL);

	/*
	 * If this is a reference in the args[] array, temporarily modify the
	 * array index according to the static argument mapping (if any),
	 * unless the argument reference is provided by a dynamic translator.
	 * If we're using a dynamic translator for args[], then just set dn_reg
	 * to an invalid reg and return: DIF_OP_XLARG will fetch the arg later.
	 *
	 * TODO dynamic translators, either implement or remove.
	 */
	if (idp->di_id == DIF_VAR_ARGS) {
		if ((idp->di_kind == DT_IDENT_XLPTR ||
		    idp->di_kind == DT_IDENT_XLSOU) &&
		    dt_xlator_dynamic(idp->di_data)) {
			dnp->dn_reg = -1;
			return;
		}
		dnp->dn_args->dn_value = prp->pr_mapping[saved];
	}

	dt_cg_node(dnp->dn_args, dlp, drp);
	dnp->dn_args->dn_value = saved;

	dnp->dn_reg = dnp->dn_args->dn_reg;

	if (idp->di_flags & DT_IDFLG_TLS)
		op = BPF_FUNC_dtrace_get_thread_array; /* not implemented! */
	else
		op = BPF_FUNC_dtrace_get_global_array;

	idp->di_flags |= DT_IDFLG_DIFR;

	ret = dt_cg_call(dlp, drp, op, idp->di_id, dnp->dn_args->dn_reg);
	dt_cg_mov(dlp, dnp->dn_reg, ret);

	/*
	 * If this is a reference to the args[] array, we need to take the
	 * additional step of explicitly eliminating any bits larger than the
	 * type size: the BPF interpreter in the kernel will always give us
	 * the raw (64-bit) argument value, and any bits larger than the type
	 * size may be junk.  As a practical matter, this arises only on 64-bit
	 * architectures and only when the argument index is larger than the
	 * number of arguments passed directly to DTrace: if a 8-, 16- or
	 * 32-bit argument must be retrieved from the stack, it is possible
	 * (and it some cases, likely) that the upper bits will be garbage.
	 */
	if (idp->di_id != DIF_VAR_ARGS || !dt_node_is_scalar(dnp))
		return;

	if ((size = dt_node_type_size(dnp)) == sizeof (uint64_t))
		return;

	assert(size < sizeof (uint64_t));
	n = sizeof (uint64_t) * NBBY - size * NBBY;

	instr = BPF_ALU64_IMM(BPF_LSH, dnp->dn_reg, n);
	dt_irlist_append(dlp, dt_cg_node_alloc(instr));

	instr = BPF_ALU64_IMM((dnp->dn_flags & DT_NF_SIGNED) ?
	    BPF_ARSH : BPF_RSH, dnp->dn_reg, n);

	dt_irlist_append(dlp, dt_cg_node_alloc(instr));
}

/*
 * Generate code for an inlined variable reference.  Inlines can be used to
 * define either scalar or associative array substitutions.  For scalars, we
 * simply generate code for the parse tree saved in the identifier's din_root,
 * and then cast the resulting expression to the inline's declaration type.
 * For arrays, we take the input parameter subtrees from dnp->dn_args and
 * temporarily store them in the din_root of each din_argv[i] identifier,
 * which are themselves inlines and were set up for us by the parser.  The
 * result is that any reference to the inlined parameter inside the top-level
 * din_root will turn into a recursive call to dt_cg_inline() for a scalar
 * inline whose din_root will refer to the subtree pointed to by the argument.
 */
static void
dt_cg_inline(dt_node_t *dnp, dt_irlist_t *dlp, dt_regset_t *drp)
{
	dt_ident_t *idp = dnp->dn_ident;
	dt_idnode_t *inp = idp->di_iarg;

	dt_idnode_t *pinp;
	dt_node_t *pnp;
	int i;

	assert(idp->di_flags & DT_IDFLG_INLINE);
	assert(idp->di_ops == &dt_idops_inline);

	if (idp->di_kind == DT_IDENT_ARRAY) {
		for (i = 0, pnp = dnp->dn_args;
		    pnp != NULL; pnp = pnp->dn_list, i++) {
			if (inp->din_argv[i] != NULL) {
				pinp = inp->din_argv[i]->di_iarg;
				pinp->din_root = pnp;
			}
		}
	}

	dt_cg_node(inp->din_root, dlp, drp);
	dnp->dn_reg = inp->din_root->dn_reg;
	dt_cg_typecast(inp->din_root, dnp, dlp, drp);

	if (idp->di_kind == DT_IDENT_ARRAY) {
		for (i = 0; i < inp->din_argc; i++) {
			pinp = inp->din_argv[i]->di_iarg;
			pinp->din_root = NULL;
		}
	}
}

static void
dt_cg_node(dt_node_t *dnp, dt_irlist_t *dlp, dt_regset_t *drp)
{
	ctf_file_t *ctfp = dnp->dn_ctfp;
	ctf_file_t *octfp;
	ctf_membinfo_t m;
	ctf_id_t type;

	struct bpf_insn instr;
	dt_ident_t *idp;
	ssize_t stroff;
	uint_t op;
	int reg;

	switch (dnp->dn_op) {
	case DT_TOK_COMMA:
		dt_cg_node(dnp->dn_left, dlp, drp);
		dt_regset_free(drp, dnp->dn_left->dn_reg);
		dt_cg_node(dnp->dn_right, dlp, drp);
		dnp->dn_reg = dnp->dn_right->dn_reg;
		break;

	case DT_TOK_ASGN:
		dt_cg_node(dnp->dn_right, dlp, drp);
		dnp->dn_reg = dnp->dn_right->dn_reg;
		dt_cg_asgn_op(dnp, dlp, drp);
		break;

	case DT_TOK_ADD_EQ:
		dt_cg_arithmetic_op(dnp, dlp, drp, BPF_ADD);
		dt_cg_asgn_op(dnp, dlp, drp);
		break;

	case DT_TOK_SUB_EQ:
		dt_cg_arithmetic_op(dnp, dlp, drp, BPF_SUB);
		dt_cg_asgn_op(dnp, dlp, drp);
		break;

	case DT_TOK_MUL_EQ:
		dt_cg_arithmetic_op(dnp, dlp, drp, BPF_MUL);
		dt_cg_asgn_op(dnp, dlp, drp);
		break;

	case DT_TOK_DIV_EQ:
		/* XXX signedness */
		dt_cg_arithmetic_op(dnp, dlp, drp,
		    (dnp->dn_flags & DT_NF_SIGNED) ? BPF_DIV : BPF_DIV);
		dt_cg_asgn_op(dnp, dlp, drp);
		break;

	case DT_TOK_MOD_EQ:
		/* XXX signedness */
		dt_cg_arithmetic_op(dnp, dlp, drp,
		    (dnp->dn_flags & DT_NF_SIGNED) ? BPF_MOD : BPF_MOD);
		dt_cg_asgn_op(dnp, dlp, drp);
		break;

	case DT_TOK_AND_EQ:
		dt_cg_arithmetic_op(dnp, dlp, drp, BPF_AND);
		dt_cg_asgn_op(dnp, dlp, drp);
		break;

	case DT_TOK_XOR_EQ:
		dt_cg_arithmetic_op(dnp, dlp, drp, BPF_XOR);
		dt_cg_asgn_op(dnp, dlp, drp);
		break;

	case DT_TOK_OR_EQ:
		dt_cg_arithmetic_op(dnp, dlp, drp, BPF_OR);
		dt_cg_asgn_op(dnp, dlp, drp);
		break;

	case DT_TOK_LSH_EQ:
		dt_cg_arithmetic_op(dnp, dlp, drp, BPF_LSH);
		dt_cg_asgn_op(dnp, dlp, drp);
		break;

	case DT_TOK_RSH_EQ:
		dt_cg_arithmetic_op(dnp, dlp, drp,
		    (dnp->dn_flags & DT_NF_SIGNED) ? BPF_ARSH : BPF_RSH);
		dt_cg_asgn_op(dnp, dlp, drp);
		break;

	case DT_TOK_QUESTION:
		dt_cg_ternary_op(dnp, dlp, drp);
		break;

	case DT_TOK_LOR:
		dt_cg_logical_or(dnp, dlp, drp);
		break;

	case DT_TOK_LXOR:
		dt_cg_logical_xor(dnp, dlp, drp);
		break;

	case DT_TOK_LAND:
		dt_cg_logical_and(dnp, dlp, drp);
		break;

	case DT_TOK_BOR:
		dt_cg_arithmetic_op(dnp, dlp, drp, BPF_OR);
		break;

	case DT_TOK_XOR:
		dt_cg_arithmetic_op(dnp, dlp, drp, BPF_XOR);
		break;

	case DT_TOK_BAND:
		dt_cg_arithmetic_op(dnp, dlp, drp, BPF_AND);
		break;

	case DT_TOK_EQU:
		dt_cg_compare_op(dnp, dlp, drp, BPF_JEQ);
		break;

	case DT_TOK_NEQ:
		dt_cg_compare_op(dnp, dlp, drp, BPF_JNE);
		break;

	case DT_TOK_LT:
		dt_cg_compare_op(dnp, dlp, drp,
		    dt_cg_compare_signed(dnp) ? BPF_OP_JSLT : BPF_OP_JLT);
		break;

	case DT_TOK_LE:
		dt_cg_compare_op(dnp, dlp, drp,
		    dt_cg_compare_signed(dnp) ? BPF_OP_JSLE : BPF_OP_JLE);
		break;

	case DT_TOK_GT:
		dt_cg_compare_op(dnp, dlp, drp,
		    dt_cg_compare_signed(dnp) ? BPF_OP_JSGT : BPF_OP_JGT);
		break;

	case DT_TOK_GE:
		dt_cg_compare_op(dnp, dlp, drp,
		    dt_cg_compare_signed(dnp) ? BPF_OP_JSGE : BPF_OP_JGE);
		break;

	case DT_TOK_LSH:
		dt_cg_arithmetic_op(dnp, dlp, drp, BPF_OP_LSH);
		break;

	case DT_TOK_RSH:
		dt_cg_arithmetic_op(dnp, dlp, drp,
		    (dnp->dn_flags & DT_NF_SIGNED) ? BPF_ARSH : BPF_RSH);
		break;

	case DT_TOK_ADD:
		dt_cg_arithmetic_op(dnp, dlp, drp, BPF_ADD);
		break;

	case DT_TOK_SUB:
		dt_cg_arithmetic_op(dnp, dlp, drp, BPF_SUB);
		break;

	case DT_TOK_MUL:
		dt_cg_arithmetic_op(dnp, dlp, drp, BPF_MUL);
		break;

	case DT_TOK_DIV:
		/* XXX signedness */
		dt_cg_arithmetic_op(dnp, dlp, drp,
		    (dnp->dn_flags & DT_NF_SIGNED) ? BPF_DIV : BPF_DIV);
		break;

	case DT_TOK_MOD:
		/* XXX signedness */
		dt_cg_arithmetic_op(dnp, dlp, drp,
		    (dnp->dn_flags & DT_NF_SIGNED) ? BPF_MOD : BPF_MOD);
		break;

	case DT_TOK_LNEG:
		dt_cg_logical_neg(dnp, dlp, drp);
		break;

	case DT_TOK_BNEG:
		dt_cg_node(dnp->dn_child, dlp, drp);
		dnp->dn_reg = dnp->dn_child->dn_reg;
		instr = BPF_ALU64_REG(BPF_NEG, dnp->dn_reg, dnp->dn_reg);
		dt_irlist_append(dlp, dt_cg_node_alloc(instr));
		break;

	case DT_TOK_PREINC:
		dt_cg_prearith_op(dnp, dlp, drp, BPF_ADD);
		break;

	case DT_TOK_POSTINC:
		dt_cg_postarith_op(dnp, dlp, drp, BPF_ADD);
		break;

	case DT_TOK_PREDEC:
		dt_cg_prearith_op(dnp, dlp, drp, BPF_SUB);
		break;

	case DT_TOK_POSTDEC:
		dt_cg_postarith_op(dnp, dlp, drp, BPF_SUB);
		break;

	case DT_TOK_IPOS:
		dt_cg_node(dnp->dn_child, dlp, drp);
		dnp->dn_reg = dnp->dn_child->dn_reg;
		break;

	case DT_TOK_INEG: {
		int r1;

		if ((r1 = dt_regset_alloc(drp)) == -1)
			longjmp(yypcb->pcb_jmpbuf, EDT_NOREG);

		dt_cg_node(dnp->dn_child, dlp, drp);
		dnp->dn_reg = dnp->dn_child->dn_reg;

		dt_cg_mov(dlp, r1, dnp->dn_reg);
		dt_cg_setx(dlp, dnp->dn_reg, 0);

		instr = BPF_ALU64_REG(BPF_SUB, dnp->dn_reg, r1);
		dt_irlist_append(dlp, dt_cg_node_alloc(instr));

		dt_regset_free(drp, r1);

		break;
	}
	case DT_TOK_DEREF:
		dt_cg_node(dnp->dn_child, dlp, drp);
		dnp->dn_reg = dnp->dn_child->dn_reg;

		if (!(dnp->dn_flags & DT_NF_REF)) {
			uint_t ubit = dnp->dn_flags & DT_NF_USERLAND;

			/*
			 * Save and restore DT_NF_USERLAND across dt_cg_load():
			 * we need the sign bit from dnp and the user bit from
			 * dnp->dn_child in order to get the proper opcode.
			 *
			 * XXX signedness
			 */
			dnp->dn_flags |=
			    (dnp->dn_child->dn_flags & DT_NF_USERLAND);

			instr = BPF_LDX_MEM(dt_cg_load(dnp, ctfp,
				dnp->dn_type), dnp->dn_reg, dnp->dn_reg, 0);

			dnp->dn_flags &= ~DT_NF_USERLAND;
			dnp->dn_flags |= ubit;

			dt_irlist_append(dlp, dt_cg_node_alloc(instr));
		}
		break;

	case DT_TOK_ADDROF: {
		uint_t rbit = dnp->dn_child->dn_flags & DT_NF_REF;

		dnp->dn_child->dn_flags |= DT_NF_REF; /* force pass-by-ref */
		dt_cg_node(dnp->dn_child, dlp, drp);
		dnp->dn_reg = dnp->dn_child->dn_reg;

		dnp->dn_child->dn_flags &= ~DT_NF_REF;
		dnp->dn_child->dn_flags |= rbit;
		break;
	}

	case DT_TOK_SIZEOF: {
		size_t size = dt_node_sizeof(dnp->dn_child);

		if ((dnp->dn_reg = dt_regset_alloc(drp)) == -1)
			longjmp(yypcb->pcb_jmpbuf, EDT_NOREG);

		assert(size != 0);
		dt_cg_setx(dlp, dnp->dn_reg, size);
		break;
	}

	case DT_TOK_STRINGOF:
		dt_cg_node(dnp->dn_child, dlp, drp);
		dnp->dn_reg = dnp->dn_child->dn_reg;
		break;

	case DT_TOK_XLATE:
		/*
		 * An xlate operator appears in either an XLATOR, indicating a
		 * reference to a dynamic translator, or an OP2, indicating
		 * use of the xlate operator in the user's program.  For the
		 * dynamic case, generate an xlate opcode with a reference to
		 * the corresponding member, pre-computed for us in dn_members.
		 *
		 * TODO dynamic translators, either implement or remove.
		 */
		if (dnp->dn_kind == DT_NODE_XLATOR) {
			/* NOT IMPLEMENTED. */
			dt_xlator_t *dxp = dnp->dn_xlator;

			assert(dxp->dx_ident->di_flags & DT_IDFLG_CGREG);
			assert(dxp->dx_ident->di_id != 0);

			if ((dnp->dn_reg = dt_regset_alloc(drp)) == -1)
				longjmp(yypcb->pcb_jmpbuf, EDT_NOREG);

			if (dxp->dx_arg == -1) {
				dt_irlist_append(dlp, dt_cg_node_alloc(BPF_NOP));
				/*
				 * Significant code removal here: if reviving
				 * dynamic xlators, see the pre-BPF codebase.
				 **/
			}
			break;
		}

		assert(dnp->dn_kind == DT_NODE_OP2);
		dt_cg_node(dnp->dn_right, dlp, drp);
		dnp->dn_reg = dnp->dn_right->dn_reg;
		break;

	case DT_TOK_LPAR:
		dt_cg_node(dnp->dn_right, dlp, drp);
		dnp->dn_reg = dnp->dn_right->dn_reg;
		dt_cg_typecast(dnp->dn_right, dnp, dlp, drp);
		break;

	case DT_TOK_PTR:
	case DT_TOK_DOT:
		assert(dnp->dn_right->dn_kind == DT_NODE_IDENT);
		dt_cg_node(dnp->dn_left, dlp, drp);

		/*
		 * If the left-hand side of PTR or DOT is a dynamic variable,
		 * we expect it to be the output of a D translator.   In this
		 * case, we look up the parse tree corresponding to the member
		 * that is being accessed and run the code generator over it.
		 * We then cast the result as if by the assignment operator.
		 *
		 * XXX underway
		 */
		if ((idp = dt_node_resolve(
		    dnp->dn_left, DT_IDENT_XLSOU)) != NULL ||
		    (idp = dt_node_resolve(
		    dnp->dn_left, DT_IDENT_XLPTR)) != NULL) {

			dt_xlator_t *dxp;
			dt_node_t *mnp;

			dxp = idp->di_data;
			mnp = dt_xlator_member(dxp, dnp->dn_right->dn_string);
			assert(mnp != NULL);

			dxp->dx_ident->di_flags |= DT_IDFLG_CGREG;
			dxp->dx_ident->di_id = dnp->dn_left->dn_reg;

			dt_cg_node(mnp->dn_membexpr, dlp, drp);
			dnp->dn_reg = mnp->dn_membexpr->dn_reg;
			dt_cg_typecast(mnp->dn_membexpr, dnp, dlp, drp);

			dxp->dx_ident->di_flags &= ~DT_IDFLG_CGREG;
			dxp->dx_ident->di_id = 0;

			if (dnp->dn_left->dn_reg != -1)
				dt_regset_free(drp, dnp->dn_left->dn_reg);
			break;
		}

		ctfp = dnp->dn_left->dn_ctfp;
		type = ctf_type_resolve(ctfp, dnp->dn_left->dn_type);

		if (dnp->dn_op == DT_TOK_PTR) {
			type = ctf_type_reference(ctfp, type);
			type = ctf_type_resolve(ctfp, type);
		}

		if ((ctfp = dt_cg_membinfo(octfp = ctfp, type,
		    dnp->dn_right->dn_string, &m)) == NULL) {
			yypcb->pcb_hdl->dt_ctferr = ctf_errno(octfp);
			longjmp(yypcb->pcb_jmpbuf, EDT_CTF);
		}

		/*
		 * If the offset is not aligned on a byte boundary, it is a
		 * bit-field member and we will extract the value bits below
		 * after we generate the appropriate load.
		 */
		if (ctm.ctm_offset != 0) {
			instr = BPF_ALU64_IMM(BPF_ADD, dnp->dn_left->dn_reg,
			    ctm.ctm_offset / NBBY);
			dt_irlist_append(dlp, dt_cg_node_alloc(instr));
		}

		if (!(dnp->dn_flags & DT_NF_REF)) {
			uint_t ubit = dnp->dn_flags & DT_NF_USERLAND;

			/*
			 * Save and restore DT_NF_USERLAND across dt_cg_load():
			 * we need the sign bit from dnp and the user bit from
			 * dnp->dn_left in order to get the proper opcode.  (Or,
			 * in the BPF world, to fail hard, since userland
			 * BPF support is not yet designed in DTrace.)
			 */
			dnp->dn_flags |=
			    (dnp->dn_left->dn_flags & DT_NF_USERLAND);

			instr = BPF_LDX_MEM(dt_cg_load(dnp, ctfp,
				m.ctm_type), dnp->dn_left->dn_reg,
			    dnp->dn_left->dn_reg, 0);

			dnp->dn_flags &= ~DT_NF_USERLAND;
			dnp->dn_flags |= ubit;

			dt_irlist_append(dlp, dt_cg_node_alloc(instr));

			if (dnp->dn_flags & DT_NF_BITFIELD)
				dt_cg_field_get(dnp, dlp, drp, ctfp, &m);
		}

		dnp->dn_reg = dnp->dn_left->dn_reg;
		break;

	case DT_TOK_STRING: {
		int ret;

		if ((dnp->dn_reg = dt_regset_alloc(drp)) == -1)
			longjmp(yypcb->pcb_jmpbuf, EDT_NOREG);

		assert(dnp->dn_kind == DT_NODE_STRING);
		stroff = dt_strtab_insert(yypcb->pcb_strtab, dnp->dn_string);

		if (stroff == -1L)
			longjmp(yypcb->pcb_jmpbuf, EDT_NOMEM);
		if (stroff > DIF_STROFF_MAX)
			longjmp(yypcb->pcb_jmpbuf, EDT_STR2BIG);

		/* XXX turn into inlined loop */
		ret = dt_cg_call(dlp, drp, BPF_FUNC_dtrace_sets, (uint64_t) stroff);
		dt_cg_mov(dlp, dnp->dn_reg, ret);
		break;
	}
	case DT_TOK_IDENT:
		/*
		 * If the specified identifier is a variable on which we have
		 * set the code generator register flag, then this variable
		 * has already had code generated for it and saved in di_id.
		 * Allocate a new register and copy the existing value to it.
		 */
		if (dnp->dn_kind == DT_NODE_VAR &&
		    (dnp->dn_ident->di_flags & DT_IDFLG_CGREG)) {
			if ((dnp->dn_reg = dt_regset_alloc(drp)) == -1)
				longjmp(yypcb->pcb_jmpbuf, EDT_NOREG);
			dt_cg_setx(dlp, dnp->dn_reg, dnp->dn_ident->di_id);
			break;
		}

		/*
		 * Identifiers can represent function calls, variable refs, or
		 * symbols.  First we check for inlined variables, and handle
		 * them by generating code for the inline parse tree.
		 */
		if (dnp->dn_kind == DT_NODE_VAR &&
		    (dnp->dn_ident->di_flags & DT_IDFLG_INLINE)) {
			dt_cg_inline(dnp, dlp, drp);
			break;
		}

		switch (dnp->dn_kind) {
		case DT_NODE_FUNC: {
			int prev_depth = yypcb->pcb_stackdepth;
			int argcount;

			if ((idp = dnp->dn_ident)->di_kind != DT_IDENT_FUNC) {
				dnerror(dnp, D_CG_EXPR, "%s %s( ) may not be "
				    "called from a D expression (D program "
				    "context required)\n",
				    dt_idkind_name(idp->di_kind), idp->di_name);
			}

			argcount = dt_cg_arglist(dnp->dn_ident, dnp->dn_args,
			    dlp, drp);

			reg = dt_cg_call(dlp, drp, dnp,
			    BPF_FUNC_dtrace_subr, dnp->dn_ident->di_id, 0,
			    argcount);
			yypcb->pcb_stackdepth = prev_depth;
			break;
		}
		case DT_NODE_VAR:
			if (dnp->dn_ident->di_kind == DT_IDENT_XLSOU ||
			    dnp->dn_ident->di_kind == DT_IDENT_XLPTR) {
				/*
				 * This can only happen if we have translated
				 * args[].  See dt_idcook_args() for details.
				 */
				assert(dnp->dn_ident->di_id == DIF_VAR_ARGS);
				dt_cg_array_op(dnp, dlp, drp);
				break;
			}

			if (dnp->dn_ident->di_kind == DT_IDENT_ARRAY) {
				if (dnp->dn_ident->di_id > DIF_VAR_ARRAY_MAX)
					dt_cg_assoc_op(dnp, dlp, drp);
				else
					dt_cg_array_op(dnp, dlp, drp);
				break;
			}

			if ((dnp->dn_reg = dt_regset_alloc(drp)) == -1)
				longjmp(yypcb->pcb_jmpbuf, EDT_NOREG);

			if (dnp->dn_ident->di_flags & DT_IDFLG_LOCAL)
				op = BPF_FUNC_dtrace_get_local;
			else if (dnp->dn_ident->di_flags & DT_IDFLG_TLS)
				op = BPF_FUNC_dtrace_get_thread;
			else
				op = BPF_FUNC_dtrace_get_global;

			dnp->dn_ident->di_flags |= DT_IDFLG_DIFR;

			ret = dt_cg_call(dlp, drp, op, idp->di_id);
			dt_cg_mov(dlp, dnp->dn_reg, ret);
			break;

		case DT_NODE_SYM: {
			dtrace_hdl_t *dtp = yypcb->pcb_hdl;
			dtrace_syminfo_t *sip = dnp->dn_ident->di_data;
			GElf_Sym sym;

			if (dtrace_lookup_by_name(dtp,
			    sip->dts_object, sip->dts_name, &sym, NULL) == -1) {
				xyerror(D_UNKNOWN, "cg failed for symbol %s`%s:"
				    " %s\n", sip->dts_object, sip->dts_name,
				    dtrace_errmsg(dtp, dtrace_errno(dtp)));
			}

			if ((dnp->dn_reg = dt_regset_alloc(drp)) == -1)
				longjmp(yypcb->pcb_jmpbuf, EDT_NOREG);

			dt_cg_xsetx(dlp, dnp->dn_ident,
			    DT_LBL_NONE, dnp->dn_reg, sym.st_value);

			if (!(dnp->dn_flags & DT_NF_REF)) {
				instr = BPF_LDX_MEM(dt_cg_load(dnp, ctfp,
					dnp->dn_type), dnp->dn_reg, dnp->dn_reg,
					0);
				dt_irlist_append(dlp, dt_cg_node_alloc(instr));
			}
			break;
		}

		default:
			xyerror(D_UNKNOWN, "internal error -- node type %u is "
			    "not valid for an identifier\n", dnp->dn_kind);
		}
		break;

	case DT_TOK_INT:
		if ((dnp->dn_reg = dt_regset_alloc(drp)) == -1)
			longjmp(yypcb->pcb_jmpbuf, EDT_NOREG);

		dt_cg_setx(dlp, dnp->dn_reg, dnp->dn_value);
		break;

	default:
		xyerror(D_UNKNOWN, "internal error -- token type %u is not a "
		    "valid D compilation token\n", dnp->dn_op);
	}
}

void
dt_cg(dt_pcb_t *pcb, dt_node_t *dnp)
{
	dt_xlator_t *dxp = NULL;

	if (pcb->pcb_regs == NULL && (pcb->pcb_regs =
	    dt_regset_create(pcb->pcb_hdl->dt_conf.dtc_difnregs)) == NULL)
		longjmp(pcb->pcb_jmpbuf, EDT_NOMEM);

	dt_regset_reset(pcb->pcb_regs);

	if (pcb->pcb_strtab != NULL)
		dt_strtab_destroy(pcb->pcb_strtab);

	if ((pcb->pcb_strtab = dt_strtab_create(BUFSIZ)) == NULL)
		longjmp(pcb->pcb_jmpbuf, EDT_NOMEM);

	dt_irlist_destroy(&pcb->pcb_ir);
	dt_irlist_create(&pcb->pcb_ir);

	assert(pcb->pcb_dret == NULL);
	pcb->pcb_dret = dnp;

	if (dt_node_is_dynamic(dnp)) {
		dnerror(dnp, D_CG_DYN, "expression cannot evaluate to result "
		    "of dynamic type\n");
	}

	/*
	 * If we're generating code for a translator body, assign the input
	 * parameter to the first available register (i.e. caller passes %r1).
	 */
	if (dnp->dn_kind == DT_NODE_MEMBER) {
		dxp = dnp->dn_membxlator;
		dnp = dnp->dn_membexpr;

		dxp->dx_ident->di_flags |= DT_IDFLG_CGREG;
		dxp->dx_ident->di_id = dt_regset_alloc(pcb->pcb_regs);
	}

	dt_cg_node(dnp, &pcb->pcb_ir, pcb->pcb_regs);
	dt_regset_free(pcb->pcb_regs, dnp->dn_reg);
	dt_irlist_append(&pcb->pcb_ir, dt_cg_node_alloc(BPF_EXIT_INSN));

	if (dnp->dn_kind == DT_NODE_MEMBER) {
		dt_regset_free(pcb->pcb_regs, dxp->dx_ident->di_id);
		dxp->dx_ident->di_id = 0;
		dxp->dx_ident->di_flags &= ~DT_IDFLG_CGREG;
	}
}
