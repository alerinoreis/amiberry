/*
 * compiler/compemu_support.cpp - Core dynamic translation engine
 *
 * Copyright (c) 2001-2009 Milan Jurik of ARAnyM dev team (see AUTHORS)
 * 
 * Inspired by Christian Bauer's Basilisk II
 *
 * This file is part of the ARAnyM project which builds a new and powerful
 * TOS/FreeMiNT compatible virtual machine running on almost any hardware.
 *
 * JIT compiler m68k -> IA-32 and AMD64 / ARM
 *
 * Original 68040 JIT compiler for UAE, copyright 2000-2002 Bernd Meyer
 * Adaptation for Basilisk II and improvements, copyright 2000-2004 Gwenole Beauchesne
 * Portions related to CPU detection come from linux/arch/i386/kernel/setup.c
 *
 * ARAnyM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * ARAnyM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ARAnyM; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define writemem_special writemem
#define readmem_special  readmem

#define USE_MATCHSTATE 0
#include "sysconfig.h"
#include "sysdeps.h"

#include "machdep/m68k.h"
#include "options.h"
#include "events.h"
#include "include/memory.h"
#include "newcpu.h"
#include "comptbl.h"
#include "compiler/compemu.h"

#include "fpu/fpu.h"
#include "fpu/flags.h"

#define NATMEM_OFFSETX (uae_u32)NATMEM_OFFSET

 // %%% BRIAN KING WAS HERE %%%
extern bool canbang;
#include <sys/mman.h>
extern void jit_abort(const TCHAR*, ...);
compop_func *compfunctbl[65536];
compop_func *nfcompfunctbl[65536];
#ifdef NOFLAGS_SUPPORT
compop_func *nfcpufunctbl[65536];
#endif
uae_u8* comp_pc_p;

uae_u8* start_pc_p;
uae_u32 start_pc;
uae_u32 current_block_pc_p;
uae_u32 current_block_start_target;
uae_u32 needed_flags;
static uae_u32 next_pc_p;
static uae_u32 taken_pc_p;
static int     branch_cc;
int segvcount = 0;
int soft_flush_count = 0;
int hard_flush_count = 0;
int compile_count = 0;
int checksum_count = 0;
static uae_u8* current_compile_p = NULL;
static uae_u8* max_compile_start;
uae_u8* compiled_code = NULL;
static uae_s32 reg_alloc_run;

static int		lazy_flush = 1;	// Flag: lazy translation cache invalidation
static int		avoid_fpu = 1;	// Flag: compile FPU instructions ?
static int		have_cmov = 0;	// target has CMOV instructions ?
static int		have_rat_stall = 1;	// target has partial register stalls ?
const int		tune_alignment = 1;	// Tune code alignments for running CPU ?
const int		tune_nop_fillers = 1;	// Tune no-op fillers for architecture

static int		setzflg_uses_bsf = 0;	// setzflg virtual instruction can use native BSF instruction correctly?
static int		align_loops = 32;	// Align the start of loops
static int		align_jumps = 32;	// Align the start of jumps

void* pushall_call_handler = NULL;
static void* popall_do_nothing = NULL;
static void* popall_exec_nostats = NULL;
static void* popall_execute_normal = NULL;
static void* popall_cache_miss = NULL;
static void* popall_recompile_block = NULL;
static void* popall_check_checksum = NULL;

extern uae_u32 oink;
extern unsigned long foink3;
extern unsigned long foink;

/* The 68k only ever executes from even addresses. So right now, we
 * waste half the entries in this array
 * UPDATE: We now use those entries to store the start of the linked
 * lists that we maintain for each hash result.
 */
static cacheline cache_tags[TAGSIZE];
static int letit = 0;
static blockinfo* hold_bi[MAX_HOLD_BI];
static blockinfo* active;
static blockinfo* dormant;

op_properties prop[65536];

#ifdef NOFLAGS_SUPPORT
/* 68040 */
extern const struct comptbl op_smalltbl_0_nf[];
#endif
extern const struct comptbl op_smalltbl_0_comp_nf[];
extern const struct comptbl op_smalltbl_0_comp_ff[];
#ifdef NOFLAGS_SUPPORT
/* 68020 + 68881 */
extern const struct cputbl op_smalltbl_1_nf[];
/* 68020 */
extern const struct cputbl op_smalltbl_2_nf[];
/* 68010 */
extern const struct cputbl op_smalltbl_3_nf[];
/* 68000 */
extern const struct cputbl op_smalltbl_4_nf[];
/* 68000 slow but compatible.  */
extern const struct cputbl op_smalltbl_5_nf[];
#endif

static void flush_icache_hard(uae_u32 ptr, int n);

static bigstate live;
static smallstate empty_ss;
static smallstate default_ss;
static int optlev;

static int writereg(int r, int size);
static void unlock(int r);
static void setlock(int r);
static int readreg_specific(int r, int size, int spec);
static int writereg_specific(int r, int size, int spec);
static void prepare_for_call_1(void);
static void prepare_for_call_2(void);
static void align_target(uae_u32 a);

static uae_s32 nextused[VREGS];

static uae_u8 *popallspace;

uae_u32 m68k_pc_offset;

/* Some arithmetic ooperations can be optimized away if the operands
 * are known to be constant. But that's only a good idea when the
 * side effects they would have on the flags are not important. This
 * variable indicates whether we need the side effects or not
 */
uae_u32 needflags=0;

/* Flag handling is complicated.
 *
 * x86 instructions create flags, which quite often are exactly what we
 * want. So at times, the "68k" flags are actually in the x86 flags.
 *
 * Then again, sometimes we do x86 instructions that clobber the x86
 * flags, but don't represent a corresponding m68k instruction. In that
 * case, we have to save them.
 *
 * We used to save them to the stack, but now store them back directly
 * into the regflags.cznv of the traditional emulation. Thus some odd
 * names.
 *
 * So flags can be in either of two places (used to be three; boy were
 * things complicated back then!); And either place can contain either
 * valid flags or invalid trash (and on the stack, there was also the
 * option of "nothing at all", now gone). A couple of variables keep
 * track of the respective states.
 *
 * To make things worse, we might or might not be interested in the flags.
 * by default, we are, but a call to dont_care_flags can change that
 * until the next call to live_flags. If we are not, pretty much whatever
 * is in the register and/or the native flags is seen as valid.
 */

STATIC_INLINE blockinfo* get_blockinfo(uae_u32 cl)
{
	return cache_tags[cl + 1].bi;
}

STATIC_INLINE blockinfo* get_blockinfo_addr(void* addr)
{
	blockinfo*  bi = get_blockinfo(cacheline(addr));

	while (bi) {
		if (bi->pc_p == addr)
			return bi;
		bi = bi->next_same_cl;
	}
	return NULL;
}

		
/*******************************************************************
 * All sorts of list related functions for all of the lists        *
 *******************************************************************/

STATIC_INLINE void remove_from_cl_list(blockinfo* bi)
{
	uae_u32 cl = cacheline(bi->pc_p);

	if (bi->prev_same_cl_p)
		*(bi->prev_same_cl_p) = bi->next_same_cl;
	if (bi->next_same_cl)
		bi->next_same_cl->prev_same_cl_p = bi->prev_same_cl_p;
	if (cache_tags[cl + 1].bi)
		cache_tags[cl].handler = cache_tags[cl + 1].bi->handler_to_use;
	else
		cache_tags[cl].handler = (cpuop_func*)popall_execute_normal;
}

STATIC_INLINE void remove_from_list(blockinfo* bi)
{
	if (bi->prev_p)
		*(bi->prev_p) = bi->next;
	if (bi->next)
		bi->next->prev_p = bi->prev_p;
}

STATIC_INLINE void remove_from_lists(blockinfo* bi)
{
	remove_from_list(bi);
	remove_from_cl_list(bi);
}

STATIC_INLINE void add_to_cl_list(blockinfo* bi)
{
	uae_u32 cl = cacheline(bi->pc_p);

	if (cache_tags[cl + 1].bi)
		cache_tags[cl + 1].bi->prev_same_cl_p = &(bi->next_same_cl);
	bi->next_same_cl = cache_tags[cl + 1].bi;

	cache_tags[cl + 1].bi = bi;
	bi->prev_same_cl_p = &(cache_tags[cl + 1].bi);

	cache_tags[cl].handler = bi->handler_to_use;
}

STATIC_INLINE void raise_in_cl_list(blockinfo* bi)
{
	remove_from_cl_list(bi);
	add_to_cl_list(bi);
}

STATIC_INLINE void add_to_active(blockinfo* bi)
{
	if (active)
		active->prev_p = &(bi->next);
	bi->next = active;

	active = bi;
	bi->prev_p = &active;
}

STATIC_INLINE void add_to_dormant(blockinfo* bi)
{
	if (dormant)
		dormant->prev_p = &(bi->next);
	bi->next = dormant;

	dormant = bi;
	bi->prev_p = &dormant;
}

STATIC_INLINE void remove_dep(dependency* d)
{
	if (d->prev_p)
		*(d->prev_p) = d->next;
	if (d->next)
		d->next->prev_p = d->prev_p;
	d->prev_p = NULL;
	d->next = NULL;
}

/* This block's code is about to be thrown away, so it no longer
   depends on anything else */
STATIC_INLINE void remove_deps(blockinfo* bi)
{
	remove_dep(&(bi->dep[0]));
	remove_dep(&(bi->dep[1]));
}

STATIC_INLINE void adjust_jmpdep(dependency* d, void* a)
{
	*(d->jmp_off) = (uae_u32)a - ((uae_u32)d->jmp_off + 4);
}

/********************************************************************
 * Soft flush handling support functions                            *
 ********************************************************************/

STATIC_INLINE void set_dhtu(blockinfo* bi, void* dh)
{
	//write_log (_T("JIT: bi is %p\n"),bi);
	if (dh != bi->direct_handler_to_use) {
		dependency* x = bi->deplist;
		//write_log (_T("JIT: bi->deplist=%p\n"),bi->deplist);
		while (x) {
			//write_log (_T("JIT: x is %p\n"),x);
			//write_log (_T("JIT: x->next is %p\n"),x->next);
			//write_log (_T("JIT: x->prev_p is %p\n"),x->prev_p);

			if (x->jmp_off) {
				adjust_jmpdep(x, dh);
			}
			x = x->next;
		}
		bi->direct_handler_to_use = static_cast<cpuop_func*>(dh);
	}
}

STATIC_INLINE void invalidate_block(blockinfo* bi)
{
	int i;

	bi->optlevel = 0;
	bi->count = currprefs.optcount[0] - 1;
	bi->handler = NULL;
	bi->handler_to_use = static_cast<cpuop_func*>(popall_execute_normal);
	bi->direct_handler = NULL;
	set_dhtu(bi, bi->direct_pen);
	bi->needed_flags = 0xff;

	for (i = 0; i<2; i++) {
		bi->dep[i].jmp_off = NULL;
		bi->dep[i].target = NULL;
	}
	remove_deps(bi);
}

STATIC_INLINE void create_jmpdep(blockinfo* bi, int i, uae_u32* jmpaddr, uae_u32 target)
{
	blockinfo*  tbi = get_blockinfo_addr(reinterpret_cast<void*>(target));

	Dif(!tbi) {
		jit_abort(_T("JIT: Could not create jmpdep!\n"));
	}
	bi->dep[i].jmp_off = jmpaddr;
	bi->dep[i].target = tbi;
	bi->dep[i].next = tbi->deplist;
	if (bi->dep[i].next)
		bi->dep[i].next->prev_p = &(bi->dep[i].next);
	bi->dep[i].prev_p = &(tbi->deplist);
	tbi->deplist = &(bi->dep[i]);
}

STATIC_INLINE void big_to_small_state(bigstate* b, smallstate* s)
{
	int i;
	int count = 0;

	for (i = 0; i<N_REGS; i++) {
		s->nat[i].validsize = 0;
		s->nat[i].dirtysize = 0;
		if (b->nat[i].nholds) {
			int index = b->nat[i].nholds - 1;
			int r = b->nat[i].holds[index];
			s->nat[i].holds = r;
			s->nat[i].validsize = b->state[r].validsize;
			s->nat[i].dirtysize = b->state[r].dirtysize;
			count++;
		}
	}
	write_log(_T("JIT: count=%d\n"), count);
	for (i = 0; i<N_REGS; i++) {  // FIXME --- don't do dirty yet
		s->nat[i].dirtysize = 0;
	}
}

STATIC_INLINE void attached_state(blockinfo* bi)
{
	bi->havestate = 1;
	if (bi->direct_handler_to_use == bi->direct_handler)
		set_dhtu(bi, bi->direct_pen);
	bi->direct_handler = bi->direct_pen;
	bi->status = BI_TARGETTED;
}

STATIC_INLINE blockinfo* get_blockinfo_addr_new(void* addr, int setstate)
{
	blockinfo*  bi = get_blockinfo_addr(addr);
	int i;

#if USE_OPTIMIZER
	if (reg_alloc_run)
		return NULL;
#endif
	if (!bi) {
		for (i = 0; i<MAX_HOLD_BI && !bi; i++) {
			if (hold_bi[i]) {
				uae_u32 cl = cacheline(addr);

				bi = hold_bi[i];
				hold_bi[i] = NULL;
				bi->pc_p = (uae_u8*)addr;
				invalidate_block(bi);
				add_to_active(bi);
				add_to_cl_list(bi);

			}
		}
	}
	if (!bi) {
		jit_abort(_T("JIT: Looking for blockinfo, can't find free one\n"));
	}

#if USE_MATCHSTATE
	if (setstate &&
		!bi->havestate) {
		big_to_small_state(&live, &(bi->env));
		attached_state(bi);
	}
#endif
	return bi;
}

static void prepare_block(blockinfo* bi);

STATIC_INLINE void alloc_blockinfos(void)
{
	int i;
	blockinfo* bi;

	for (i = 0; i<MAX_HOLD_BI; i++) {
		if (hold_bi[i])
			return;
		bi = hold_bi[i] = (blockinfo*)current_compile_p;
		current_compile_p += sizeof(blockinfo);

		prepare_block(bi);
	}
}

/********************************************************************
* Preferences handling. This is just a convenient place to put it  *
********************************************************************/
extern bool have_done_picasso;

