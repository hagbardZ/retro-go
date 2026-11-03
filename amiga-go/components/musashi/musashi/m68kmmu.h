/*
    m68kmmu.h - the 68030's MMU (for the Macintosh LC II and LC III)

    Written for esp32emu from the MC68030 User's Manual, section 9, with
    MAME's m68kmmu.h (R. Belmont) as the cross-check. Replaces the fork's
    68851/68030 sketch, which walked the tables on every access, had no
    transparent translation, read the 68030's PMOVE TT0/TT1 as SRP/CRP, and
    called exit() on anything it didn't expect.

    - TT0/TT1 transparent translation (address base/mask, function code,
      read/write)
    - TC: IS, TIA-TID, PS, SRE (the supervisor root pointer), FCL (a
      function code level)
    - the table walk: short (4-byte) and long (8-byte) descriptors, early
      termination (page descriptors above the last level), indirect page
      descriptors, U and M written back, WP, S and limits noted
    - an address translation cache (direct mapped, 256 pages), flushed by
      PMOVE to TC/SRP/CRP (unless FD), PFLUSH and resets; before it, 16
      recent pages for each function code and direction (m68ki_cpu.mmu_last,
      looked up inline in m68kcpu.h), emptied whenever translation changes
    - PMOVE (all three formats), PFLUSH/PFLUSHA, PLOAD, PTEST (MMUSR, An)

    Faults (invalid descriptors, write protection) are logged and the
    access goes through untranslated: no MMU exceptions, so no virtual
    memory.
*/

#define MMU_ATC_SIZE        256

typedef struct
{
	uint tag;           /* logical page | 0x80000000 when valid, 0x40000000 supervisor root */
	uint phys;          /* physical page base */
	uint8 modified;     /* the page descriptor's M is set */
} m68ki_atc_entry;

static m68ki_atc_entry m68ki_atc[MMU_ATC_SIZE];

/* The instructions after a PMOVE that changes translation were fetched
 * already (prefetch, the instruction cache): Apple's ROMs turn the MMU on
 * with `pmove (a3),tc; jmp (a5)`, where the jmp's address means something
 * else under the new tables. Program fetches up to the end of that cache
 * line keep the old mapping; the first one elsewhere ends it.
 */
static struct
{
	int active;
	uint lo, hi, phys;
} pmmu_prefetched;

/* MMUSR bits (N, the levels walked, in bits 2-0) */
#define MMUSR_B     0x8000
#define MMUSR_L     0x4000
#define MMUSR_S     0x2000
#define MMUSR_W     0x0800
#define MMUSR_I     0x0400
#define MMUSR_M     0x0200
#define MMUSR_T     0x0040

static void pmmu_log(const char *what, uint value)
{
	static int logged;
	if (logged < 16)
	{
		logged++;
		fprintf(stderr, "68030 MMU: %s (%08x, PC %08x)\n", what, value, REG_PPC);
	}
}

static void pmmu_forget_last(void)
{
	memset(m68ki_cpu.mmu_last, 0, sizeof(m68ki_cpu.mmu_last));
}

static void pmmu_flush(void)
{
	memset(m68ki_atc, 0, sizeof(m68ki_atc));
	pmmu_forget_last();
}

/* A page of this kind of access: phys is where it goes */
static inline void pmmu_remember(uint addr, uint fc, int write, uint phys)
{
	uint i = ((((fc & 7) << 1) | write) << 4) | ((addr >> m68ki_cpu.mmu_page_shift) & 15);
	uint mask = m68ki_cpu.mmu_page_mask;
	m68ki_cpu.mmu_last[i].tag = (addr & mask) | 1;
	m68ki_cpu.mmu_last[i].phys = phys & mask;
}

/* After a PMOVE: TC, SRP and CRP flush the ATC unless FD is set (Apple's
 * SwapMMUMode moves CRP with FD so that the ATC still has the page holding
 * the TC it loads next); TT0 and TT1 never do (MAME's MMU doesn't either)
 */
