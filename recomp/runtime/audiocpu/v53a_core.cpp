// license:BSD-3-Clause
// copyright-holders:Bryan McPhail
/****************************************************************************

    NEC V20/V30/V33 emulator - V33A core of the standalone V53A port

    Ported from MAME src/devices/cpu/nec/nec.cpp + necpriv.ipp (V33 paths
    only; V20/V30/V25 quirks and the 8080 emulation mode are not needed on
    the V33A: BRKEM is rejected there and the mode flag can never clear).

    (Re)Written June-September 2000 by Bryan McPhail (mish@tendril.co.uk) based
    on code by Oliver Bergmann (Raul_Bloodworth@hotmail.com) who based code
    on the i286 emulator by Fabrice Frances which had initial work based on
    David Hedley's pcemu(!).

    This new core features 99% accurate cycle counts for each processor,
    there are still some complex situations where cycle counts are wrong,
    typically where a few instructions have differing counts for odd/even
    source and odd/even destination memory operands.

    Flag settings are also correct for the NEC processors rather than the
    I86 versions.

    Changelist:

    22/02/2003:
        Removed cycle counts from memory accesses - they are certainly wrong,
        and there is already a memory access cycle penalty in the opcodes
        using them.

        Fixed save states.

        Fixed ADJBA/ADJBS/ADJ4A/ADJ4S flags/return values for all situations.
        (Fixes bugs in Geostorm and Thunderblaster)

        Fixed carry flag on NEG (I thought this had been fixed circa Mame 0.58,
        but it seems I never actually submitted the fix).

        Fixed many cycle counts in instructions and bug in cycle count
        macros (odd word cases were testing for odd instruction word address
        not data address).

****************************************************************************/

#include "v53a.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace v53 {

typedef uint8_t BOOLEAN;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;

#define BIT(x, n) (((x) >> (n)) & 1)
#define LOGMASKED(mask, ...) do { } while (0)
#define LOG_BUSLOCK (1 << 1)

/*********************** necpriv.ipp ***********************/