bool check_prefs_changed_comp(void)
{
	bool changed = 0;
	static int cachesize_prev, comptrust_prev;
	static bool canbang_prev;

	if (currprefs.comptrustbyte != changed_prefs.comptrustbyte ||
		currprefs.comptrustword != changed_prefs.comptrustword ||
		currprefs.comptrustlong != changed_prefs.comptrustlong ||
		currprefs.comptrustnaddr != changed_prefs.comptrustnaddr ||
		currprefs.compnf != changed_prefs.compnf ||
		currprefs.comp_hardflush != changed_prefs.comp_hardflush ||
		currprefs.comp_constjump != changed_prefs.comp_constjump ||
		currprefs.comp_oldsegv != changed_prefs.comp_oldsegv ||
		currprefs.compfpu != changed_prefs.compfpu ||
		currprefs.fpu_strict != changed_prefs.fpu_strict)
		changed = 1;

	currprefs.comptrustbyte = changed_prefs.comptrustbyte;
	currprefs.comptrustword = changed_prefs.comptrustword;
	currprefs.comptrustlong = changed_prefs.comptrustlong;
	currprefs.comptrustnaddr = changed_prefs.comptrustnaddr;
	currprefs.compnf = changed_prefs.compnf;
	currprefs.comp_hardflush = changed_prefs.comp_hardflush;
	currprefs.comp_constjump = changed_prefs.comp_constjump;
	currprefs.comp_oldsegv = changed_prefs.comp_oldsegv;
	currprefs.compfpu = changed_prefs.compfpu;
	currprefs.fpu_strict = changed_prefs.fpu_strict;

	if (currprefs.cachesize != changed_prefs.cachesize) {
		if (currprefs.cachesize && !changed_prefs.cachesize) {
			cachesize_prev = currprefs.cachesize;
			comptrust_prev = currprefs.comptrustbyte;
			canbang_prev = canbang;
		}
		else if (!currprefs.cachesize && changed_prefs.cachesize == cachesize_prev) {
			changed_prefs.comptrustbyte = currprefs.comptrustbyte = comptrust_prev;
			changed_prefs.comptrustword = currprefs.comptrustword = comptrust_prev;
			changed_prefs.comptrustlong = currprefs.comptrustlong = comptrust_prev;
			changed_prefs.comptrustnaddr = currprefs.comptrustnaddr = comptrust_prev;
		}
		currprefs.cachesize = changed_prefs.cachesize;
		alloc_cache();
		changed = 1;
	}
	if (!candirect)
		canbang = 0;

	// Turn off illegal-mem logging when using JIT...
	if (currprefs.cachesize)
		currprefs.illegal_mem = changed_prefs.illegal_mem;// = 0;

	currprefs.comp_midopt = changed_prefs.comp_midopt;
	currprefs.comp_lowopt = changed_prefs.comp_lowopt;

	if ((!canbang || !currprefs.cachesize) && currprefs.comptrustbyte != 1) {
		// Set all of these to indirect when canbang == 0
		// Basically, set the compforcesettings option...
		currprefs.comptrustbyte = 1;
		currprefs.comptrustword = 1;
		currprefs.comptrustlong = 1;
		currprefs.comptrustnaddr = 1;

		changed_prefs.comptrustbyte = 1;
		changed_prefs.comptrustword = 1;
		changed_prefs.comptrustlong = 1;
		changed_prefs.comptrustnaddr = 1;

		changed = 1;

		if (currprefs.cachesize)
			write_log(_T("JIT: Reverting to \"indirect\" access, because canbang is zero!\n"));
	}

	if (changed)
		write_log(_T("JIT: cache=%d. b=%d w=%d l=%d fpu=%d nf=%d const=%d hard=%d\n"),
			currprefs.cachesize,
			currprefs.comptrustbyte, currprefs.comptrustword, currprefs.comptrustlong,
			currprefs.compfpu, currprefs.compnf, currprefs.comp_constjump, currprefs.comp_hardflush);
	return changed;
}

/********************************************************************
 * Functions to emit data into memory, and other general support    *
 ********************************************************************/

static uae_u8* target;

static  void emit_init(void)
{
}

STATIC_INLINE void emit_byte(uae_u8 x)
{
	*target++ = x;
}

STATIC_INLINE void emit_word(uae_u16 x)
{
	*reinterpret_cast<uae_u16*>(target) = x;
	target += 2;
}

STATIC_INLINE void emit_long(uae_u32 x)
{
	*reinterpret_cast<uae_u32*>(target) = x;
	target += 4;
}

#if defined(USE_DATA_BUFFER)

static uae_u8* data_start;
static uae_u8* data_target;
static uae_u8* data_ptr;

static inline void data_byte(uae_u8 x)
{
    *(--data_target)=x;
}

static inline void data_word(uae_u16 x)
{
    data_target-=2;
    *((uae_u16*)data_target)=x;
}

static inline void data_long(uae_u32 x)
{
    data_target-=4;
    *((uae_u32*)data_target)=x;
}

static __inline__ void data_quad(uae_u64 x)
{
    data_target-=8;
    *((uae_u64*)data_target)=x;
}

static inline void data_skip(int size) {
	data_target -= size;
}

static inline void data_block(const uae_u8 *block, uae_u32 blocklen)
{
	data_target-=blocklen;
	memcpy((uae_u8 *)data_target,block,blocklen);
}

static inline void data_addr(void *addr) {
	data_target-=sizeof(void *);
	*((void **)data_target) = addr;
}

static inline long get_data_offset() {
	return data_target - data_start;
}

static inline uae_u8* get_data_target(void)
{
    return data_target;
}

static inline void set_data_start() {
	data_start = data_target;
}

static inline void reset_data_buffer() {
	data_target = data_ptr;
}

#define MAX_COMPILE_PTR 	data_target

#else

#define MAX_COMPILE_PTR		max_compile_start

#endif

STATIC_INLINE uae_u32 reverse32(uae_u32 oldv)
{
#if 1
	// gb-- We have specialized byteswapping functions, just use them
	return do_byteswap_32(oldv);
#else
	return ((v>>24)&0xff) | ((v>>8)&0xff00) | ((v<<8)&0xff0000) | ((v<<24)&0xff000000);
#endif
}

void set_target(uae_u8* t)
{
	target = t;
}

STATIC_INLINE uae_u8* get_target_noopt(void)
{
	return target;
}

STATIC_INLINE uae_u8* get_target(void)
{
	return get_target_noopt();
}

/********************************************************************
 * Getting the information about the target CPU                     *
 ********************************************************************/

#if defined(CPU_arm) 
#include "codegen_arm.cpp"
#endif

/********************************************************************
 * Flags status handling. EMIT TIME!                                *
 ********************************************************************/

static void bt_l_ri_noclobber(R4 r, IMM i);

static void make_flags_live_internal(void)
{
	if (live.flags_in_flags == VALID)
		return;
	Dif(live.flags_on_stack == TRASH) {
		jit_abort(_T("JIT: Want flags, got something on stack, but it is TRASH\n"));
	}
	if (live.flags_on_stack == VALID) {
		int tmp;
		tmp = readreg_specific(FLAGTMP, 4, FLAG_NREG2);
		raw_reg_to_flags(tmp);
		unlock(tmp);

		live.flags_in_flags = VALID;
		return;
	}
	jit_abort(_T("JIT: Huh? live.flags_in_flags=%d, live.flags_on_stack=%d, but need to make live\n"),
		live.flags_in_flags, live.flags_on_stack);
}

static void flags_to_stack(void)
{
	if (live.flags_on_stack == VALID)
		return;
	if (!live.flags_are_important) {
		live.flags_on_stack = VALID;
		return;
	}
	Dif(live.flags_in_flags != VALID)
		jit_abort(_T("flags_to_stack != VALID"));
	else {
		int tmp;
		tmp = writereg_specific(FLAGTMP, 4, FLAG_NREG1);
		raw_flags_to_reg(tmp);
		unlock(tmp);
	}
	live.flags_on_stack = VALID;
}

STATIC_INLINE void clobber_flags(void)
{
	if (live.flags_in_flags == VALID && live.flags_on_stack != VALID)
		flags_to_stack();
	live.flags_in_flags = TRASH;
}

/* Prepare for leaving the compiled stuff */
STATIC_INLINE void flush_flags(void)
{
	flags_to_stack();
	return;
}

int touchcnt;

/********************************************************************
 * register allocation per block logging                            *
 ********************************************************************/

static uae_s8 vstate[VREGS];
static uae_s8 nstate[N_REGS];

#define L_UNKNOWN -127
#define L_UNAVAIL -1
#define L_NEEDED -2
#define L_UNNEEDED -3

STATIC_INLINE void log_startblock(void)
{
	int i;
	for (i = 0; i<VREGS; i++)
		vstate[i] = L_UNKNOWN;
	for (i = 0; i<N_REGS; i++)
		nstate[i] = L_UNKNOWN;
}

STATIC_INLINE void log_isused(int n)
{
	if (nstate[n] == L_UNKNOWN)
		nstate[n] = L_UNAVAIL;
}

STATIC_INLINE void log_isreg(int n, int r)
{
	if (nstate[n] == L_UNKNOWN)
		nstate[n] = r;
	if (vstate[r] == L_UNKNOWN)
		vstate[r] = L_NEEDED;
}

STATIC_INLINE void log_clobberreg(int r)
{
	if (vstate[r] == L_UNKNOWN)
		vstate[r] = L_UNNEEDED;
}

/* This ends all possibility of clever register allocation */

STATIC_INLINE void log_flush(void)
{
	int i;
	for (i = 0; i<VREGS; i++)
		if (vstate[i] == L_UNKNOWN)
			vstate[i] = L_NEEDED;
	for (i = 0; i<N_REGS; i++)
		if (nstate[i] == L_UNKNOWN)
			nstate[i] = L_UNAVAIL;
}

STATIC_INLINE void log_dump(void)
{
	int i;

	return;
}

/********************************************************************
 * register status handling. EMIT TIME!                             *
 ********************************************************************/

STATIC_INLINE void set_status(int r, int status)
{
	if (status == ISCONST)
		log_clobberreg(r);
	live.state[r].status = status;
}

STATIC_INLINE int isinreg(int r)
{
	return live.state[r].status == CLEAN || live.state[r].status == DIRTY;
}

STATIC_INLINE void adjust_nreg(int r, uae_u32 val)
{
	if (!val)
		return;
	raw_lea_l_brr(r, r, val);
}

static  void tomem(int r)
{
	int rr = live.state[r].realreg;

	if (isinreg(r)) {
		if (live.state[r].val &&
			live.nat[rr].nholds == 1 &&
			!live.nat[rr].locked) {
			// write_log (_T("JIT: RemovingA offset %x from reg %d (%d) at %p\n"),
			//   live.state[r].val,r,rr,target);
			adjust_nreg(rr, live.state[r].val);
			live.state[r].val = 0;
			live.state[r].dirtysize = 4;
			set_status(r, DIRTY);
		}
	}

	if (live.state[r].status == DIRTY) {
		switch (live.state[r].dirtysize) {
		case 1: raw_mov_b_mr(uae_u32(live.state[r].mem), rr); break;
		case 2: raw_mov_w_mr(uae_u32(live.state[r].mem), rr); break;
		case 4: raw_mov_l_mr(uae_u32(live.state[r].mem), rr); break;
		default: abort();
		}
		set_status(r, CLEAN);
		live.state[r].dirtysize = 0;
	}
}

STATIC_INLINE int isconst(int r)
{
	return live.state[r].status == ISCONST;
}

int is_const(int r)
{
	return isconst(r);
}

STATIC_INLINE void writeback_const(int r)
{
	if (!isconst(r))
		return;
	Dif(live.state[r].needflush == NF_HANDLER) {
		jit_abort(_T("JIT: Trying to write back constant NF_HANDLER!\n"));
	}

	raw_mov_l_mi((uae_u32)live.state[r].mem, live.state[r].val);
	live.state[r].val = 0;
	set_status(r, INMEM);
}

STATIC_INLINE void tomem_c(int r)
{
	if (isconst(r)) {
		writeback_const(r);
	}
	else
		tomem(r);
}

static  void evict(int r)
{
	int rr;

	if (!isinreg(r))
		return;
	tomem(r);
	rr = live.state[r].realreg;

	Dif(live.nat[rr].locked &&
		live.nat[rr].nholds == 1) {
		jit_abort(_T("JIT: register %d in nreg %d is locked!\n"), r, live.state[r].realreg);
	}

	live.nat[rr].nholds--;
	if (live.nat[rr].nholds != live.state[r].realind) { /* Was not last */
		int topreg = live.nat[rr].holds[live.nat[rr].nholds];
		int thisind = live.state[r].realind;
		live.nat[rr].holds[thisind] = topreg;
		live.state[topreg].realind = thisind;
	}
	live.state[r].realreg = -1;
	set_status(r, INMEM);
}

STATIC_INLINE void free_nreg(int r)
{
	int i = live.nat[r].nholds;

	while (i) {
		int vr;

		--i;
		vr = live.nat[r].holds[i];
		evict(vr);
	}
	Dif(live.nat[r].nholds != 0) {
		jit_abort(_T("JIT: Failed to free nreg %d, nholds is %d\n"), r, live.nat[r].nholds);
	}
}

/* Use with care! */
STATIC_INLINE void isclean(int r)
{
	if (!isinreg(r))
		return;
	live.state[r].validsize = 4;
	live.state[r].dirtysize = 0;
	live.state[r].val = 0;
	set_status(r, CLEAN);
}

STATIC_INLINE void disassociate(int r)
{
	isclean(r);
	evict(r);
}

STATIC_INLINE void set_const(int r, uae_u32 val)
{
	disassociate(r);
	live.state[r].val = val;
	set_status(r, ISCONST);
}

STATIC_INLINE uae_u32 get_offset(int r)
{
	return live.state[r].val;
}

static  int alloc_reg_hinted(int r, int size, int willclobber, int hint)
{
	int bestreg;
	uae_s32 when;
	int i;
	uae_s32 badness = 0; /* to shut up gcc */
	bestreg = -1;
	when = 2000000000;

	for (i = N_REGS; i--;) {
		badness = live.nat[i].touched;
		if (live.nat[i].nholds == 0)
			badness = 0;
		if (i == hint)
			badness -= 200000000;
		if (!live.nat[i].locked && badness<when) {
			if ((size == 1 && live.nat[i].canbyte) ||
				(size == 2 && live.nat[i].canword) ||
				(size == 4)) {
				bestreg = i;
				when = badness;
				if (live.nat[i].nholds == 0 && hint<0)
					break;
				if (i == hint)
					break;
			}
		}
	}
	Dif(bestreg == -1)
		jit_abort(_T("alloc_reg_hinted bestreg=-1"));

	if (live.nat[bestreg].nholds>0) {
		free_nreg(bestreg);
	}
	if (isinreg(r)) {
		int rr = live.state[r].realreg;
		/* This will happen if we read a partially dirty register at a
		bigger size */
		Dif(willclobber || live.state[r].validsize >= size)
			jit_abort(_T("willclobber || live.state[r].validsize>=size"));
		Dif(live.nat[rr].nholds != 1)
			jit_abort(_T("live.nat[rr].nholds!=1"));
		if (size == 4 && live.state[r].validsize == 2) {
			log_isused(bestreg);
			raw_mov_l_rm(bestreg, (uae_u32)live.state[r].mem);
			raw_bswap_32(bestreg);
			raw_zero_extend_16_rr(rr, rr);
			raw_zero_extend_16_rr(bestreg, bestreg);
			raw_bswap_32(bestreg);
			raw_lea_l_rr_indexed(rr, rr, bestreg);
			live.state[r].validsize = 4;
			live.nat[rr].touched = touchcnt++;
			return rr;
		}
		if (live.state[r].validsize == 1) {
			/* Nothing yet */
		}
		evict(r);
	}

	if (!willclobber) {
		if (live.state[r].status != UNDEF) {
			if (isconst(r)) {
				compemu_raw_mov_l_ri(bestreg, live.state[r].val);
				live.state[r].val = 0;
				live.state[r].dirtysize = 4;
				set_status(r, DIRTY);
				log_isused(bestreg);
			}
			else {
				if (r == FLAGTMP)
					raw_load_flagreg(bestreg, r);
				else if (r == FLAGX)
					raw_load_flagx(bestreg, r);
				else {
					raw_mov_l_rm(bestreg, (uae_u32)live.state[r].mem);
				}
				live.state[r].dirtysize = 0;
				set_status(r, CLEAN);
				log_isreg(bestreg, r);
			}
		}
		else {
			live.state[r].val = 0;
			live.state[r].dirtysize = 0;
			set_status(r, CLEAN);
			log_isused(bestreg);
		}
		live.state[r].validsize = 4;
	}
	else { /* this is the easiest way, but not optimal. FIXME! */
		   /* Now it's trickier, but hopefully still OK */
		if (!isconst(r) || size == 4) {
			live.state[r].validsize = size;
			live.state[r].dirtysize = size;
			live.state[r].val = 0;
			set_status(r, DIRTY);
			if (size == 4)
				log_isused(bestreg);
			else
				log_isreg(bestreg, r);
		}
		else {
			if (live.state[r].status != UNDEF)
				compemu_raw_mov_l_ri(bestreg, live.state[r].val);
			live.state[r].val = 0;
			live.state[r].validsize = 4;
			live.state[r].dirtysize = 4;
			set_status(r, DIRTY);
			log_isused(bestreg);
		}
	}
	live.state[r].realreg = bestreg;
	live.state[r].realind = live.nat[bestreg].nholds;
	live.nat[bestreg].touched = touchcnt++;
	live.nat[bestreg].holds[live.nat[bestreg].nholds] = r;
	live.nat[bestreg].nholds++;

	return bestreg;
}