static void pmmu_update_enabled(int flush)
{
	uint tc = m68ki_cpu.mmu_tc;
	m68ki_cpu.pmmu_enabled = (tc & 0x80000000) ||
		(m68ki_cpu.mmu_tt0 & 0x8000) || (m68ki_cpu.mmu_tt1 & 0x8000) ||
		pmmu_prefetched.active;
	/* The recent pages are TC's size (4 KB with TT0/TT1 alone) */
	uint ps = (tc >> 20) & 0xf;
	m68ki_cpu.mmu_page_shift = (tc & 0x80000000) && ps >= 8 ? ps : 12;
	m68ki_cpu.mmu_page_mask = ~((1u << m68ki_cpu.mmu_page_shift) - 1);
	pmmu_forget_last();
	if (flush)
		pmmu_flush();
}

/* A reset turns translation off (TC and the TT registers' E) */
static void pmmu_reset(void)
{
	m68ki_cpu.mmu_tc &= ~0x80000000;
	m68ki_cpu.mmu_tt0 &= ~0x8000;
	m68ki_cpu.mmu_tt1 &= ~0x8000;
	m68ki_cpu.pmmu_enabled = 0;
	m68ki_cpu.mmu_page_shift = 12;
	m68ki_cpu.mmu_page_mask = 0xfffff000;
	pmmu_prefetched.active = 0;
	pmmu_flush();
}

uint pmmu_translate_addr(uint addr, uint fc, int write);
static uint pmmu_atc_load(uint addr, uint fc, int write);

/* Before a PMOVE changes translation: where the next instructions came
 * from (still the first mapping when several PMOVEs follow each other)
 */
static void pmmu_keep_prefetch(void)
{
	uint fc = FLAG_S | FUNCTION_CODE_USER_PROGRAM;
	uint phys;
	if (pmmu_prefetched.active && REG_PC - pmmu_prefetched.lo < pmmu_prefetched.hi - pmmu_prefetched.lo)
		phys = pmmu_prefetched.phys + (REG_PC - pmmu_prefetched.lo);
	else
	{
		pmmu_prefetched.active = 0;
		phys = PMMU_ENABLED ? pmmu_translate_addr(REG_PC, fc, 0) : REG_PC;
	}
	pmmu_prefetched.lo = REG_PC;
	pmmu_prefetched.hi = (REG_PC + 4 + 15) & ~15;
	pmmu_prefetched.phys = phys;
	pmmu_prefetched.active = 1;
	pmmu_forget_last();             /* program fetches: through the window */
}

/* A transparent translation register matches: address bits 31-24 under
 * the mask, the function code under its mask, and read/write unless that
 * is masked out (R/W 1 = read)
 */
static inline int pmmu_tt_match(uint tt, uint addr, uint fc, int write)
{
	if (!(tt & 0x8000))
		return 0;
	uint mask = (tt >> 16) & 0xff;
	if ((((addr >> 24) ^ (tt >> 24)) & ~mask & 0xff) != 0)
		return 0;
	if (((fc ^ (tt >> 4)) & ~tt & 7) != 0)
		return 0;
	if (!(tt & 0x0100) && (((tt >> 9) & 1) == (uint)write))
		return 0;
	return 1;
}

/* Set U (and M on a write) in a descriptor's first long word */
static inline uint pmmu_mark(uint at, uint d, int write, int update)
{
	uint want = d | 8 | (write ? 0x10 : 0);
	if (update && want != d)
	{
		m68k_write_memory_32(at, want);
		d = want;
	}
	return d;
}

/* The table walk. Returns the physical address; *status gets MMUSR bits
 * and the levels walked (I set: no translation), *desc_at the last
 * descriptor's address.
 */