namespace {

/* Cpu types, steps of 8 to help the cycle count calculation */
#define V33_TYPE 0
#define V30_TYPE 8
#define V20_TYPE 16
#define V25_TYPE 24

/* interrupt vectors */
enum
{
	NEC_DIVIDE_VECTOR   = 0,
	NEC_TRAP_VECTOR     = 1,
	NEC_NMI_VECTOR      = 2,
	NEC_BRKV_VECTOR     = 4,
	NEC_CHKIND_VECTOR   = 5
};

/* interrupt sources */
enum INTSOURCES
{
	BRK     = 0,
	INT_IRQ = 1,
	NMI_IRQ = 2
};


enum SREGS { DS1=0, PS, SS, DS0 };
enum WREGS { AW=0, CW, DW, BW, SP, BP, IX, IY };
enum BREGS {
	// little-endian host (checked in v53a.h)
	AL = 0x0,
	AH = 0x1,
	CL = 0x2,
	CH = 0x3,
	DL = 0x4,
	DH = 0x5,
	BL = 0x6,
	BH = 0x7
};

#define CLEAR_LINE 0

#define Sreg(x) m_sregs[x]
#define Wreg(x) m_regs.w[x]
#define Breg(x) m_regs.b[x]

#define PC()    ((Sreg(PS)<<4)+m_ip)

#define CF      (m_CarryVal!=0)
#define SF      (m_SignVal<0)
#define ZF      (m_ZeroVal==0)
#define PF      parity_table[(BYTE)m_ParityVal]
#define AF      (m_AuxVal!=0)
#define OF      (m_OverVal!=0)

/************************************************************************/

// m_chip_type is always V33_TYPE: addresses always go through v33_translate()
#define read_mem_byte(a)        mem_rb(v33_translate(a))
#define read_mem_word(a)        mem_rw(v33_translate(a))
#define write_mem_byte(a,d)     mem_wb(v33_translate(a), (d))
#define write_mem_word(a,d)     mem_ww(v33_translate(a), (d))

#define read_port_byte(a)       io_read_byte(a)
#define read_port_word(a)       io_read_word(a)
#define write_port_byte(a,d)    io_write_byte((a),(d))
#define write_port_word(a,d)    io_write_word((a),(d))

/************************************************************************/

#define CHANGE_PC do { EMPTY_PREfetch(); } while (0)

#define SegBase(Seg) (Sreg(Seg) << 4)

#define DefaultBase(Seg) ((m_seg_prefix && (Seg==DS0 || Seg==SS)) ? m_prefix_base : Sreg(Seg) << 4)

#define GetMemB(Seg,Off) (read_mem_byte(DefaultBase(Seg) + (Off)))
#define GetMemW(Seg,Off) (read_mem_word(DefaultBase(Seg) + (Off)))

#define PutMemB(Seg,Off,x) { write_mem_byte(DefaultBase(Seg) + (Off), (x)); }
#define PutMemW(Seg,Off,x) { write_mem_word(DefaultBase(Seg) + (Off), (x)); }

/* prefetch timing */

#define EMPTY_PREfetch()    m_prefetch_reset = 1


#define PUSH(val) { Wreg(SP) -= 2; write_mem_word(((Sreg(SS)<<4)+Wreg(SP)), val); }
#define POP(var) { Wreg(SP) += 2; var = read_mem_word(((Sreg(SS)<<4) + ((Wreg(SP)-2) & 0xffff))); }

#define PUSH80(val) { Wreg(BP) -= 2; write_mem_word(((Sreg(DS0)<<4)+Wreg(BP)), val); }
#define POP80(var) { Wreg(BP) += 2; var = read_mem_word(((Sreg(DS0)<<4) + ((Wreg(BP)-2) & 0xffff))); }

#define BRKXA(xa) { if (m_chip_type == V33_TYPE) { nec_brk(fetch()); m_xa = xa; } else logerror("%06x: %sXA instruction is V33 exclusive\n", PC(), xa ? "BRK" : "RET"); }
#define BRKEM { if (m_chip_type == V33_TYPE) logerror("%06x: BRKEM not supported on V33\n",PC()); else nec_brk(fetch()); }

#define GetModRM uint32_t ModRM=fetch()

/* Cycle count macros:
    CLK  - cycle count is the same on all processors
    CLKS - cycle count differs between processors, list all counts
    CLKW - cycle count for word read/write differs for odd/even source/destination address
    CLKM - cycle count for reg/mem instructions
    CLKR - cycle count for reg/mem instructions with different counts for odd/even addresses

    Prefetch & buswait time is not emulated.
    Extra cycles for PUSH'ing or POP'ing registers to odd addresses is not emulated.
*/

#define CLK(all) do { int cc = all; m_cur_cycles+=cc; m_icount-=cc; } while(0)
#define CLKS(v20,v30,v33,v25) do { const uint32_t ccount=(v20<<16)|(v30<<8)|v33; CLK((ccount>>m_chip_type)&0x7f); } while(0)
#define CLKW(v20o,v30o,v33o,v25o,v20e,v30e,v33e,v25e,addr) do { const uint32_t ocount=(v20o<<16)|(v30o<<8)|v33o, ecount=(v20e<<16)|(v30e<<8)|v33e; CLK((addr&1)?((ocount>>m_chip_type)&0x7f):((ecount>>m_chip_type)&0x7f)); } while(0)
#define CLKM(v20,v30,v33,v25,v20m,v30m,v33m,v25m) do { const uint32_t ccount=(v20<<16)|(v30<<8)|v33, mcount=(v20m<<16)|(v30m<<8)|v33m; CLK(( ModRM >=0xc0 )?((ccount>>m_chip_type)&0x7f):((mcount>>m_chip_type)&0x7f)); } while(0)
#define CLKR(v20o,v30o,v33o,v25o,v20e,v30e,v33e,v25e,vall,v25all,addr) do { const uint32_t ocount=(v20o<<16)|(v30o<<8)|v33o, ecount=(v20e<<16)|(v30e<<8)|v33e; if (ModRM >=0xc0) CLK(vall); else CLK((addr&1)?((ocount>>m_chip_type)&0x7f):((ecount>>m_chip_type)&0x7f)); } while(0)

/************************************************************************/
#define CompressFlags() (WORD)(int(CF) | 0x02 | (int(PF) << 2) | (int(AF) << 4) | (int(ZF) << 6) \
				| (int(SF) << 7) | (m_TF << 8) | (m_IF << 9) \
				| (m_DF << 10) | (int(OF) << 11) | 0x7000 | (m_MF << 15))

#define ExpandFlags(f) \
{ \
	m_CarryVal = (f) & 0x0001; \
	m_ParityVal = !((f) & 0x0004); \
	m_AuxVal = (f) & 0x0010; \
	m_ZeroVal = !((f) & 0x0040); \
	m_SignVal = (f) & 0x0080 ? -1 : 0; \
	m_TF = ((f) & 0x0100) == 0x0100; \
	m_IF = ((f) & 0x0200) == 0x0200; \
	m_DF = ((f) & 0x0400) == 0x0400; \
	m_OverVal = (f) & 0x0800; \
	m_MF = (((f) & 0x8000) == 0x8000) || m_em; \
}

} // anonymous namespace