static  int alloc_reg(int r, int size, int willclobber)
{
	return alloc_reg_hinted(r, size, willclobber, -1);
}

static  void unlock(int r)
{
	Dif(!live.nat[r].locked)
		jit_abort(_T("unlock %d not locked"), r);
	live.nat[r].locked--;
}

static  void setlock(int r)
{
	live.nat[r].locked++;
}


static void mov_nregs(int d, int s)
{
	int ns = live.nat[s].nholds;
	int nd = live.nat[d].nholds;
	int i;

	if (s == d)
		return;

	if (nd>0)
		free_nreg(d);

	raw_mov_l_rr(d, s);
	log_isused(d);

	for (i = 0; i<live.nat[s].nholds; i++) {
		int vs = live.nat[s].holds[i];

		live.state[vs].realreg = d;
		live.state[vs].realind = i;
		live.nat[d].holds[i] = vs;
	}
	live.nat[d].nholds = live.nat[s].nholds;

	live.nat[s].nholds = 0;
}


STATIC_INLINE void make_exclusive(int r, int size, int spec)
{
	reg_status oldstate;
	int rr = live.state[r].realreg;
	int nr;
	int nind;
	int ndirt = 0;
	int i;

	if (!isinreg(r))
		return;
	if (live.nat[rr].nholds == 1)
		return;
	for (i = 0; i<live.nat[rr].nholds; i++) {
		int vr = live.nat[rr].holds[i];
		if (vr != r &&
			(live.state[vr].status == DIRTY || live.state[vr].val))
			ndirt++;
	}
	if (!ndirt && size<live.state[r].validsize && !live.nat[rr].locked) {
		/* Everything else is clean, so let's keep this register */
		for (i = 0; i<live.nat[rr].nholds; i++) {
			int vr = live.nat[rr].holds[i];
			if (vr != r) {
				evict(vr);
				i--; /* Try that index again! */
			}
		}
		Dif(live.nat[rr].nholds != 1) {
			jit_abort(_T("JIT: natreg %d holds %d vregs, %d not exclusive\n"),
				rr, live.nat[rr].nholds, r);
		}
		return;
	}

	/* We have to split the register */
	oldstate = live.state[r];

	setlock(rr); /* Make sure this doesn't go away */
				 /* Forget about r being in the register rr */
	disassociate(r);
	/* Get a new register, that we will clobber completely */
	if (oldstate.status == DIRTY) {
		/* If dirtysize is <4, we need a register that can handle the
		eventual smaller memory store! Thanks to Quake68k for exposing
		this detail ;-) */
		nr = alloc_reg_hinted(r, oldstate.dirtysize, 1, spec);
	}
	else {
		nr = alloc_reg_hinted(r, 4, 1, spec);
	}
	nind = live.state[r].realind;
	live.state[r] = oldstate;   /* Keep all the old state info */
	live.state[r].realreg = nr;
	live.state[r].realind = nind;

	if (size<live.state[r].validsize) {
		if (live.state[r].val) {
			/* Might as well compensate for the offset now */
			raw_lea_l_brr(nr, rr, oldstate.val);
			live.state[r].val = 0;
			live.state[r].dirtysize = 4;
			set_status(r, DIRTY);
		}
		else
			raw_mov_l_rr(nr, rr);  /* Make another copy */
	}
	unlock(rr);
}

STATIC_INLINE void add_offset(int r, uae_u32 off)
{
	live.state[r].val += off;
}

STATIC_INLINE void remove_offset(int r, int spec)
{
	int rr;

	if (isconst(r))
		return;
	if (live.state[r].val == 0)
		return;
	if (isinreg(r) && live.state[r].validsize<4)
		evict(r);

	if (!isinreg(r))
		alloc_reg_hinted(r, 4, 0, spec);

	Dif(live.state[r].validsize != 4) {
		jit_abort(_T("JIT: Validsize=%d in remove_offset\n"), live.state[r].validsize);
	}
	make_exclusive(r, 0, -1);
	/* make_exclusive might have done the job already */
	if (live.state[r].val == 0)
		return;

	rr = live.state[r].realreg;

	if (live.nat[rr].nholds == 1) {
		//write_log (_T("JIT: RemovingB offset %x from reg %d (%d) at %p\n"),
		//       live.state[r].val,r,rr,target);
		adjust_nreg(rr, live.state[r].val);
		live.state[r].dirtysize = 4;
		live.state[r].val = 0;
		set_status(r, DIRTY);
		return;
	}
	jit_abort(_T("JIT: Failed in remove_offset\n"));
}

STATIC_INLINE void remove_all_offsets(void)
{
	int i;

	for (i = 0; i<VREGS; i++)
		remove_offset(i, -1);
}

STATIC_INLINE int readreg_general(int r, int size, int spec, int can_offset)
{
	int n;
	int answer = -1;

	if (live.state[r].status == UNDEF) {
		write_log(_T("JIT: WARNING: Unexpected read of undefined register %d\n"), r);
	}
	if (!can_offset)
		remove_offset(r, spec);

	if (isinreg(r) && live.state[r].validsize >= size) {
		n = live.state[r].realreg;
		switch (size) {
		case 1:
			if (live.nat[n].canbyte || spec >= 0) {
				answer = n;
			}
			break;
		case 2:
			if (live.nat[n].canword || spec >= 0) {
				answer = n;
			}
			break;
		case 4:
			answer = n;
			break;
		default: abort();
		}
		if (answer<0)
			evict(r);
	}
	/* either the value was in memory to start with, or it was evicted and
	is in memory now */
	if (answer<0) {
		answer = alloc_reg_hinted(r, spec >= 0 ? 4 : size, 0, spec);
	}

	if (spec >= 0 && spec != answer) {
		/* Too bad */
		mov_nregs(spec, answer);
		answer = spec;
	}
	live.nat[answer].locked++;
	live.nat[answer].touched = touchcnt++;
	return answer;
}



static int readreg(int r, int size)
{
	return readreg_general(r, size, -1, 0);
}

static int readreg_specific(int r, int size, int spec)
{
	return readreg_general(r, size, spec, 0);
}

static int readreg_offset(int r, int size)
{
	return readreg_general(r, size, -1, 1);
}

/* writereg_general(r, size, spec)
 *
 * INPUT
 * - r    : mid-layer register
 * - size : requested size (1/2/4)
 * - spec : -1 if find or make a register free, otherwise specifies
 *          the physical register to use in any case
 *
 * OUTPUT
 * - hard (physical, x86 here) register allocated to virtual register r
 */
STATIC_INLINE int writereg_general(int r, int size, int spec)
{
	int n;
	int answer = -1;

	if (size<4) {
		remove_offset(r, spec);
	}

	make_exclusive(r, size, spec);
	if (isinreg(r)) {
		int nvsize = size>live.state[r].validsize ? size : live.state[r].validsize;
		int ndsize = size>live.state[r].dirtysize ? size : live.state[r].dirtysize;
		n = live.state[r].realreg;

		Dif(live.nat[n].nholds != 1)
			jit_abort(_T("live.nat[%d].nholds!=1"), n);
		switch (size) {
		case 1:
			if (live.nat[n].canbyte || spec >= 0) {
				live.state[r].dirtysize = ndsize;
				live.state[r].validsize = nvsize;
				answer = n;
			}
			break;
		case 2:
			if (live.nat[n].canword || spec >= 0) {
				live.state[r].dirtysize = ndsize;
				live.state[r].validsize = nvsize;
				answer = n;
			}
			break;
		case 4:
			live.state[r].dirtysize = ndsize;
			live.state[r].validsize = nvsize;
			answer = n;
			break;
		default: abort();
		}
		if (answer<0)
			evict(r);
	}
	/* either the value was in memory to start with, or it was evicted and
	is in memory now */
	if (answer<0) {
		answer = alloc_reg_hinted(r, size, 1, spec);
	}
	if (spec >= 0 && spec != answer) {
		mov_nregs(spec, answer);
		answer = spec;
	}
	if (live.state[r].status == UNDEF)
		live.state[r].validsize = 4;
	live.state[r].dirtysize = size>live.state[r].dirtysize ? size : live.state[r].dirtysize;
	live.state[r].validsize = size>live.state[r].validsize ? size : live.state[r].validsize;

	live.nat[answer].locked++;
	live.nat[answer].touched = touchcnt++;
	if (size == 4) {
		live.state[r].val = 0;
	}
	else {
		Dif(live.state[r].val) {
			jit_abort(_T("JIT: Problem with val\n"));
		}
	}
	set_status(r, DIRTY);
	return answer;
}

static int writereg(int r, int size)
{
	return writereg_general(r, size, -1);
}

static int writereg_specific(int r, int size, int spec)
{
	return writereg_general(r, size, spec);
}

STATIC_INLINE int rmw_general(int r, int wsize, int rsize, int spec)
{
	int n;
	int answer = -1;

	if (live.state[r].status == UNDEF) {
		write_log(_T("JIT: WARNING: Unexpected read of undefined register %d\n"), r);
	}
	remove_offset(r, spec);
	make_exclusive(r, 0, spec);

	Dif(wsize<rsize) {
		jit_abort(_T("JIT: Cannot handle wsize<rsize in rmw_general()\n"));
	}
	if (isinreg(r) && live.state[r].validsize >= rsize) {
		n = live.state[r].realreg;
		Dif(live.nat[n].nholds != 1)
			jit_abort(_T("live.nat[n].nholds!=1"), n);

		switch (rsize) {
		case 1:
			if (live.nat[n].canbyte || spec >= 0) {
				answer = n;
			}
			break;
		case 2:
			if (live.nat[n].canword || spec >= 0) {
				answer = n;
			}
			break;
		case 4:
			answer = n;
			break;
		default: abort();
		}
		if (answer<0)
			evict(r);
	}
	/* either the value was in memory to start with, or it was evicted and
	is in memory now */
	if (answer<0) {
		answer = alloc_reg_hinted(r, spec >= 0 ? 4 : rsize, 0, spec);
	}

	if (spec >= 0 && spec != answer) {
		/* Too bad */
		mov_nregs(spec, answer);
		answer = spec;
	}
	if (wsize>live.state[r].dirtysize)
		live.state[r].dirtysize = wsize;
	if (wsize>live.state[r].validsize)
		live.state[r].validsize = wsize;
	set_status(r, DIRTY);

	live.nat[answer].locked++;
	live.nat[answer].touched = touchcnt++;

	Dif(live.state[r].val) {
		jit_abort(_T("JIT: Problem with val(rmw)\n"));
	}
	return answer;
}

static int rmw(int r, int wsize, int rsize)
{
	return rmw_general(r, wsize, rsize, -1);
}

static int rmw_specific(int r, int wsize, int rsize, int spec)
{
	return rmw_general(r, wsize, rsize, spec);
}


/* needed for restoring the carry flag on non-P6 cores */
static void bt_l_ri_noclobber(RR4 r, IMM i)
{
	int size=4;
	if (i<16)
		size=2;
	r=readreg(r,size);
	raw_bt_l_ri(r,i);
	unlock(r);
}

/********************************************************************
 * FPU register status handling. EMIT TIME!                         *
 ********************************************************************/

static  void f_tomem(int r)
{
	if (live.fate[r].status == DIRTY) {
#if USE_LONG_DOUBLE
		raw_fmov_ext_mr((uae_u32)live.fate[r].mem, live.fate[r].realreg);
#else
		raw_fmov_mr(uae_u32(live.fate[r].mem), live.fate[r].realreg);
#endif
		live.fate[r].status = CLEAN;
	}
}

static  void f_tomem_drop(int r)
{
	if (live.fate[r].status == DIRTY) {
#if USE_LONG_DOUBLE
		raw_fmov_ext_mr_drop((uae_u32)live.fate[r].mem, live.fate[r].realreg);
#else
		raw_fmov_mr_drop(uae_u32(live.fate[r].mem), live.fate[r].realreg);
#endif
		live.fate[r].status = INMEM;
	}
}


STATIC_INLINE int f_isinreg(int r)
{
	return live.fate[r].status == CLEAN || live.fate[r].status == DIRTY;
}

static void f_evict(int r)
{
	int rr;

	if (!f_isinreg(r))
		return;
	rr = live.fate[r].realreg;
	if (live.fat[rr].nholds == 1)
		f_tomem_drop(r);
	else
		f_tomem(r);

	Dif(live.fat[rr].locked &&
		live.fat[rr].nholds == 1) {
		jit_abort(_T("JIT: FPU register %d in nreg %d is locked!\n"), r, live.fate[r].realreg);
	}

	live.fat[rr].nholds--;
	if (live.fat[rr].nholds != live.fate[r].realind) { /* Was not last */
		int topreg = live.fat[rr].holds[live.fat[rr].nholds];
		int thisind = live.fate[r].realind;
		live.fat[rr].holds[thisind] = topreg;
		live.fate[topreg].realind = thisind;
	}
	live.fate[r].status = INMEM;
	live.fate[r].realreg = -1;
}