static uint pmmu_walk(uint addr, uint fc, int write, int update, uint *status, uint *desc_at)
{
	uint tc = m68ki_cpu.mmu_tc;
	uint is = (tc >> 16) & 0xf;
	uint ti[4] = { (tc >> 12) & 0xf, (tc >> 8) & 0xf, (tc >> 4) & 0xf, tc & 0xf };
	uint root_limit, root_aptr;
	uint levels = 0, used = is, s = 0;

	if ((tc & 0x02000000) && (fc & 4))
	{
		root_limit = m68ki_cpu.mmu_srp_limit;
		root_aptr = m68ki_cpu.mmu_srp_aptr;
	}
	else
	{
		root_limit = m68ki_cpu.mmu_crp_limit;
		root_aptr = m68ki_cpu.mmu_crp_aptr;
	}
	*desc_at = 0;

	/* `size`: the format of the descriptors in `table` (2 short, 3 long);
	 * `limit_word`: the long descriptor whose limit bounds the next index
	 */
	uint size = root_limit & 3;
	uint table = root_aptr & 0xfffffff0;
	uint limit_word = root_limit;
	if (size == 0)
	{
		*status = MMUSR_I;
		return addr;
	}
	if (size == 1)
	{
		/* the root pointer is a page descriptor: no tables */
		*status = 0;
		return (root_aptr & 0xffffff00) + ((addr << is) >> is);
	}

	for (int level = -1; level < 4; level++)
	{
		uint index, bits;
		if (level < 0)
		{
			if (!(tc & 0x01000000))
				continue;
			index = fc & 7;     /* the function code level */
			bits = 3;
		}
		else
		{
			bits = ti[level];
			if (bits == 0)
				break;
			index = (addr << used) >> (32 - bits);
			used += bits;
		}
		if ((limit_word & 3) == 3)
		{
			uint lim = (limit_word >> 16) & 0x7fff;
			if ((limit_word & 0x80000000) ? index < lim : index > lim)
				s |= MMUSR_L;
		}
		uint at = table + index * (size == 3 ? 8 : 4);
		uint d = m68k_read_memory_32(at);
		uint d1 = size == 3 ? m68k_read_memory_32(at + 4) : 0;
		uint target = size == 3 ? d1 : d;
		uint dt = d & 3;
		levels++;
		*desc_at = at;
		if (d & 4)
			s |= MMUSR_W;
		if (size == 3 && (d & 0x100))
			s |= MMUSR_S;
		if (dt == 0)
		{
			*status = s | MMUSR_I | levels;
			return addr;
		}
		int last = level == 3 || (level >= 0 && ti[level + 1] == 0);
		if (dt != 1 && last)
		{
			/* indirect: the address of a page descriptor (dt: its format) */
			at = target & (dt == 3 ? 0xfffffff0 : 0xfffffffc);
			d = m68k_read_memory_32(at);
			target = dt == 3 ? m68k_read_memory_32(at + 4) : d;
			levels++;
			*desc_at = at;
			if ((d & 3) != 1)
			{
				*status = s | MMUSR_I | levels;
				return addr;
			}
			if (d & 4)
				s |= MMUSR_W;
			dt = 1;
		}
		if (dt == 1)
		{
			/* a page descriptor (above the last level: early termination) */
			d = pmmu_mark(at, d, write, update);
			if (d & 0x10)
				s |= MMUSR_M;
			*status = s | levels;
			return (target & 0xffffff00) + (used < 32 ? (addr << used) >> used : 0);
		}
		pmmu_mark(at, d, 0, update);
		limit_word = d;
		table = target & 0xfffffff0;
		size = dt;
	}
	*status = s | MMUSR_I | levels;
	return addr;
}