/*********************** nec.cpp ***********************/

void V53A::prefetch()
{
	m_prefetch_count--;
}

void V53A::do_prefetch()
{
	/* The implementation is not accurate, but comes close.
	 * It does not respect that the V30 will fetch two bytes
	 * at once directly, but instead uses only 2 cycles instead
	 * of 4. There are however only very few sources publicly
	 * available and they are vague.
	 */
	while (m_prefetch_count < 0)
	{
		m_prefetch_count++;
		if (m_cur_cycles > m_prefetch_cycles)
			m_cur_cycles -= m_prefetch_cycles;
		else
			m_icount -= m_prefetch_cycles;
	}

	if (m_prefetch_reset)
	{
		m_prefetch_count = 0;
		m_prefetch_reset = 0;
		return;
	}

	while (m_cur_cycles >= m_prefetch_cycles && m_prefetch_count < m_prefetch_size)
	{
		m_cur_cycles -= m_prefetch_cycles;
		m_prefetch_count++;
	}
}

uint8_t V53A::fetch()
{
	prefetch();
	return mem_rb(v33_translate((Sreg(PS)<<4) + m_ip++));
}

uint16_t V53A::fetchword()
{
	uint16_t r = fetch();
	r |= (fetch()<<8);
	return r;
}

#include "v53a_necinstr.h"
#include "v53a_necmacro.h"
#include "v53a_necea.h"
#include "v53a_necmodrm.h"

static uint8_t parity_table[256];

uint8_t V53A::fetchop()
{
	prefetch();
	return mem_rb(v33_translate((Sreg(PS)<<4) + m_ip++));
}

uint16_t V53A::mem_rw(uint32_t a)
{
	a &= 0xffffff;
	if (!(a & 1) && cb.mem_read16)
		return cb.mem_read16(a);
	return uint16_t(mem_rb(a) | (mem_rb(a + 1) << 8));
}

void V53A::mem_ww(uint32_t a, uint16_t d)
{
	a &= 0xffffff;
	if (!(a & 1) && cb.mem_write16)
	{
		cb.mem_write16(a, d);
		return;
	}
	mem_wb(a, uint8_t(d));
	mem_wb(a + 1, uint8_t(d >> 8));
}

uint16_t V53A::flags() const
{
	return CompressFlags();
}

/***************************************************************************/

void V53A::core_reset()
{
	// the V30 leaves its general registers undefined on reset; unverified on the other variants
	if (!has_v30_quirks())
		std::fill(std::begin(m_regs.w), std::end(m_regs.w), 0);

	m_ip = 0;
	m_prev_ip = 0;
	m_rep_ip = 0;
	m_TF = 0;
	m_IF = 0;
	m_DF = 0;
	m_MF = 1;
	m_em = 1;
	m_SignVal = 0;
	m_AuxVal = 0;
	m_OverVal = 0;
	m_ZeroVal = 1;
	m_CarryVal = 0;
	m_ParityVal = 1;
	m_pending_irq = 0;
	m_nmi_state = 0;
	m_irq_state = 0;
	m_poll_state = 1;
	m_halted = 0;
	m_rep_params = 0;
	m_io_retry = false;
	m_io_word_pending = false;
	m_io_word_latch = 0;

	if (m_chip_type == V33_TYPE)
		m_xa = false;

	Sreg(PS) = 0xffff;
	Sreg(SS) = 0;
	Sreg(DS0) = 0;
	Sreg(DS1) = 0;

	CHANGE_PC;
}


void V53A::nec_interrupt(unsigned int_num, int/*INTSOURCES*/ source)
{
	uint32_t dest_seg, dest_off;

	m_rep_params = 0;
	i_pushf();
	m_TF = m_IF = 0;
	m_MF = 1;

	if (source == INT_IRQ)  /* get vector */
		int_num = standard_irq_callback();

	dest_off = read_mem_word(int_num*4);
	dest_seg = read_mem_word(int_num*4+2);

	PUSH(Sreg(PS));
	PUSH(m_ip);
	m_prev_ip = m_ip = (WORD)dest_off;
	Sreg(PS) = (WORD)dest_seg;
	CHANGE_PC;
}

void V53A::nec_trap()
{
	(this->*s_nec_instruction[fetchop()])();
	nec_interrupt(NEC_TRAP_VECTOR, BRK);
}

void V53A::nec_brk(unsigned int_num)
{
	if (m_chip_type != V33_TYPE)
	{
		m_em = 0;
		m_MF = 0;
		i_pushf();
		PUSH(Sreg(PS));
		PUSH(m_ip);
	}
	m_prev_ip = m_ip = read_mem_word(int_num*4);
	Sreg(PS) = read_mem_word(int_num*4+2);
	CHANGE_PC;
}