STATIC_INLINE void f_free_nreg(int r)
{
	int i = live.fat[r].nholds;

	while (i) {
		int vr;

		--i;
		vr = live.fat[r].holds[i];
		f_evict(vr);
	}
	Dif(live.fat[r].nholds != 0) {
		jit_abort(_T("JIT: Failed to free nreg %d, nholds is %d\n"), r, live.fat[r].nholds);
	}
}


/* Use with care! */
STATIC_INLINE void f_isclean(int r)
{
	if (!f_isinreg(r))
		return;
	live.fate[r].status = CLEAN;
}

STATIC_INLINE void f_disassociate(int r)
{
	f_isclean(r);
	f_evict(r);
}



static  int f_alloc_reg(int r, int willclobber)
{
	int bestreg;
	uae_s32 when;
	int i;
	uae_s32 badness;
	bestreg = -1;
	when = 2000000000;
	for (i = N_FREGS; i--;) {
		badness = live.fat[i].touched;
		if (live.fat[i].nholds == 0)
			badness = 0;

		if (!live.fat[i].locked && badness<when) {
			bestreg = i;
			when = badness;
			if (live.fat[i].nholds == 0)
				break;
		}
	}
	Dif(bestreg == -1)
		abort();

	if (live.fat[bestreg].nholds>0) {
		f_free_nreg(bestreg);
	}
	if (f_isinreg(r)) {
		f_evict(r);
	}

	if (!willclobber) {
		if (live.fate[r].status != UNDEF) {
#if USE_LONG_DOUBLE
			raw_fmov_ext_rm(bestreg, (uae_u32)live.fate[r].mem);
#else
			raw_fmov_rm(bestreg, uae_u32(live.fate[r].mem));
#endif
		}
		live.fate[r].status = CLEAN;
	}
	else {
		live.fate[r].status = DIRTY;
	}
	live.fate[r].realreg = bestreg;
	live.fate[r].realind = live.fat[bestreg].nholds;
	live.fat[bestreg].touched = touchcnt++;
	live.fat[bestreg].holds[live.fat[bestreg].nholds] = r;
	live.fat[bestreg].nholds++;

	return bestreg;
}

static  void f_unlock(int r)
{
	Dif(!live.fat[r].locked)
		jit_abort(_T("unlock %d"), r);
	live.fat[r].locked--;
}

static  void f_setlock(int r)
{
	live.fat[r].locked++;
}

STATIC_INLINE int f_readreg(int r)
{
	int n;
	int answer = -1;

	if (f_isinreg(r)) {
		n = live.fate[r].realreg;
		answer = n;
	}
	/* either the value was in memory to start with, or it was evicted and
	is in memory now */
	if (answer<0)
		answer = f_alloc_reg(r, 0);

	live.fat[answer].locked++;
	live.fat[answer].touched = touchcnt++;
	return answer;
}

STATIC_INLINE void f_make_exclusive(int r, int clobber)
{
	freg_status oldstate;
	int rr = live.fate[r].realreg;
	int nr;
	int nind;
	int ndirt = 0;
	int i;

	if (!f_isinreg(r))
		return;
	if (live.fat[rr].nholds == 1)
		return;
	for (i = 0; i<live.fat[rr].nholds; i++) {
		int vr = live.fat[rr].holds[i];
		if (vr != r && live.fate[vr].status == DIRTY)
			ndirt++;
	}
	if (!ndirt && !live.fat[rr].locked) {
		/* Everything else is clean, so let's keep this register */
		for (i = 0; i<live.fat[rr].nholds; i++) {
			int vr = live.fat[rr].holds[i];
			if (vr != r) {
				f_evict(vr);
				i--; /* Try that index again! */
			}
		}
		Dif(live.fat[rr].nholds != 1) {
			write_log(_T("JIT: realreg %d holds %d ("), rr, live.fat[rr].nholds);
			for (i = 0; i<live.fat[rr].nholds; i++) {
				write_log(_T("JIT: %d(%d,%d)"), live.fat[rr].holds[i],
					live.fate[live.fat[rr].holds[i]].realreg,
					live.fate[live.fat[rr].holds[i]].realind);
			}
			write_log(_T("\n"));
			jit_abort(_T("x"));
		}
		return;
	}

	/* We have to split the register */
	oldstate = live.fate[r];

	f_setlock(rr); /* Make sure this doesn't go away */
				   /* Forget about r being in the register rr */
	f_disassociate(r);
	/* Get a new register, that we will clobber completely */
	nr = f_alloc_reg(r, 1);
	nind = live.fate[r].realind;
	if (!clobber)
		raw_fmov_rr(nr, rr);  /* Make another copy */
	live.fate[r] = oldstate;   /* Keep all the old state info */
	live.fate[r].realreg = nr;
	live.fate[r].realind = nind;
	f_unlock(rr);
}


STATIC_INLINE int f_writereg(int r)
{
	int n;
	int answer = -1;

	f_make_exclusive(r, 1);
	if (f_isinreg(r)) {
		n = live.fate[r].realreg;
		answer = n;
	}
	if (answer<0) {
		answer = f_alloc_reg(r, 1);
	}
	live.fate[r].status = DIRTY;
	live.fat[answer].locked++;
	live.fat[answer].touched = touchcnt++;
	return answer;
}

static int f_rmw(int r)
{
	int n;

	f_make_exclusive(r, 0);
	if (f_isinreg(r)) {
		n = live.fate[r].realreg;
	}
	else
		n = f_alloc_reg(r, 0);
	live.fate[r].status = DIRTY;
	live.fat[n].locked++;
	live.fat[n].touched = touchcnt++;
	return n;
}

static void fflags_into_flags_internal(uae_u32 tmp)
{
	int r;

	clobber_flags();
	r = f_readreg(FP_RESULT);
	raw_fflags_into_flags(r);
	f_unlock(r);
}


#if defined(CPU_arm)
#include "compemu_midfunc_arm.cpp"
#endif

//#if defined(CPU_i386) || defined(CPU_x86_64)
//#include "compemu_midfunc_x86.cpp"
//#endif


/********************************************************************
 * Support functions exposed to gencomp. CREATE time                *
 ********************************************************************/

int kill_rodent(int r)
{
	return KILLTHERAT &&
		have_rat_stall &&
		(live.state[r].status == INMEM ||
			live.state[r].status == CLEAN ||
			live.state[r].status == ISCONST ||
			live.state[r].dirtysize == 4);
}

uae_u32 get_const(int r)
{
#if USE_OPTIMIZER
	if (!reg_alloc_run)
#endif
		Dif(!isconst(r)) {
		jit_abort(_T("JIT: Register %d should be constant, but isn't\n"), r);
	}
	return live.state[r].val;
}

void sync_m68k_pc(void)
{
	if (m68k_pc_offset) {
		add_l_ri(PC_P, m68k_pc_offset);
		comp_pc_p += m68k_pc_offset;
		m68k_pc_offset = 0;
	}
}
    
/********************************************************************
* Support functions exposed to newcpu                              *
********************************************************************/

uae_u32 scratch[VREGS];
fptype fscratch[VFREGS];

void init_comp(void)
{
	int i;
	uae_u8* cb = can_byte;
	uae_u8* cw = can_word;
	uae_u8* au = always_used;

	for (i = 0; i<VREGS; i++) {
		live.state[i].realreg = -1;
		live.state[i].needflush = NF_SCRATCH;
		live.state[i].val = 0;
		set_status(i, UNDEF);
	}

	for (i = 0; i<VFREGS; i++) {
		live.fate[i].status = UNDEF;
		live.fate[i].realreg = -1;
		live.fate[i].needflush = NF_SCRATCH;
	}

	for (i = 0; i<VREGS; i++) {
		if (i<16) { /* First 16 registers map to 68k registers */
			live.state[i].mem = &regs.regs[i];
			live.state[i].needflush = NF_TOMEM;
			set_status(i, INMEM);
		}
		else
			live.state[i].mem = scratch + i;
	}
	live.state[PC_P].mem = (uae_u32*)&(regs.pc_p);
	live.state[PC_P].needflush = NF_TOMEM;
	set_const(PC_P, uae_u32(comp_pc_p));

	live.state[FLAGX].mem = &(regflags.x);
	live.state[FLAGX].needflush = NF_TOMEM;
	set_status(FLAGX, INMEM);
	
#if defined(CPU_arm)
    live.state[FLAGTMP].mem=(uae_u32*)&(regflags.nzcv);
#else
    live.state[FLAGTMP].mem=(uae_u32*)&(regflags.cznv);
#endif
    live.state[FLAGTMP].needflush=NF_TOMEM;
    set_status(FLAGTMP,INMEM);

    live.state[NEXT_HANDLER].needflush=NF_HANDLER;
    set_status(NEXT_HANDLER,UNDEF);

	for (i = 0; i<VFREGS; i++) {
		if (i<8) { /* First 8 registers map to 68k FPU registers */
			live.fate[i].mem = reinterpret_cast<uae_u32*>(&regs.fp[i].fp);
			live.fate[i].needflush = NF_TOMEM;
			live.fate[i].status = INMEM;
		}
		else if (i == FP_RESULT) {
			live.fate[i].mem = reinterpret_cast<uae_u32*>(&regs.fp_result);
			live.fate[i].needflush = NF_TOMEM;
			live.fate[i].status = INMEM;
		}
		else
			live.fate[i].mem = reinterpret_cast<uae_u32*>(fscratch + i);
	}

	for (i = 0; i<N_REGS; i++) {
		live.nat[i].touched = 0;
		live.nat[i].nholds = 0;
		live.nat[i].locked = 0;
		if (*cb == i) {
			live.nat[i].canbyte = 1; cb++;
		}
		else live.nat[i].canbyte = 0;
		if (*cw == i) {
			live.nat[i].canword = 1; cw++;
		}
		else live.nat[i].canword = 0;
		if (*au == i) {
			live.nat[i].locked = 1; au++;
		}
	}

	for (i = 0; i<N_FREGS; i++) {
		live.fat[i].touched = 0;
		live.fat[i].nholds = 0;
		live.fat[i].locked = 0;
	}

	touchcnt = 1;
	m68k_pc_offset = 0;
	live.flags_in_flags = TRASH;
	live.flags_on_stack = VALID;
	live.flags_are_important = 1;

	raw_fp_init();
}

static void vinton(int i, uae_s8* vton, int depth)
{
	int n;
	int rr;

	Dif(vton[i] == -1) {
		jit_abort(_T("JIT: Asked to load register %d, but nowhere to go\n"), i);
	}
	n = vton[i];
	Dif(live.nat[n].nholds>1)
		jit_abort(_T("vinton"));
	if (live.nat[n].nholds && depth<N_REGS) {
		vinton(live.nat[n].holds[0], vton, depth + 1);
	}
	if (!isinreg(i))
		return;  /* Oops --- got rid of that one in the recursive calls */
	rr = live.state[i].realreg;
	if (rr != n)
		mov_nregs(n, rr);
}

#if USE_MATCHSTATE
/* This is going to be, amongst other things, a more elaborate version of
flush() */
STATIC_INLINE void match_states(smallstate* s)
{
	uae_s8 vton[VREGS];
	uae_s8 ndone[N_REGS];
	int i;
	int again = 0;

	for (i = 0; i<VREGS; i++)
		vton[i] = -1;

	for (i = 0; i<N_REGS; i++)
		if (s->nat[i].validsize)
			vton[s->nat[i].holds] = i;

	flush_flags(); /* low level */
	sync_m68k_pc(); /* mid level */

					/* We don't do FREGS yet, so this is raw flush() code */
	for (i = 0; i<VFREGS; i++) {
		if (live.fate[i].needflush == NF_SCRATCH ||
			live.fate[i].status == CLEAN) {
			f_disassociate(i);
		}
	}
	for (i = 0; i<VFREGS; i++) {
		if (live.fate[i].needflush == NF_TOMEM &&
			live.fate[i].status == DIRTY) {
			f_evict(i);
		}
	}
	raw_fp_cleanup_drop();

	/* Now comes the fun part. First, we need to remove all offsets */
	for (i = 0; i<VREGS; i++)
		if (!isconst(i) && live.state[i].val)
			remove_offset(i, -1);

	/* Next, we evict everything that does not end up in registers,
	write back overly dirty registers, and write back constants */
	for (i = 0; i<VREGS; i++) {
		switch (live.state[i].status) {
		case ISCONST:
			if (i != PC_P)
				writeback_const(i);
			break;
		case DIRTY:
			if (vton[i] == -1) {
				evict(i);
				break;
			}
			if (live.state[i].dirtysize>s->nat[vton[i]].dirtysize)
				tomem(i);
			/* Fall-through! */
		case CLEAN:
			if (vton[i] == -1 ||
				live.state[i].validsize<s->nat[vton[i]].validsize)
				evict(i);
			else
				make_exclusive(i, 0, -1);
			break;
		case INMEM:
			break;
		case UNDEF:
			break;
		default:
			write_log(_T("JIT: Weird status: %d\n"), live.state[i].status);
			abort();
		}
	}

	/* Quick consistency check */
	for (i = 0; i<VREGS; i++) {
		if (isinreg(i)) {
			int n = live.state[i].realreg;

			if (live.nat[n].nholds != 1) {
				write_log(_T("JIT: Register %d isn't alone in nreg %d\n"),
					i, n);
				abort();
			}
			if (vton[i] == -1) {
				write_log(_T("JIT: Register %d is still in register, shouldn't be\n"),
					i);
				abort();
			}
		}
	}

	/* Now we need to shuffle things around so the VREGs are in the
	right N_REGs. */
	for (i = 0; i<VREGS; i++) {
		if (isinreg(i) && vton[i] != live.state[i].realreg)
			vinton(i, vton, 0);
	}

	/* And now we may need to load some registers from memory */
	for (i = 0; i<VREGS; i++) {
		int n = vton[i];
		if (n == -1) {
			Dif(isinreg(i)) {
				write_log(_T("JIT: Register %d unexpectedly in nreg %d\n"),
					i, live.state[i].realreg);
				abort();
			}
		}
		else {
			switch (live.state[i].status) {
			case CLEAN:
			case DIRTY:
				Dif(n != live.state[i].realreg)
					abort();
				break;
			case INMEM:
				Dif(live.nat[n].nholds) {
					write_log(_T("JIT: natreg %d holds %d vregs, should be empty\n"),
						n, live.nat[n].nholds);
				}
				raw_mov_l_rm(n, (uae_u32)live.state[i].mem);
				live.state[i].validsize = 4;
				live.state[i].dirtysize = 0;
				live.state[i].realreg = n;
				live.state[i].realind = 0;
				live.state[i].val = 0;
				live.state[i].is_swapped = 0;
				live.nat[n].nholds = 1;
				live.nat[n].holds[0] = i;

				set_status(i, CLEAN);
				break;
			case ISCONST:
				if (i != PC_P) {
					write_log(_T("JIT: Got constant in matchstate for reg %d. Bad!\n"), i);
					abort();
				}
				break;
			case UNDEF:
				break;
			}
		}
	}

	/* One last consistency check, and adjusting the states in live
	to those in s */
	for (i = 0; i<VREGS; i++) {
		int n = vton[i];
		switch (live.state[i].status) {
		case INMEM:
			if (n != -1)
				abort();
			break;
		case ISCONST:
			if (i != PC_P)
				abort();
			break;
		case CLEAN:
		case DIRTY:
			if (n == -1)
				abort();
			if (live.state[i].dirtysize>s->nat[n].dirtysize)
				abort;
			if (live.state[i].validsize<s->nat[n].validsize)
				abort;
			live.state[i].dirtysize = s->nat[n].dirtysize;
			live.state[i].validsize = s->nat[n].validsize;
			if (live.state[i].dirtysize)
				set_status(i, DIRTY);
			break;
		case UNDEF:
			break;
		}
		if (n != -1)
			live.nat[n].touched = touchcnt++;
	}
}
#else
STATIC_INLINE void match_states(smallstate* s)
{
	flush(1);
}
#endif