/* Translate one access */
uint pmmu_translate_addr(uint addr, uint fc, int write)
{
	if (pmmu_prefetched.active && (fc & 3) == FUNCTION_CODE_USER_PROGRAM)
	{
		if (addr - pmmu_prefetched.lo < pmmu_prefetched.hi - pmmu_prefetched.lo)
			return pmmu_prefetched.phys + (addr - pmmu_prefetched.lo);
		pmmu_prefetched.active = 0;
		m68ki_cpu.pmmu_enabled = (m68ki_cpu.mmu_tc & 0x80000000) ||
			(m68ki_cpu.mmu_tt0 & 0x8000) || (m68ki_cpu.mmu_tt1 & 0x8000);
	}
	uint tc = m68ki_cpu.mmu_tc;
	if (pmmu_tt_match(m68ki_cpu.mmu_tt0, addr, fc, write) ||
		pmmu_tt_match(m68ki_cpu.mmu_tt1, addr, fc, write) || !(tc & 0x80000000))
	{
		/* (TT0/TT1 cover 16 MB: the page is theirs) */
		pmmu_remember(addr, fc, write, addr);
		return addr;
	}

	uint ps = (tc >> 20) & 0xf;
	uint page = addr >> ps;
	uint tag = 0x80000000 | page | ((tc & 0x02000000) && (fc & 4) ? 0x40000000 : 0);
	m68ki_atc_entry *e = &m68ki_atc[page & (MMU_ATC_SIZE - 1)];
	if (e->tag == tag && (!write || e->modified))
	{
		pmmu_remember(addr, fc, write, e->phys);
		return e->phys | (addr & ((1u << ps) - 1));
	}
	return pmmu_atc_load(addr, fc, write);
}

/* Walk the tables for addr and keep the page in the ATC */
static uint pmmu_atc_load(uint addr, uint fc, int write)
{
	uint tc = m68ki_cpu.mmu_tc;
	uint ps = (tc >> 20) & 0xf;
	uint page = addr >> ps;
	uint tag = 0x80000000 | page | ((tc & 0x02000000) && (fc & 4) ? 0x40000000 : 0);
	m68ki_atc_entry *e = &m68ki_atc[page & (MMU_ATC_SIZE - 1)];
	uint status, desc_at;
	uint phys = pmmu_walk(addr, fc, write, 1, &status, &desc_at);
	if (status & MMUSR_I)
	{
		char what[48];
		snprintf(what, sizeof(what), "invalid descriptor, fc %u %s, level %u", fc,
			write ? "write" : "read", status & 7);
		pmmu_log(what, addr);
		return addr;
	}
	if ((status & MMUSR_W) && write)
		pmmu_log("write to a protected page", addr);
	e->tag = tag;
	e->phys = phys & ~((1u << ps) - 1);
	e->modified = (status & MMUSR_M) != 0;
	if (!write || e->modified)
		pmmu_remember(addr, fc, write, e->phys);
	return phys;
}

/* ---- The MMU instructions (coprocessor 0) ---- */

static uint pmmu_fc(uint field)
{
	if ((field & 0x18) == 0x10)
		return field & 7;
	if ((field & 0x18) == 0x08)
		return REG_D[field & 7] & 7;
	if (field == 0)
		return REG_SFC & 7;
	if (field == 1)
		return REG_DFC & 7;
	return 0;
}

/* The address a control addressing mode names */
static uint pmmu_ea(uint ea)
{
	switch (ea >> 3)
	{
		case 2: return EA_AY_AI_32();
		case 5: return EA_AY_DI_32();
		case 6: return EA_AY_IX_32();
		case 7:
			switch (ea & 7)
			{
				case 0: return EA_AW_32();
				case 1: return EA_AL_32();
				case 2: return EA_PCDI_32();
				case 3: return EA_PCIX_32();
			}
	}
	pmmu_log("MMU instruction with an unexpected mode", ea);
	return 0;
}