void V53A::external_int()
{
	if (m_pending_irq & NMI_IRQ)
	{
		nec_interrupt(NEC_NMI_VECTOR, NMI_IRQ);
		m_pending_irq &= ~NMI_IRQ;
	}
	else if (m_pending_irq)
	{
		/* the actual vector is retrieved after pushing flags */
		/* and clearing the IF */
		nec_interrupt((uint32_t)-1, INT_IRQ);
		m_irq_state = CLEAR_LINE;
		m_pending_irq &= ~INT_IRQ;
	}
}

/****************************************************************************/
/*                             OPCODES                                      */
/****************************************************************************/

#include "v53a_necinstr.hxx"

/*****************************************************************************/

void V53A::set_int_line(int state)
{
	m_irq_state = state;
	if (state == CLEAR_LINE)
		m_pending_irq &= ~INT_IRQ;
	else
	{
		m_pending_irq |= INT_IRQ;
		m_halted = 0;
	}
}

void V53A::set_nmi_line(int state)
{
	if (m_nmi_state == uint32_t(state))
		return;
	m_nmi_state = state;
	if (state != CLEAR_LINE)
	{
		m_pending_irq |= NMI_IRQ;
		m_halted = 0;
	}
}

void V53A::set_poll_line(int state)
{
	m_poll_state = state;
}

void V53A::core_start()
{
	unsigned int i, j, c;

	static const WREGS wreg_name[8]={ AW, CW, DW, BW, SP, BP, IX, IY };
	static const BREGS breg_name[8]={ AL, CL, DL, BL, AH, CH, DH, BH };

	for (i = 0; i < 256; i++)
	{
		for (j = i, c = 0; j > 0; j >>= 1)
			if (j & 1) c++;
		parity_table[i] = !(c & 1);
	}

	for (i = 0; i < 256; i++)
	{
		Mod_RM.reg.b[i] = breg_name[(i & 0x38) >> 3];
		Mod_RM.reg.w[i] = wreg_name[(i & 0x38) >> 3];
	}

	for (i = 0xc0; i < 0x100; i++)
	{
		Mod_RM.RM.w[i] = wreg_name[i & 7];
		Mod_RM.RM.b[i] = breg_name[i & 7];
	}

	m_no_interrupt = 0;
	m_cur_cycles = 0;
	m_prefetch_count = 0;
	m_prefetch_reset = 0;
	m_prefix_base = 0;
	m_seg_prefix = 0;
	m_EA = 0;
	m_EO = 0;
	m_E16 = 0;
	m_ip = 0;
	m_prev_ip = 0;
	m_rep_ip = 0;
	m_io_retry = false;
	m_io_word_pending = false;
	m_io_word_latch = 0;
	m_xa = false;
	m_icount = 0;

	memset(m_regs.w, 0x00, sizeof(m_regs.w));
	memset(m_sregs, 0x00, sizeof(m_sregs));
}

uint16_t V53A::nec_io_read_word(uint32_t a)
{
	a &= 0xffff;
	if (!(a & 1))
		return ext_io_read_word(a);

	// an odd word takes two cycles
	const uint8_t lo = io_read_byte(a);
	return uint16_t(lo | (io_read_byte((a + 1) & 0xffff) << 8));
}

void V53A::nec_io_write_word(uint32_t a, uint16_t v)
{
	a &= 0xffff;
	if (!(a & 1))
	{
		ext_io_write_word(a, v);
		return;
	}

	io_write_byte(a, uint8_t(v));
	io_write_byte((a + 1) & 0xffff, uint8_t(v >> 8));
}

void V53A::execute_run()
{
	if (m_halted)
	{
		m_icount = 0;
		return;
	}

	while(m_icount>0)
	{
		m_prev_ip = m_ip;

		// Dispatch IRQ
		if (!m_io_retry && m_pending_irq && m_no_interrupt==0)
		{
			if (m_pending_irq & NMI_IRQ)
				external_int();
			else if (m_IF)
				external_int();
		}

		// No interrupt allowed between last instruction and this one
		if (!m_io_retry && m_no_interrupt)
			m_no_interrupt--;

		m_cur_cycles = 0;

		if (m_rep_params)
			cont_rep();
		else
		{
			// m_MF can't be cleared on the V33 (no BRKEM), so the 8080 table is never used
			(this->*s_nec_instruction[fetchop()])();
		}
		do_prefetch();
	}
}

} // namespace v53