/* Only do this if you really mean it! The next call should be to init!*/
void flush(int save_regs)
{
	int i;

	log_flush();
	flush_flags(); /* low level */
	sync_m68k_pc(); /* mid level */

	if (save_regs) {
		for (i = 0; i<VFREGS; i++) {
			if (live.fate[i].needflush == NF_SCRATCH ||
				live.fate[i].status == CLEAN) {
				f_disassociate(i);
			}
		}
		for (i = 0; i<VREGS; i++) {
			if (live.state[i].needflush == NF_TOMEM) {
				switch (live.state[i].status) {
				case INMEM:
					if (live.state[i].val) {
						compemu_raw_add_l_mi((uae_u32)live.state[i].mem, live.state[i].val);
						live.state[i].val = 0;
					}
					break;
				case CLEAN:
				case DIRTY:
					remove_offset(i, -1); tomem(i); break;
				case ISCONST:
					if (i != PC_P)
						writeback_const(i);
					break;
				default: break;
				}
				Dif(live.state[i].val && i != PC_P) {
					write_log(_T("JIT: Register %d still has val %x\n"),
						i, live.state[i].val);
				}
			}
		}
		for (i = 0; i<VFREGS; i++) {
			if (live.fate[i].needflush == NF_TOMEM &&
				live.fate[i].status == DIRTY) {
				f_evict(i);
			}
		}
		raw_fp_cleanup_drop();
	}
	if (needflags) {
		write_log(_T("JIT: Warning! flush with needflags=1!\n"));
	}
}

static void flush_keepflags(void)
{
	int i;

	for (i = 0; i<VFREGS; i++) {
		if (live.fate[i].needflush == NF_SCRATCH ||
			live.fate[i].status == CLEAN) {
			f_disassociate(i);
		}
	}
	for (i = 0; i<VREGS; i++) {
		if (live.state[i].needflush == NF_TOMEM) {
			switch (live.state[i].status) {
			case INMEM:
				/* Can't adjust the offset here --- that needs "add" */
				break;
			case CLEAN:
			case DIRTY:
				remove_offset(i, -1); tomem(i); break;
			case ISCONST:
				if (i != PC_P)
					writeback_const(i);
				break;
			default: break;
			}
		}
	}
	for (i = 0; i<VFREGS; i++) {
		if (live.fate[i].needflush == NF_TOMEM &&
			live.fate[i].status == DIRTY) {
			f_evict(i);
		}
	}
	raw_fp_cleanup_drop();
}

void freescratch(void)
{
	int i;
	for (i = 0; i<N_REGS; i++)
		if (live.nat[i].locked && i != 4)
			write_log(_T("JIT: Warning! %d is locked\n"), i);

	for (i = 0; i<VREGS; i++)
		if (live.state[i].needflush == NF_SCRATCH) {
			forget_about(i);
		}

	for (i = 0; i<VFREGS; i++)
		if (live.fate[i].needflush == NF_SCRATCH) {
			f_forget_about(i);
		}
}

/********************************************************************
 * Support functions, internal                                      *
 ********************************************************************/


static void align_target(uae_u32 a)
{
	if (!a)
		return;

	if (tune_nop_fillers)
		raw_emit_nop_filler(a - (uintptr(target) & (a - 1)));
	else {
		/* Fill with NOPs --- makes debugging with gdb easier */
		while ((uintptr)target&(a-1))
			*target++=0x90; // Attention x86 specific code
	}
}

STATIC_INLINE int isinrom(uae_u32 addr)
{
	return (addr >= uae_u32(kickmem_bank.baseaddr) &&
		addr<uae_u32(kickmem_bank.baseaddr) + 8 * 65536);
}

static void flush_all(void)
{
	int i;

	log_flush();
	for (i = 0; i<VREGS; i++)
		if (live.state[i].status == DIRTY) {
			if (!call_saved[live.state[i].realreg]) {
				tomem(i);
			}
		}
	for (i = 0; i<VFREGS; i++)
		if (f_isinreg(i))
			f_evict(i);
	raw_fp_cleanup_drop();
}

/* Make sure all registers that will get clobbered by a call are
   save and sound in memory */
static void prepare_for_call_1(void)
{
    flush_all();  /* If there are registers that don't get clobbered,
		   * we should be a bit more selective here */
}

/* We will call a C routine in a moment. That will clobber all registers,
   so we need to disassociate everything */
static void prepare_for_call_2(void)
{
	int i;
	for (i = 0; i<N_REGS; i++)
		if (!call_saved[i] && live.nat[i].nholds>0)
			free_nreg(i);

	for (i = 0; i<N_FREGS; i++)
		if (live.fat[i].nholds>0)
			f_free_nreg(i);

	live.flags_in_flags = TRASH;  /* Note: We assume we already rescued the
								  flags at the very start of the call_r
								  functions! */
}

/********************************************************************
 * Memory access and related functions, CREATE time                 *
 ********************************************************************/

void register_branch(uae_u32 not_taken, uae_u32 taken, uae_u8 cond)
{
	next_pc_p = not_taken;
	taken_pc_p = taken;
	branch_cc = cond;
}

static uae_u32 get_handler_address(uae_u32 addr)
{
	uae_u32 cl = cacheline(addr);
	blockinfo* bi = get_blockinfo_addr_new(reinterpret_cast<void*>(addr), 0);

#if USE_OPTIMIZER
	if (!bi && reg_alloc_run)
		return 0;
#endif
	return uae_u32(&(bi->direct_handler_to_use));
}

static uae_u32 get_handler(uae_u32 addr)
{
	uae_u32 cl = cacheline(addr);
	blockinfo* bi = get_blockinfo_addr_new(reinterpret_cast<void*>(addr), 0);

#if USE_OPTIMIZER
	if (!bi && reg_alloc_run)
		return 0;
#endif
	return uae_u32(bi->direct_handler_to_use);
}

static void load_handler(int reg, uae_u32 addr)
{
	mov_l_rm(reg, get_handler_address(addr));
}

/* This version assumes that it is writing *real* memory, and *will* fail
 *  if that assumption is wrong! No branches, no second chances, just
 *  straight go-for-it attitude */

static void writemem_real(int address, int source, int size, int tmp, int clobber)
{
    int f=tmp;

#ifdef NATMEM_OFFSET
	if (canbang) {  /* Woohoo! go directly at the memory! */
	if (clobber)
	    f=source;

	switch(size) {
	 case 1: mov_b_bRr(address,source, NATMEM_OFFSETX); break;
	 case 2: mov_w_rr(f,source); mid_bswap_16(f); mov_w_bRr(address,f, NATMEM_OFFSETX); break;
	 case 4: mov_l_rr(f,source); mid_bswap_32(f); mov_l_bRr(address,f, NATMEM_OFFSETX); break;
	}
	forget_about(tmp);
	forget_about(f);
	return;
	}
#endif

}

void writebyte(int address, int source, int tmp)
{
	writemem_real(address,source,1,tmp,0);
}

static inline void writeword_general(int address, int source, int tmp,
					 int clobber)
{
	writemem_real(address,source,2,tmp,clobber);
}

void writeword_clobber(int address, int source, int tmp)
{
    writeword_general(address,source,tmp,1);
}

void writeword(int address, int source, int tmp)
{
    writeword_general(address,source,tmp,0);
}

static inline void writelong_general(int address, int source, int tmp, 
					 int clobber)
{
	writemem_real(address,source,4,tmp,clobber);
}

void writelong_clobber(int address, int source, int tmp)
{
    writelong_general(address,source,tmp,1);
}

void writelong(int address, int source, int tmp)
{
    writelong_general(address,source,tmp,0);
}



/* This version assumes that it is reading *real* memory, and *will* fail
 *  if that assumption is wrong! No branches, no second chances, just
 *  straight go-for-it attitude */

static void readmem_real(int address, int dest, int size, int tmp)
{
    int f=tmp; 

    if (size==4 && address!=dest)
	f=dest;

	switch(size) {
	 case 1: mov_b_brR(dest,address,MEMBaseDiff); break; 
	 case 2: mov_w_brR(dest,address,MEMBaseDiff); mid_bswap_16(dest); break;
	 case 4: mov_l_brR(dest,address,MEMBaseDiff); mid_bswap_32(dest); break;
	}
	forget_about(tmp);
	(void) f;
}

void readbyte(int address, int dest, int tmp)
{
	readmem_real(address,dest,1,tmp);
}

void readword(int address, int dest, int tmp)
{
	readmem_real(address,dest,2,tmp);
}

void readlong(int address, int dest, int tmp)
{
	readmem_real(address,dest,4,tmp);
}

void get_n_addr(int address, int dest, int tmp)
{
	// a is the register containing the virtual address
	// after the offset had been fetched
	int a=tmp;
	
	// f is the register that will contain the offset
	int f=tmp;
	
	// a == f == tmp if (address == dest)
	if (address!=dest) {
	a=address;
	f=dest;
	}

#if FIXED_ADDRESSING
	lea_l_brr(dest,address,MEMBaseDiff);
#else
# error "Only fixed adressing mode supported"
#endif
	forget_about(tmp);
	(void) f;
	(void) a;
}

void get_n_addr_jmp(int address, int dest, int tmp)
{
	/* For this, we need to get the same address as the rest of UAE
	 would --- otherwise we end up translating everything twice */
    get_n_addr(address,dest,tmp);
}


/* base is a register, but dp is an actual value. 
   target is a register, as is tmp */
void calc_disp_ea_020(int base, uae_u32 dp, int target, int tmp)
{
    int reg = (dp >> 12) & 15;
    int regd_shift=(dp >> 9) & 3;

    if (dp & 0x100) {
	int ignorebase=(dp&0x80);
	int ignorereg=(dp&0x40);
	int addbase=0;
	int outer=0;
    
	if ((dp & 0x30) == 0x20) addbase = (uae_s32)(uae_s16)comp_get_iword((m68k_pc_offset+=2)-2);
	if ((dp & 0x30) == 0x30) addbase = comp_get_ilong((m68k_pc_offset+=4)-4);

	if ((dp & 0x3) == 0x2) outer = (uae_s32)(uae_s16)comp_get_iword((m68k_pc_offset+=2)-2);
	if ((dp & 0x3) == 0x3) outer = comp_get_ilong((m68k_pc_offset+=4)-4);

	if ((dp & 0x4) == 0) {  /* add regd *before* the get_long */
	    if (!ignorereg) {
		if ((dp & 0x800) == 0) 
		    sign_extend_16_rr(target,reg);
		else
		    mov_l_rr(target,reg);
		shll_l_ri(target,regd_shift);
	    }
	    else
		mov_l_ri(target,0);

	    /* target is now regd */
	    if (!ignorebase)
		add_l(target,base);
	    add_l_ri(target,addbase);
	    if (dp&0x03) readlong(target,target,tmp);
	} else { /* do the getlong first, then add regd */
	    if (!ignorebase) {
		mov_l_rr(target,base);
		add_l_ri(target,addbase);
	    }
	    else
		mov_l_ri(target,addbase);
	    if (dp&0x03) readlong(target,target,tmp);

	    if (!ignorereg) {
		if ((dp & 0x800) == 0) 
		    sign_extend_16_rr(tmp,reg);
		else
		    mov_l_rr(tmp,reg);
		shll_l_ri(tmp,regd_shift);
		/* tmp is now regd */
		add_l(target,tmp);
	    }
	}
	add_l_ri(target,outer);
    }
    else { /* 68000 version */
	if ((dp & 0x800) == 0) { /* Sign extend */
	    sign_extend_16_rr(target,reg);
	    lea_l_brr_indexed(target,base,target,1<<regd_shift,(uae_s32)((uae_s8)dp));
	}
	else {
	    lea_l_brr_indexed(target,base,reg,1<<regd_shift,(uae_s32)((uae_s8)dp));
	}
    }
    forget_about(tmp);
}





void set_cache_state(int enabled)
{
    if (enabled!=letit)
	flush_icache_hard(77);
    letit=enabled;
}

int get_cache_state(void)
{
    return letit;
}

uae_u32 get_jitted_size(void)
{
    if (compiled_code)
	return current_compile_p-compiled_code;
    return 0;
}

const int CODE_ALLOC_MAX_ATTEMPTS = 10;
const int CODE_ALLOC_BOUNDARIES   = 128 * 1024; // 128 KB

static uint8 *do_alloc_code(uint32 size, int depth)
{
#if defined(__linux__) && 0
	/*
	  This is a really awful hack that is known to work on Linux at
	  least.
	  
	  The trick here is to make sure the allocated cache is nearby
	  code segment, and more precisely in the positive half of a
	  32-bit address space. i.e. addr < 0x80000000. Actually, it
	  turned out that a 32-bit binary run on AMD64 yields a cache
	  allocated around 0xa0000000, thus causing some troubles when
	  translating addresses from m68k to x86.
	*/
	static uint8 * code_base = NULL;
	if (code_base == NULL) {
		uintptr page_size = getpagesize();
		uintptr boundaries = CODE_ALLOC_BOUNDARIES;
		if (boundaries < page_size)
			boundaries = page_size;
		code_base = (uint8 *)sbrk(0);
		for (int attempts = 0; attempts < CODE_ALLOC_MAX_ATTEMPTS; attempts++) {
			if (vm_acquire_fixed(code_base, size) == 0) {
				uint8 *code = code_base;
				code_base += size;
				return code;
			}
			code_base += boundaries;
		}
		return NULL;
	}

	if (vm_acquire_fixed(code_base, size) == 0) {
		uint8 *code = code_base;
		code_base += size;
		return code;
	}

	if (depth >= CODE_ALLOC_MAX_ATTEMPTS)
		return NULL;

	return do_alloc_code(size, depth + 1);
#else
	UNUSED(depth);
	uint8 *code = (uint8 *)vm_acquire(size, VM_MAP_DEFAULT | VM_MAP_32BIT);
	return code == VM_MAP_FAILED ? NULL : code;
#endif
}

static inline uint8 *alloc_code(uint32 size)
{
	uint8 *ptr = do_alloc_code(size, 0);
	/* allocated code must fit in 32-bit boundaries */
	assert((uintptr)ptr <= 0xffffffff);
	return ptr;
}