static void pmmu_pmove(uint modes, uint ea)
{
	int to_memory = (modes >> 9) & 1;
	uint reg = (modes >> 10) & 7;
	switch ((modes >> 13) & 7)
	{
		case 0:     /* format 1: TT0 (2), TT1 (3) */
			if (reg == 2 || reg == 3)
			{
				uint *tt = reg == 2 ? &m68ki_cpu.mmu_tt0 : &m68ki_cpu.mmu_tt1;
				if (to_memory)
					WRITE_EA_32(ea, *tt);
				else
				{
					uint v = READ_EA_32(ea);
					pmmu_keep_prefetch();
					*tt = v;
					pmmu_update_enabled(0);
				}
				return;
			}
			break;
		case 2:     /* format 2: TC (0), SRP (2), CRP (3) */
			if (reg == 0)
			{
				if (to_memory)
					WRITE_EA_32(ea, m68ki_cpu.mmu_tc);
				else
				{
					uint v = READ_EA_32(ea);
					pmmu_keep_prefetch();
					m68ki_cpu.mmu_tc = v;
					pmmu_update_enabled(!(modes & 0x100));
				}
				return;
			}
			if (reg == 2 || reg == 3)
			{
				uint *limit = reg == 2 ? &m68ki_cpu.mmu_srp_limit : &m68ki_cpu.mmu_crp_limit;
				uint *aptr = reg == 2 ? &m68ki_cpu.mmu_srp_aptr : &m68ki_cpu.mmu_crp_aptr;
				if (to_memory)
					WRITE_EA_64(ea, (uint64)*limit << 32 | *aptr);
				else
				{
					uint64 v = READ_EA_64(ea);
					pmmu_keep_prefetch();
					*limit = (uint)(v >> 32);
					*aptr = (uint)v;
					pmmu_update_enabled(!(modes & 0x100));
				}
				return;
			}
			break;
		case 3:     /* format 3: MMUSR */
			if (to_memory)
				WRITE_EA_16(ea, m68ki_cpu.mmu_sr);
			else
				m68ki_cpu.mmu_sr = READ_EA_16(ea);
			return;
	}
	pmmu_log("PMOVE with an unknown register", modes);
}

void m68881_mmu_ops(void)
{
	uint ea = m68ki_cpu.ir & 0x3f;

	if ((m68ki_cpu.ir & 0xffc0) == 0xf0c0 || (m68ki_cpu.ir & 0xffc0) == 0xf080)
	{
		pmmu_log("PBcc (68851 only)", m68ki_cpu.ir);
		return;
	}
	if ((m68ki_cpu.ir >> 9) & 7)
	{
		pmmu_log("unknown MMU instruction group", m68ki_cpu.ir);
		return;
	}
	uint modes = OPER_I_16();

	switch (modes >> 13)
	{
		case 1:
			if ((modes & 0xfde0) == 0x2000)
			{
				/* PLOAD: an ATC entry now (U and M too) */
				uint fc = pmmu_fc(modes & 0x1f);
				uint addr = pmmu_ea(ea);
				if (m68ki_cpu.mmu_tc & 0x80000000)
					pmmu_atc_load(addr, fc, !((modes >> 9) & 1));
				return;
			}
			/* PFLUSH, PFLUSHA (by function code and address as well: all of it) */
			if (((modes >> 10) & 7) >= 4)
				(void)pmmu_ea(ea);
			pmmu_flush();
			return;
		case 4:
		{
			/* PTEST: walk (to the level asked for), MMUSR and An */
			uint fc = pmmu_fc(modes & 0x1f);
			uint addr = pmmu_ea(ea);
			int write = !((modes >> 9) & 1);
			uint status = 0, desc_at = 0;
			if (pmmu_tt_match(m68ki_cpu.mmu_tt0, addr, fc, write) ||
				pmmu_tt_match(m68ki_cpu.mmu_tt1, addr, fc, write))
				status = MMUSR_T;
			else if (m68ki_cpu.mmu_tc & 0x80000000)
			{
				pmmu_walk(addr, fc, write, 0, &status, &desc_at);
				if (((modes >> 10) & 7) == 0)
					status &= ~7;   /* level 0: the cache only */
			}
			m68ki_cpu.mmu_sr = status;
			if (modes & 0x100)
				REG_A[(modes >> 5) & 7] = desc_at;
			return;
		}
		case 0:
		case 2:
		case 3:
			pmmu_pmove(modes, ea);
			return;
	}
	pmmu_log("unknown MMU instruction", modes);
}