void alloc_cache(void)
{
	if (compiled_code) {
		flush_icache_hard(6);
		vm_release(compiled_code, cache_size * 1024);
		compiled_code = 0;
	}
	
	if (cache_size == 0)
		return;
	
	while (!compiled_code && cache_size) {
		if ((compiled_code = alloc_code(cache_size * 1024)) == NULL) {
			compiled_code = 0;
			cache_size /= 2;
		}
	}
	vm_protect(compiled_code, cache_size * 1024, VM_PAGE_READ | VM_PAGE_WRITE | VM_PAGE_EXECUTE);
	
	if (compiled_code) {
		D(bug("<JIT compiler> : actual translation cache size : %d KB at %p-%p", cache_size, compiled_code, compiled_code + cache_size*1024));
		max_compile_start = compiled_code + cache_size*1024 - BYTES_PER_INST;
		current_compile_p = compiled_code;
		current_cache_size = 0;
#if defined(USE_DATA_BUFFER)
		data_target = data_ptr = compiled_code + cache_size * 1024;
#endif
	}
}



extern void op_illg_1 (uae_u32 opcode) REGPARAM;

static void calc_checksum(blockinfo* bi, uae_u32* c1, uae_u32* c2)
{
	uae_u32 k1 = 0;
	uae_u32 k2 = 0;

#if USE_CHECKSUM_INFO
	checksum_info *csi = bi->csi;
	Dif(!csi) abort();
	while (csi) {
		uae_s32 len = csi->length;
		uintptr tmp = (uintptr)csi->start_p;
#else
		uae_s32 len = bi->len;
		uintptr tmp = (uintptr)bi->min_pcp;
#endif
		uae_u32*pos;

		len += (tmp & 3);
		tmp &= ~((uintptr)3);
		pos = (uae_u32 *)tmp;

		if (len >= 0 && len <= MAX_CHECKSUM_LEN) {
			while (len > 0) {
				k1 += *pos;
				k2 ^= *pos;
				pos++;
				len -= 4;
			}
		}

#if USE_CHECKSUM_INFO
		csi = csi->next;
	}
#endif

	*c1 = k1;
	*c2 = k2;
}

#if 0
static void show_checksum(CSI_TYPE* csi)
{
    uae_u32 k1=0;
    uae_u32 k2=0;
    uae_s32 len=CSI_LENGTH(csi);
    uae_u32 tmp=(uintptr)CSI_START_P(csi);
    uae_u32* pos;

    len+=(tmp&3);
    tmp&=(~3);
    pos=(uae_u32*)tmp;

    if (len<0 || len>MAX_CHECKSUM_LEN) {
	return;
    }
    else {
	while (len>0) {
	    D(panicbug("%08x ",*pos));
	    pos++;
	    len-=4;
	}
	D(panicbug(" bla"));
    }
}
#endif


int check_for_cache_miss(void)
{
    blockinfo* bi=get_blockinfo_addr(regs.pc_p);
    
    if (bi) {
	int cl=cacheline(regs.pc_p);
	if (bi!=cache_tags[cl+1].bi) {
	    raise_in_cl_list(bi);
	    return 1;
	}
    }
    return 0;
}

    
static void recompile_block(void)
{
    /* An existing block's countdown code has expired. We need to make
       sure that execute_normal doesn't refuse to recompile due to a
       perceived cache miss... */
    blockinfo*  bi=get_blockinfo_addr(regs.pc_p);

    Dif (!bi) 
	abort();
    raise_in_cl_list(bi);
    execute_normal();
    return;
}
static void cache_miss(void)
{
    blockinfo*  bi=get_blockinfo_addr(regs.pc_p);
    uae_u32     cl=cacheline(regs.pc_p);
    blockinfo*  bi2=get_blockinfo(cl);

    if (!bi) {
	execute_normal(); /* Compile this block now */
	return;
    }
    Dif (!bi2 || bi==bi2) {
	D(panicbug("Unexplained cache miss %p %p",bi,bi2));
	abort();
    }
    raise_in_cl_list(bi);
    return;
}

static int called_check_checksum(blockinfo* bi);

static inline int block_check_checksum(blockinfo* bi) 
{
    uae_u32     c1,c2;
    bool        isgood;
    
    if (bi->status!=BI_NEED_CHECK)
	return 1;  /* This block is in a checked state */
    
    checksum_count++;

    if (bi->c1 || bi->c2)
	calc_checksum(bi,&c1,&c2);
    else {
	c1=c2=1;  /* Make sure it doesn't match */
    }

    isgood=(c1==bi->c1 && c2==bi->c2);

    if (isgood) { 
	/* This block is still OK. So we reactivate. Of course, that
	   means we have to move it into the needs-to-be-flushed list */
	bi->handler_to_use=bi->handler;
	set_dhtu(bi,bi->direct_handler);
	bi->status=BI_CHECKING;
	isgood=called_check_checksum(bi);
    }
    if (isgood) {
	D2(bug("reactivate %p/%p (%x %x/%x %x)",bi,bi->pc_p, c1,c2,bi->c1,bi->c2));
	remove_from_list(bi);
	add_to_active(bi);
	raise_in_cl_list(bi);
	bi->status=BI_ACTIVE;
    }
    else {
	/* This block actually changed. We need to invalidate it,
	   and set it up to be recompiled */
	D2(bug("discard %p/%p (%x %x/%x %x)",bi,bi->pc_p, c1,c2,bi->c1,bi->c2));
	invalidate_block(bi);
	raise_in_cl_list(bi);
    }
    return isgood;
}

static int called_check_checksum(blockinfo* bi) 
{
    int isgood=1;
    int i;
    
    for (i=0;i<2 && isgood;i++) {
	if (bi->dep[i].jmp_off) {
	    isgood=block_check_checksum(bi->dep[i].target);
	}
    }
    return isgood;
}

static void check_checksum(void) 
{
    blockinfo*  bi=get_blockinfo_addr(regs.pc_p);
    uae_u32     cl=cacheline(regs.pc_p);
    blockinfo*  bi2=get_blockinfo(cl);

    /* These are not the droids you are looking for...  */
    if (!bi) {
	/* Whoever is the primary target is in a dormant state, but
	   calling it was accidental, and we should just compile this
	   new block */
	execute_normal();
	return;
    }
    if (bi!=bi2) {
	/* The block was hit accidentally, but it does exist. Cache miss */
	cache_miss();
	return;
    }

    if (!block_check_checksum(bi))
	execute_normal();
}

static inline void match_states(blockinfo* bi)
{
    int i;
    smallstate* s=&(bi->env);
    
    if (bi->status==BI_NEED_CHECK) {
	block_check_checksum(bi);
    }
    if (bi->status==BI_ACTIVE || 
	bi->status==BI_FINALIZING) {  /* Deal with the *promises* the 
					 block makes (about not using 
					 certain vregs) */
	for (i=0;i<16;i++) {
	    if (s->virt[i]==L_UNNEEDED) {
		D2(panicbug("unneeded reg %d at %p",i,target));
		COMPCALL(forget_about)(i); // FIXME
	    }
	}
    }
    flush(1);

    /* And now deal with the *demands* the block makes */
    for (i=0;i<N_REGS;i++) {
	int v=s->nat[i];
	if (v>=0) {
	    // printf("Loading reg %d into %d at %p\n",v,i,target);
	    readreg_specific(v,4,i);
	    // do_load_reg(i,v);
	    // setlock(i);
	}
    }
    for (i=0;i<N_REGS;i++) {
	int v=s->nat[i];
	if (v>=0) {
	    unlock2(i);
	}
    }
}

static inline void create_popalls(void)
{
  int i,r;

  if ((popallspace = alloc_code(POPALLSPACE_SIZE)) == NULL) {
	  panicbug("FATAL: Could not allocate popallspace!");
	  abort();
  }
  vm_protect(popallspace, POPALLSPACE_SIZE, VM_PAGE_READ | VM_PAGE_WRITE);

  int stack_space = STACK_OFFSET;
  for (i=0;i<N_REGS;i++) {
	  if (need_to_preserve[i])
		  stack_space += sizeof(void *);
  }
  stack_space %= STACK_ALIGN;
  if (stack_space)
	  stack_space = STACK_ALIGN - stack_space;

  current_compile_p=popallspace;
  set_target(current_compile_p);

  /* We need to guarantee 16-byte stack alignment on x86 at any point
     within the JIT generated code. We have multiple exit points
     possible but a single entry. A "jmp" is used so that we don't
     have to generate stack alignment in generated code that has to
     call external functions (e.g. a generic instruction handler).

     In summary, JIT generated code is not leaf so we have to deal
     with it here to maintain correct stack alignment. */
  align_target(align_jumps);
  current_compile_p=get_target();
  pushall_call_handler=get_target();
  raw_push_regs_to_preserve();
  raw_dec_sp(stack_space);
  r=REG_PC_TMP;
  compemu_raw_mov_l_rm(r,(uintptr)&regs.pc_p);
  compemu_raw_and_l_ri(r,TAGMASK);
  compemu_raw_jmp_m_indexed((uintptr)cache_tags,r,SIZEOF_VOID_P);

  /* now the exit points */
  align_target(align_jumps);
  popall_do_nothing=get_target();
  raw_inc_sp(stack_space);
  raw_pop_preserved_regs();
  compemu_raw_jmp((uintptr)do_nothing);
  
  align_target(align_jumps);
  popall_execute_normal=get_target();
  raw_inc_sp(stack_space);
  raw_pop_preserved_regs();
  compemu_raw_jmp((uintptr)execute_normal);

  align_target(align_jumps);
  popall_cache_miss=get_target();
  raw_inc_sp(stack_space);
  raw_pop_preserved_regs();
  compemu_raw_jmp((uintptr)cache_miss);

  align_target(align_jumps);
  popall_recompile_block=get_target();
  raw_inc_sp(stack_space);
  raw_pop_preserved_regs();
  compemu_raw_jmp((uintptr)recompile_block);

  align_target(align_jumps);
  popall_exec_nostats=get_target();
  raw_inc_sp(stack_space);
  raw_pop_preserved_regs();
  compemu_raw_jmp((uintptr)exec_nostats);

  align_target(align_jumps);
  popall_check_checksum=get_target();
  raw_inc_sp(stack_space);
  raw_pop_preserved_regs();
  compemu_raw_jmp((uintptr)check_checksum);

  // no need to further write into popallspace
  vm_protect(popallspace, POPALLSPACE_SIZE, VM_PAGE_READ | VM_PAGE_EXECUTE);
  // No need to flush. Initialized and not modified
  // flush_cpu_icache((void *)popallspace, (void *)target);
}

static inline void reset_lists(void)
{
    int i;
    
    for (i=0;i<MAX_HOLD_BI;i++)
	hold_bi[i]=NULL;
    active=NULL;
    dormant=NULL;
}

static void prepare_block(blockinfo* bi)
{
    int i;

    set_target(current_compile_p);
    align_target(align_jumps);
    bi->direct_pen=(cpuop_func *)get_target();
    compemu_raw_mov_l_rm(0,(uintptr)&(bi->pc_p));
    compemu_raw_mov_l_mr((uintptr)&regs.pc_p,0);
    compemu_raw_jmp((uintptr)popall_execute_normal);

    align_target(align_jumps);
    bi->direct_pcc=(cpuop_func *)get_target();
    compemu_raw_mov_l_rm(0,(uintptr)&(bi->pc_p));
    compemu_raw_mov_l_mr((uintptr)&regs.pc_p,0);
    compemu_raw_jmp((uintptr)popall_check_checksum);
    flush_cpu_icache((void *)current_compile_p, (void *)target);
    current_compile_p=get_target();

    bi->deplist=NULL;
    for (i=0;i<2;i++) {
	bi->dep[i].prev_p=NULL;
	bi->dep[i].next=NULL;
    }
    bi->env=default_ss;
    bi->status=BI_INVALID;
    bi->havestate=0;
    //bi->env=empty_ss;
}

// OPCODE is in big endian format, use cft_map() beforehand, if needed.
static inline void reset_compop(int opcode)
{
	compfunctbl[opcode] = NULL;
	nfcompfunctbl[opcode] = NULL;
}

static int read_opcode(const char *p)
{
	int opcode = 0;
	for (int i = 0; i < 4; i++) {
		int op = p[i];
		switch (op) {
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
			opcode = (opcode << 4) | (op - '0');
			break;
		case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
			opcode = (opcode << 4) | ((op - 'a') + 10);
			break;
		case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
			opcode = (opcode << 4) | ((op - 'A') + 10);
			break;
		default:
			return -1;
		}
	}
	return opcode;
}

static bool merge_blacklist()
{
	const char *blacklist = bx_options.jit.jitblacklist;
	if (blacklist[0] != '\0') {
		const char *p = blacklist;
		for (;;) {
			if (*p == 0)
				return true;

			int opcode1 = read_opcode(p);
			if (opcode1 < 0)
				return false;
			p += 4;

			int opcode2 = opcode1;
			if (*p == '-') {
				p++;
				opcode2 = read_opcode(p);
				if (opcode2 < 0)
					return false;
				p += 4;
			}

			if (*p == 0 || *p == ',') {
				D(bug("<JIT compiler> : blacklist opcodes : %04x-%04x\n", opcode1, opcode2));
				for (int opcode = opcode1; opcode <= opcode2; opcode++)
					reset_compop(cft_map(opcode));

				if (*(p++) == ',')
					continue;

				return true;
			}

			return false;
		}
	}
	return true;
}

void build_comp(void) 
{
	int i;
    unsigned long opcode;
    struct comptbl* tbl=op_smalltbl_0_comp_ff;
    struct comptbl* nftbl=op_smalltbl_0_comp_nf;
    int count;
	unsigned int cpu_level = 4;			// 68040
    struct cputbl *nfctbl = op_smalltbl_0_nf;

#ifdef NATMEM_OFFSET
    signal(SIGSEGV, (sighandler_t)segfault_vec);
    D(bug("<JIT compiler> : NATMEM OFFSET handler installed"));
#endif

    D(bug("<JIT compiler> : building compiler function tables"));
	
	for (opcode = 0; opcode < 65536; opcode++) {
		reset_compop(opcode);
		nfcpufunctbl[opcode] = op_illg_1;
		prop[opcode].use_flags = 0x1f;
		prop[opcode].set_flags = 0x1f;
		prop[opcode].cflow = fl_trap; // ILLEGAL instructions do trap
	}
	
	for (i = 0; tbl[i].opcode < 65536; i++) {
		int cflow = table68k[tbl[i].opcode].cflow;
		if (follow_const_jumps && (tbl[i].specific & 16))
			cflow = fl_const_jump;
		else
			cflow &= ~fl_const_jump;
		prop[cft_map(tbl[i].opcode)].cflow = cflow;

		int uses_fpu = tbl[i].specific & 32;
		if (uses_fpu && avoid_fpu)
			compfunctbl[cft_map(tbl[i].opcode)] = NULL;
		else
			compfunctbl[cft_map(tbl[i].opcode)] = tbl[i].handler;
	}

    for (i = 0; nftbl[i].opcode < 65536; i++) {
		int uses_fpu = tbl[i].specific & 32;
		if (uses_fpu && avoid_fpu)
			nfcompfunctbl[cft_map(nftbl[i].opcode)] = NULL;
		else
			nfcompfunctbl[cft_map(nftbl[i].opcode)] = nftbl[i].handler;
		
		nfcpufunctbl[cft_map(nftbl[i].opcode)] = nfctbl[i].handler;
    }

	for (i = 0; nfctbl[i].handler; i++) {
		nfcpufunctbl[cft_map(nfctbl[i].opcode)] = nfctbl[i].handler;
	}

    for (opcode = 0; opcode < 65536; opcode++) {
		compop_func *f;
		compop_func *nff;
		cpuop_func *nfcf;
		int isaddx,cflow;

		if ((instrmnem)table68k[opcode].mnemo == i_ILLG || table68k[opcode].clev > cpu_level)
			continue;

		if (table68k[opcode].handler != -1) {
			f = compfunctbl[cft_map(table68k[opcode].handler)];
			nff = nfcompfunctbl[cft_map(table68k[opcode].handler)];
			nfcf = nfcpufunctbl[cft_map(table68k[opcode].handler)];
			cflow = prop[cft_map(table68k[opcode].handler)].cflow;
			isaddx = prop[cft_map(table68k[opcode].handler)].is_addx;
			prop[cft_map(opcode)].cflow = cflow;
			prop[cft_map(opcode)].is_addx = isaddx;
			compfunctbl[cft_map(opcode)] = f;
			nfcompfunctbl[cft_map(opcode)] = nff;
			Dif (nfcf == op_illg_1)
			abort();
			nfcpufunctbl[cft_map(opcode)] = nfcf;
		}
		prop[cft_map(opcode)].set_flags = table68k[opcode].flagdead;
		prop[cft_map(opcode)].use_flags = table68k[opcode].flaglive;
		/* Unconditional jumps don't evaluate condition codes, so they
		 * don't actually use any flags themselves */
		if (prop[cft_map(opcode)].cflow & fl_const_jump)
			prop[cft_map(opcode)].use_flags = 0;
    }
	for (i = 0; nfctbl[i].handler != NULL; i++) {
		if (nfctbl[i].specific)
			nfcpufunctbl[cft_map(tbl[i].opcode)] = nfctbl[i].handler;
	}

    /* Merge in blacklist */
    if (!merge_blacklist())
	panicbug("<JIT compiler> : blacklist merge failure!\n");

    count=0;
    for (opcode = 0; opcode < 65536; opcode++) {
	if (compfunctbl[cft_map(opcode)])
	    count++;
    }
	D(bug("<JIT compiler> : supposedly %d compileable opcodes!",count));

    /* Initialise state */
    create_popalls();
    alloc_cache();
    reset_lists();

    for (i=0;i<TAGSIZE;i+=2) {
	cache_tags[i].handler=(cpuop_func *)popall_execute_normal;
	cache_tags[i+1].bi=NULL;
    }
    
#if 0
    for (i=0;i<N_REGS;i++) {
	empty_ss.nat[i].holds=-1;
	empty_ss.nat[i].validsize=0;
	empty_ss.nat[i].dirtysize=0;
    }
#endif
    for (i=0;i<VREGS;i++) {
	empty_ss.virt[i]=L_NEEDED;
    }
    for (i=0;i<N_REGS;i++) {
	empty_ss.nat[i]=L_UNKNOWN;
    }
    default_ss=empty_ss;
}


static void flush_icache_none(int)
{
	/* Nothing to do.  */
}

static void flush_icache_hard(int n)
{
    blockinfo* bi, *dbi;

    hard_flush_count++;
    D(bug("Flush Icache_hard(%d/%x/%p), %u KB",
	   n,regs.pc,regs.pc_p,current_cache_size/1024));
	UNUSED(n);
#if 0
	current_cache_size = 0;
#endif
    bi=active;
    while(bi) {
	cache_tags[cacheline(bi->pc_p)].handler=(cpuop_func *)popall_execute_normal;
	cache_tags[cacheline(bi->pc_p)+1].bi=NULL;
	dbi=bi; bi=bi->next;
	free_blockinfo(dbi);
    }
    bi=dormant;
    while(bi) {
	cache_tags[cacheline(bi->pc_p)].handler=(cpuop_func *)popall_execute_normal;
	cache_tags[cacheline(bi->pc_p)+1].bi=NULL;
	dbi=bi; bi=bi->next;
	free_blockinfo(dbi);
    }

    reset_lists();
    if (!compiled_code)
	return;

#if defined(USE_DATA_BUFFER)
    reset_data_buffer();
#endif

    current_compile_p=compiled_code;

    SPCFLAGS_SET( SPCFLAG_JIT_EXEC_RETURN ); /* To get out of compiled code */
}


/* "Soft flushing" --- instead of actually throwing everything away,
   we simply mark everything as "needs to be checked". 
*/

static inline void flush_icache_lazy(int)
{
    blockinfo* bi;
    blockinfo* bi2;

        soft_flush_count++;
	if (!active)
	    return;

	bi=active;
	while (bi) {
	    uae_u32 cl=cacheline(bi->pc_p);
		if (bi->status==BI_INVALID ||
			bi->status==BI_NEED_RECOMP) { 
		if (bi==cache_tags[cl+1].bi) 
		    cache_tags[cl].handler=(cpuop_func *)popall_execute_normal;
		bi->handler_to_use=(cpuop_func *)popall_execute_normal;
		set_dhtu(bi,bi->direct_pen);
	    bi->status=BI_INVALID;
	    }
	    else {
		if (bi==cache_tags[cl+1].bi) 
		    cache_tags[cl].handler=(cpuop_func *)popall_check_checksum;
		bi->handler_to_use=(cpuop_func *)popall_check_checksum;
		set_dhtu(bi,bi->direct_pcc);
		bi->status=BI_NEED_CHECK;
	    }
	    bi2=bi;
	    bi=bi->next;
	}
	/* bi2 is now the last entry in the active list */
	bi2->next=dormant;
	if (dormant)
	    dormant->prev_p=&(bi2->next);
    
	dormant=active;
	active->prev_p=&dormant;
	active=NULL;
}

void flush_icache_range(uae_u32 start, uae_u32 length)
{
	if (!active)
		return;

#if LAZY_FLUSH_ICACHE_RANGE
	uae_u8 *start_p = get_real_address(start);
	blockinfo *bi = active;
	while (bi) {
#if USE_CHECKSUM_INFO
		bool invalidate = false;
		for (checksum_info *csi = bi->csi; csi && !invalidate; csi = csi->next)
			invalidate = (((start_p - csi->start_p) < csi->length) ||
						  ((csi->start_p - start_p) < length));
#else
		// Assume system is consistent and would invalidate the right range
		const bool invalidate = (bi->pc_p - start_p) < length;
#endif
		if (invalidate) {
			uae_u32 cl = cacheline(bi->pc_p);
			if (bi == cache_tags[cl + 1].bi)
					cache_tags[cl].handler = (cpuop_func *)popall_execute_normal;
			bi->handler_to_use = (cpuop_func *)popall_execute_normal;
			set_dhtu(bi, bi->direct_pen);
			bi->status = BI_NEED_RECOMP;
		}
		bi = bi->next;
	}
	return;
#else
		UNUSED(start);
		UNUSED(length);
#endif
	flush_icache(-1);
}

/*
static void catastrophe(void)
{
    abort();
}
*/

int failure;

#define TARGET_M68K		0
#define TARGET_POWERPC	1
#define TARGET_X86		2
#define TARGET_X86_64	3
#define TARGET_ARM		4
#if defined(CPU_i386)
#define TARGET_NATIVE	TARGET_X86
#endif
#if defined(CPU_powerpc)
#define TARGET_NATIVE	TARGET_POWERPC
#endif
#if defined(CPU_x86_64)
#define TARGET_NATIVE	TARGET_X86_64
#endif
#if defined(CPU_arm)
#define TARGET_NATIVE	TARGET_ARM
#endif

void disasm_block(int /* target */, uint8 * /* start */, size_t /* length */)
{
	if (!JITDebug)
		return;
}

static inline void disasm_native_block(uint8 *start, size_t length)
{
	disasm_block(TARGET_NATIVE, start, length);
}

static inline void disasm_m68k_block(uint8 *start, size_t length)
{
	disasm_block(TARGET_M68K, start, length);
}

#ifdef HAVE_GET_WORD_UNSWAPPED
# define DO_GET_OPCODE(a) (do_get_mem_word_unswapped((uae_u16 *)(a)))
#else
# define DO_GET_OPCODE(a) (do_get_mem_word((uae_u16 *)(a)))
#endif

#ifdef JIT_DEBUG
static uae_u8 *last_regs_pc_p = 0;
static uae_u8 *last_compiled_block_addr = 0;

void compiler_dumpstate(void)
{
	if (!JITDebug)
		return;
	
	bug("### Host addresses");
	bug("MEM_BASE    : %x", MEMBaseDiff);
	bug("PC_P        : %p", &regs.pc_p);
	bug("SPCFLAGS    : %p", &regs.spcflags);
	bug("D0-D7       : %p-%p", &regs.regs[0], &regs.regs[7]);
	bug("A0-A7       : %p-%p", &regs.regs[8], &regs.regs[15]);
	bug("");
	
	bug("### M68k processor state");
	m68k_dumpstate(stderr, 0);
	bug("");
	
	bug("### Block in Atari address space");
	bug("M68K block   : %p",
			  (void *)(uintptr)last_regs_pc_p);
	if (last_regs_pc_p != 0) {
		bug("Native block : %p (%d bytes)",
			  (void *)last_compiled_block_addr,
			  get_blockinfo_addr(last_regs_pc_p)->direct_handler_size);
	}
	bug("");
}
#endif

static void compile_block(cpu_history* pc_hist, int blocklen)
{
    if (letit && compiled_code) {
#ifdef PROFILE_COMPILE_TIME
	compile_count++;
	clock_t start_time = clock();
#endif
#ifdef JIT_DEBUG
	bool disasm_block = false;
#endif
	
	/* OK, here we need to 'compile' a block */
	int i;
	int r;
	int was_comp=0;
	uae_u8 liveflags[MAXRUN+1];
#if USE_CHECKSUM_INFO
	bool trace_in_rom = isinrom((uintptr)pc_hist[0].location);
	uintptr max_pcp=(uintptr)pc_hist[blocklen - 1].location;
	uintptr min_pcp=max_pcp;
#else
	uintptr max_pcp=(uintptr)pc_hist[0].location;
	uintptr min_pcp=max_pcp;
#endif
	uae_u32 cl=cacheline(pc_hist[0].location);
	void* specflags=(void*)&regs.spcflags;
	blockinfo* bi=NULL;
	blockinfo* bi2;
	int extra_len=0;

	redo_current_block=0;
	if (current_compile_p>=MAX_COMPILE_PTR)
	    flush_icache_hard(7);

	alloc_blockinfos();

	bi=get_blockinfo_addr_new(pc_hist[0].location,0);
	bi2=get_blockinfo(cl);

	optlev=bi->optlevel;
	if (bi->status!=BI_INVALID) {
	    Dif (bi!=bi2) { 
		/* I don't think it can happen anymore. Shouldn't, in 
		   any case. So let's make sure... */
		panicbug("WOOOWOO count=%d, ol=%d %p %p", bi->count,bi->optlevel,bi->handler_to_use, cache_tags[cl].handler);
		abort();
	    }

	    Dif (bi->count!=-1 && bi->status!=BI_NEED_RECOMP) {
		panicbug("bi->count=%d, bi->status=%d,bi->optlevel=%d",bi->count,bi->status,bi->optlevel);
		/* What the heck? We are not supposed to be here! */
		abort();
	    }
	}	
	if (bi->count==-1) {
	    optlev++;
	    while (!optcount[optlev])
		optlev++;
	    bi->count=optcount[optlev]-1;
	}
	current_block_pc_p=(uintptr)pc_hist[0].location;
	
	remove_deps(bi); /* We are about to create new code */
	bi->optlevel=optlev;
	bi->pc_p=(uae_u8*)pc_hist[0].location;
#if USE_CHECKSUM_INFO
	free_checksum_info_chain(bi->csi);
	bi->csi = NULL;
#endif
	
	liveflags[blocklen]=0x1f; /* All flags needed afterwards */
	i=blocklen;
	while (i--) {
	    uae_u16* currpcp=pc_hist[i].location;
	    uae_u32 op=DO_GET_OPCODE(currpcp);

#if USE_CHECKSUM_INFO
		trace_in_rom = trace_in_rom && isinrom((uintptr)currpcp);
		if (follow_const_jumps && is_const_jump(op)) {
			checksum_info *csi = alloc_checksum_info();
			csi->start_p = (uae_u8 *)min_pcp;
			csi->length = max_pcp - min_pcp + LONGEST_68K_INST;
			csi->next = bi->csi;
			bi->csi = csi;
			max_pcp = (uintptr)currpcp;
		}
		min_pcp = (uintptr)currpcp;
#else
	    if ((uintptr)currpcp<min_pcp)
		min_pcp=(uintptr)currpcp;
	    if ((uintptr)currpcp>max_pcp)
		max_pcp=(uintptr)currpcp;
#endif

		liveflags[i]=((liveflags[i+1]&
			       (~prop[op].set_flags))|
			      prop[op].use_flags);
		if (prop[op].is_addx && (liveflags[i+1]&FLAG_Z)==0)
		    liveflags[i]&= ~FLAG_Z;
	}

#if USE_CHECKSUM_INFO
	checksum_info *csi = alloc_checksum_info();
	csi->start_p = (uae_u8 *)min_pcp;
	csi->length = max_pcp - min_pcp + LONGEST_68K_INST;
	csi->next = bi->csi;
	bi->csi = csi;
#endif

	bi->needed_flags=liveflags[0];

	align_target(align_loops);
	was_comp=0;

	bi->direct_handler=(cpuop_func *)get_target();
	set_dhtu(bi,bi->direct_handler);
	bi->status=BI_COMPILING;
	current_block_start_target=(uintptr)get_target();

	log_startblock();

#if defined(USE_DATA_BUFFER)
	set_data_start();
	raw_init_dp();
#endif
	
	if (bi->count>=0) { /* Need to generate countdown code */
	    compemu_raw_mov_l_mi((uintptr)&regs.pc_p,(uintptr)pc_hist[0].location);
	    compemu_raw_sub_l_mi((uintptr)&(bi->count),1);
	    compemu_raw_jl((uintptr)popall_recompile_block);
	}
	if (optlev==0) { /* No need to actually translate */
	    /* Execute normally without keeping stats */
		compemu_raw_mov_l_mi((uintptr)&regs.pc_p,(uintptr)pc_hist[0].location);
		compemu_raw_jmp((uintptr)popall_exec_nostats);
	}
	else {
	    reg_alloc_run=0;
	    next_pc_p=0;
	    taken_pc_p=0;
	    branch_cc=0; // Only to be initialized. Will be set together with next_pc_p
		
	    comp_pc_p=(uae_u8*)pc_hist[0].location;
	    init_comp();
	    was_comp=1;

#ifdef USE_CPU_EMUL_SERVICES
	    compemu_raw_sub_l_mi((uintptr)&emulated_ticks,blocklen);
	    compemu_raw_jcc_b_oponly(NATIVE_CC_GT);
	    uae_s8 *branchadd=(uae_s8*)get_target();
	    emit_byte(0);
	    compemu_raw_call((uintptr)cpu_do_check_ticks);
	    *branchadd=(uintptr)get_target()-((uintptr)branchadd+1);
#endif

#ifdef JIT_DEBUG
		if (JITDebug) {
			compemu_raw_mov_l_mi((uintptr)&last_regs_pc_p,(uintptr)pc_hist[0].location);
			compemu_raw_mov_l_mi((uintptr)&last_compiled_block_addr,current_block_start_target);
		}
#endif
		
	    for (i=0;i<blocklen &&
		     get_target_noopt()<MAX_COMPILE_PTR;i++) {
		cpuop_func **cputbl;
		compop_func **comptbl;
		uae_u32 opcode=DO_GET_OPCODE(pc_hist[i].location);
		needed_flags=(liveflags[i+1] & prop[opcode].set_flags);
		if (!needed_flags) {
		    cputbl=nfcpufunctbl;
		    comptbl=nfcompfunctbl;
		}
		else {
		    cputbl=cpufunctbl;
		    comptbl=compfunctbl;
		}

#ifdef FLIGHT_RECORDER
		{
		    mov_l_ri(S1, ((uintptr)(pc_hist[i].location)) | 1);
		    /* store also opcode to second register */
		    clobber_flags();
		    remove_all_offsets();
		    int arg = readreg_specific(S1,4,REG_PAR1);
		    prepare_for_call_1();
		    unlock2(arg);
		    prepare_for_call_2();
		    compemu_raw_call((uintptr)m68k_record_step);
		}
#endif
		
		failure = 1; // gb-- defaults to failure state
		if (comptbl[opcode] && optlev>1) { 
		    failure=0;
		    if (!was_comp) {
			comp_pc_p=(uae_u8*)pc_hist[i].location;
			init_comp();
		    }
		    was_comp=1;

		    comptbl[opcode](opcode);
		    freescratch();
		    if (!(liveflags[i+1] & FLAG_CZNV)) { 
			/* We can forget about flags */
			dont_care_flags();
		    }
#if INDIVIDUAL_INST 
		    flush(1);
		    nop();
		    flush(1);
		    was_comp=0;
#endif
		}
		
		if (failure) {
		    if (was_comp) {
			flush(1);
			was_comp=0;
		    }
		    compemu_raw_mov_l_ri(REG_PAR1,(uae_u32)opcode);
#if USE_NORMAL_CALLING_CONVENTION
		    raw_push_l_r(REG_PAR1);
#endif
		    compemu_raw_mov_l_mi((uintptr)&regs.pc_p,
				 (uintptr)pc_hist[i].location);
		    compemu_raw_call((uintptr)cputbl[opcode]);
#ifdef PROFILE_UNTRANSLATED_INSNS
			// raw_cputbl_count[] is indexed with plain opcode (in m68k order)
		    compemu_raw_add_l_mi((uintptr)&raw_cputbl_count[cft_map(opcode)],1);
#endif
#if USE_NORMAL_CALLING_CONVENTION
		    raw_inc_sp(4);
#endif
		    
		    if (i < blocklen - 1) {
			uae_s8* branchadd;
			
			compemu_raw_mov_l_rm(0,(uintptr)specflags);
			compemu_raw_test_l_rr(0,0);
			compemu_raw_jz_b_oponly();
			branchadd=(uae_s8 *)get_target();
			emit_byte(0);
			compemu_raw_jmp((uintptr)popall_do_nothing);
			*branchadd=(uintptr)get_target()-(uintptr)branchadd-1;
		    }
		}
	    }
#if 1 /* This isn't completely kosher yet; It really needs to be
	 be integrated into a general inter-block-dependency scheme */
	    if (next_pc_p && taken_pc_p &&
		was_comp && taken_pc_p==current_block_pc_p) {
		blockinfo* bi1=get_blockinfo_addr_new((void*)next_pc_p,0);
		blockinfo* bi2=get_blockinfo_addr_new((void*)taken_pc_p,0);
		uae_u8 x=bi1->needed_flags;
		
		if (x==0xff || 1) {  /* To be on the safe side */
		    uae_u16* next=(uae_u16*)next_pc_p;
		    uae_u32 op=DO_GET_OPCODE(next);

		    x=0x1f;
		    x&=(~prop[op].set_flags);
		    x|=prop[op].use_flags;
		}
		
		x|=bi2->needed_flags;
		if (!(x & FLAG_CZNV)) { 
		    /* We can forget about flags */
		    dont_care_flags();
		    extra_len+=2; /* The next instruction now is part of this
				     block */
		}
		    
	    }
#endif
		log_flush();

	    if (next_pc_p) { /* A branch was registered */
		uintptr t1=next_pc_p;
		uintptr t2=taken_pc_p;
		int     cc=branch_cc;
		
		uae_u32* branchadd;
		uae_u32* tba;
		bigstate tmp;
		blockinfo* tbi;

		if (taken_pc_p<next_pc_p) {
		    /* backward branch. Optimize for the "taken" case ---
		       which means the raw_jcc should fall through when
		       the 68k branch is taken. */
		    t1=taken_pc_p;
		    t2=next_pc_p;
		    cc=branch_cc^1;
		}
		
		tmp=live; /* ouch! This is big... */
		compemu_raw_jcc_l_oponly(cc);
		branchadd=(uae_u32*)get_target();
		emit_long(0);
		
		/* predicted outcome */
		tbi=get_blockinfo_addr_new((void*)t1,1);
		match_states(tbi);
		compemu_raw_cmp_l_mi8((uintptr)specflags,0);
		compemu_raw_jcc_l_oponly(NATIVE_CC_EQ);
		tba=(uae_u32*)get_target();
		emit_jmp_target(get_handler(t1));
		compemu_raw_mov_l_mi((uintptr)&regs.pc_p,t1);
		flush_reg_count();
		compemu_raw_jmp((uintptr)popall_do_nothing);
		create_jmpdep(bi,0,tba,t1);

		align_target(align_jumps);
		/* not-predicted outcome */
		write_jmp_target(branchadd, (cpuop_func *)get_target());
		live=tmp; /* Ouch again */
		tbi=get_blockinfo_addr_new((void*)t2,1);
		match_states(tbi);

		//flush(1); /* Can only get here if was_comp==1 */
		compemu_raw_cmp_l_mi8((uintptr)specflags,0);
		compemu_raw_jcc_l_oponly(NATIVE_CC_EQ);
		tba=(uae_u32*)get_target();
		emit_jmp_target(get_handler(t2));
		compemu_raw_mov_l_mi((uintptr)&regs.pc_p,t2);
		flush_reg_count();
		compemu_raw_jmp((uintptr)popall_do_nothing);
		create_jmpdep(bi,1,tba,t2);
	    }		
	    else 
	    {
		if (was_comp) {
		    flush(1);
		}
		flush_reg_count();
		
		/* Let's find out where next_handler is... */
		if (was_comp && isinreg(PC_P)) { 
		    r=live.state[PC_P].realreg;
		    compemu_raw_and_l_ri(r,TAGMASK);
			int r2 = (r==0) ? 1 : 0;
			compemu_raw_mov_l_ri(r2,(uintptr)popall_do_nothing);
			compemu_raw_cmp_l_mi8((uintptr)specflags,0);
			compemu_raw_cmov_l_rm_indexed(r2,(uintptr)cache_tags,r,SIZEOF_VOID_P,NATIVE_CC_EQ);
			compemu_raw_jmp_r(r2);
		}
		else if (was_comp && isconst(PC_P)) {
		    uintptr v=live.state[PC_P].val;
		    uae_u32* tba;
		    blockinfo* tbi;

		    tbi=get_blockinfo_addr_new((void*)v,1);
		    match_states(tbi);

		    compemu_raw_cmp_l_mi8((uintptr)specflags,0);
		    compemu_raw_jcc_l_oponly(NATIVE_CC_EQ);
		    tba=(uae_u32*)get_target();
		    emit_jmp_target(get_handler(v));
		    compemu_raw_mov_l_mi((uintptr)&regs.pc_p,v);
		    compemu_raw_jmp((uintptr)popall_do_nothing);
		    create_jmpdep(bi,0,tba,v);
		}
		else {
		    r=REG_PC_TMP;
		    compemu_raw_mov_l_rm(r,(uintptr)&regs.pc_p);
		    compemu_raw_and_l_ri(r,TAGMASK);
			int r2 = (r==0) ? 1 : 0;
			compemu_raw_mov_l_ri(r2,(uintptr)popall_do_nothing);
			compemu_raw_cmp_l_mi8((uintptr)specflags,0);
			compemu_raw_cmov_l_rm_indexed(r2,(uintptr)cache_tags,r,SIZEOF_VOID_P,NATIVE_CC_EQ);
			compemu_raw_jmp_r(r2);
		}
	    }
	}

#if USE_MATCH	
	if (callers_need_recompile(&live,&(bi->env))) {
	    mark_callers_recompile(bi);
	}

	big_to_small_state(&live,&(bi->env));
#endif

#if USE_CHECKSUM_INFO
	remove_from_list(bi);
	if (trace_in_rom) {
		// No need to checksum that block trace on cache invalidation
		free_checksum_info_chain(bi->csi);
		bi->csi = NULL;
		add_to_dormant(bi);
	}
	else {
		calc_checksum(bi,&(bi->c1),&(bi->c2));
		add_to_active(bi);
	}
#else
	if (next_pc_p+extra_len>=max_pcp && 
	    next_pc_p+extra_len<max_pcp+LONGEST_68K_INST) 
	    max_pcp=next_pc_p+extra_len;  /* extra_len covers flags magic */
	else
	    max_pcp+=LONGEST_68K_INST;

	bi->len=max_pcp-min_pcp;
	bi->min_pcp=min_pcp;

	remove_from_list(bi);
	if (isinrom(min_pcp) && isinrom(max_pcp)) {
		add_to_dormant(bi); /* No need to checksum it on cache flush.
				       Please don't start changing ROMs in
				       flight! */
	}
	else {
		calc_checksum(bi,&(bi->c1),&(bi->c2));
		add_to_active(bi);
	}
#endif

	current_cache_size += get_target() - (uae_u8 *)current_compile_p;
	
#ifdef JIT_DEBUG
	if (JITDebug)
		bi->direct_handler_size = get_target() - (uae_u8 *)current_block_start_target;
	
	if (JITDebug && disasm_block) {
		uaecptr block_addr = start_pc + ((char *)pc_hist[0].location - (char *)start_pc_p);
		D(bug("M68K block @ 0x%08x (%d insns)\n", block_addr, blocklen));
		uae_u32 block_size = ((uae_u8 *)pc_hist[blocklen - 1].location - (uae_u8 *)pc_hist[0].location) + 1;
		disasm_m68k_block((uae_u8 *)pc_hist[0].location, block_size);
		D(bug("Compiled block @ 0x%08x\n", pc_hist[0].location));
		disasm_native_block((uae_u8 *)current_block_start_target, bi->direct_handler_size);
		getchar();
	}
#endif
	
	log_dump();
	align_target(align_jumps);

	/* This is the non-direct handler */
	bi->handler=
	    bi->handler_to_use=(cpuop_func *)get_target();
	compemu_raw_cmp_l_mi((uintptr)&regs.pc_p,(uintptr)pc_hist[0].location);
	compemu_raw_jnz((uintptr)popall_cache_miss);
	comp_pc_p=(uae_u8*)pc_hist[0].location;

	bi->status=BI_FINALIZING;
	init_comp();
	match_states(bi);
	flush(1);

	compemu_raw_jmp((uintptr)bi->direct_handler);

	flush_cpu_icache((void *)current_block_start_target, (void *)target);
	current_compile_p=get_target();
	raise_in_cl_list(bi);
	
	/* We will flush soon, anyway, so let's do it now */
	if (current_compile_p>=MAX_COMPILE_PTR)
		flush_icache_hard(7);
	
	bi->status=BI_ACTIVE;
	if (redo_current_block)
	    block_need_recompile(bi);
	
#ifdef PROFILE_COMPILE_TIME
	compile_time += (clock() - start_time);
#endif
    }

    /* Account for compilation time */
    cpu_do_check_ticks();
}

void do_nothing(void)
{
    /* What did you expect this to do? */
}

void exec_nostats(void)
{
	for (;;)  { 
		uae_u32 opcode = GET_OPCODE;
#ifdef FLIGHT_RECORDER
		m68k_record_step(m68k_getpc(), opcode);
#endif
		(*cpufunctbl[opcode])(opcode);
		cpu_check_ticks();
		if (end_block(opcode) || SPCFLAGS_TEST(SPCFLAG_ALL)) {
			return; /* We will deal with the spcflags in the caller */
		}
	}
}

void execute_normal(void)
{
	if (!check_for_cache_miss()) {
		cpu_history pc_hist[MAXRUN];
		int blocklen = 0;
#if 0 && FIXED_ADDRESSING
		start_pc_p = regs.pc_p;
		start_pc = get_virtual_address(regs.pc_p);
#else
		start_pc_p = regs.pc_oldp;  
		start_pc = regs.pc; 
#endif
		for (;;)  { /* Take note: This is the do-it-normal loop */
			pc_hist[blocklen++].location = (uae_u16 *)regs.pc_p;
			uae_u32 opcode = GET_OPCODE;
#ifdef FLIGHT_RECORDER
			m68k_record_step(m68k_getpc(), opcode);
#endif
			(*cpufunctbl[opcode])(opcode);
			cpu_check_ticks();
			if (end_block(opcode) || SPCFLAGS_TEST(SPCFLAG_ALL) || blocklen>=MAXRUN) {
				compile_block(pc_hist, blocklen);
				return; /* We will deal with the spcflags in the caller */
			}
			/* No need to check regs.spcflags, because if they were set,
			we'd have ended up inside that "if" */
		}
	}
}

typedef void (*compiled_handler)(void);

void m68k_do_compile_execute(void)
{
	for (;;) {
		((compiled_handler)(pushall_call_handler))();
		/* Whenever we return from that, we should check spcflags */
		if (SPCFLAGS_TEST(SPCFLAG_ALL)) {
			if (m68k_do_specialties ())
				return;
		}
	}
}

void m68k_compile_execute (void)
{
setjmpagain:
    TRY(prb) {
	for (;;) {
	    if (quit_program > 0) {
		if (quit_program == 1) {
#ifdef FLIGHT_RECORDER
		    dump_flight_recorder();
#endif
		    break;
		}
		quit_program = 0;
		m68k_reset ();
	    }
	    m68k_do_compile_execute();
	}
    }
    CATCH(prb) {
	flush_icache(0);
        Exception(prb, 0);
    	goto setjmpagain;
    }
}

