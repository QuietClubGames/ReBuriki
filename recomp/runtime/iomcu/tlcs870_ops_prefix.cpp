// license:BSD-3-Clause
// copyright-holders:David Haywood
/*************************************************************************************************************

    Toshiba TLCS-870 Series MCUs  (standalone port of MAME's tlcs870 core)

    Prefixed opcodes:
      (src) prefix ops  E0-E7  (MAME tlcs870_ops_src.cpp)
      (reg) prefix ops  E8-EF  (MAME tlcs870_ops_reg.cpp)
      (dst) prefix ops  F0-F7  (MAME tlcs870_ops_dst.cpp)

    (src)/(dst) address mode from the low 3 bits of the prefix:
      0 (x)  1 (PC+A)  2 (DE)  3 (HL)  4 (HL+d)  5 (HL+C)  6 (HL+)  7 (-HL)
    (x) and (HL+d) have the immediate byte directly after the prefix.  F1/F5 are invalid for dst.

    (reg) prefix: E8-EF select A,W,C,B,E,D,L,H (8-bit) or WA,BC,DE,HL,WA,BC,DE,HL (16-bit);
    RETN is E8 04 only.

*************************************************************************************************************/

#include "tlcs870.h"

/*************************************************************************************************************
    (src) E0-E7
*************************************************************************************************************/

void Tlcs870::do_srcprefixtype_opcode(uint8_t opbyte0)
{
	m_cycles = uint8_t(get_base_srcdst_cycles(opbyte0 & 0x7)); // base number of cycles from src prefix mode

	uint16_t srcaddr;
	if ((opbyte0 == 0xe0) || (opbyte0 == 0xe4))
		srcaddr = get_addr((opbyte0 & 0x7), READ8());
	else
		srcaddr = get_addr((opbyte0 & 0x7), 0);

	const uint8_t opbyte1 = READ8();

	switch (opbyte1)
	{
	case 0x08: do_ROLD_A_insrc(opbyte0, opbyte1, srcaddr); break;
	case 0x09: do_RORD_A_insrc(opbyte0, opbyte1, srcaddr); break;
	case 0x14: case 0x15: case 0x16: case 0x17: do_LD_rr_insrc(opbyte0, opbyte1, srcaddr); break;
	case 0x20: do_INC_insrc(opbyte0, opbyte1, srcaddr); break;
	case 0x26: do_LD_inx_insrc(opbyte0, opbyte1, srcaddr); break;
	case 0x27: do_LD_inHL_insrc(opbyte0, opbyte1, srcaddr); break;
	case 0x28: do_DEC_insrc(opbyte0, opbyte1, srcaddr); break;
	case 0x2f: do_MCMP_insrc_n(opbyte0, opbyte1, srcaddr); break;
	case 0xfc: do_CALL_insrc(opbyte0, opbyte1, srcaddr); break;
	case 0xfe: do_JP_insrc(opbyte0, opbyte1, srcaddr); break;
	default:
		if (opbyte1 >= 0x40 && opbyte1 <= 0x47) do_SET_insrcbit(opbyte0, opbyte1, srcaddr);
		else if (opbyte1 >= 0x48 && opbyte1 <= 0x4f) do_CLR_insrcbit(opbyte0, opbyte1, srcaddr);
		else if (opbyte1 >= 0x58 && opbyte1 <= 0x5f) do_LD_r_insrc(opbyte0, opbyte1, srcaddr);
		else if (opbyte1 >= 0x60 && opbyte1 <= 0x67) do_ALUOP_insrc_inHL(opbyte0, opbyte1, srcaddr);
		else if (opbyte1 >= 0x70 && opbyte1 <= 0x77) do_ALUOP_insrc_n(opbyte0, opbyte1, srcaddr);
		else if (opbyte1 >= 0x78 && opbyte1 <= 0x7f) do_ALUOP_A_insrc(opbyte0, opbyte1, srcaddr);
		else if (opbyte1 >= 0xa8 && opbyte1 <= 0xaf) do_XCH_r_insrc(opbyte0, opbyte1, srcaddr);
		else if (opbyte1 >= 0xc0 && opbyte1 <= 0xc7) do_CPL_insrcbit(opbyte0, opbyte1, srcaddr);
		else if (opbyte1 >= 0xc8 && opbyte1 <= 0xcf) do_LD_insrcbit_CF(opbyte0, opbyte1, srcaddr);
		else if (opbyte1 >= 0xd0 && opbyte1 <= 0xd7) do_XOR_CF_insrcbit(opbyte0, opbyte1, srcaddr);
		else if (opbyte1 >= 0xd8 && opbyte1 <= 0xdf) do_LD_CF_insrcbit(opbyte0, opbyte1, srcaddr);
		else do_e0_to_e7_oprand_illegal(opbyte0, opbyte1, srcaddr);
		break;
	}
}

void Tlcs870::do_e0_to_e7_oprand_illegal(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr)
{
	m_cycles += 1;
	m_stats.illegal_src_opcode++;
	m_stats.last_illegal_pc = m_tmppc;
	logf("%04x: illegal src prefix opcode %02x %02x (src addr %04x)", m_tmppc, opbyte0, opbyte1, srcaddr);
}

void Tlcs870::do_LD_rr_insrc(uint8_t, uint8_t opbyte1, uint16_t srcaddr) // LD rr, (src)  1 - - -  +4
{
	m_cycles += 4;
	const uint16_t val = RM16(srcaddr);
	set_reg16(opbyte1 & 0x3, val);
	set_JF();
}

void Tlcs870::do_INC_insrc(uint8_t, uint8_t, uint16_t srcaddr) // INC (src)  C Z - -  +4
{
	m_cycles += 4;
	uint8_t val = RM8(srcaddr);
	val++;

	if (val == 0)
	{
		set_ZF();
		set_JF();
	}
	else
	{
		clear_ZF();
		clear_JF();
	}
	WM8(srcaddr, val);
}

void Tlcs870::do_DEC_insrc(uint8_t, uint8_t, uint16_t srcaddr) // DEC (src)  C Z - -  +4
{
	m_cycles += 4;
	uint8_t val = RM8(srcaddr);
	val--;

	if (val == 0xff) set_JF();
	else clear_JF();

	if (val == 0x00) set_ZF();
	else clear_ZF();

	WM8(srcaddr, val);
}

void Tlcs870::do_ROLD_A_insrc(uint8_t, uint8_t, uint16_t srcaddr) // ROLD A,(src)  1 0 0 0  +7
{
	m_cycles += 7;
	const uint8_t val = RM8(srcaddr);
	const uint8_t reg = get_reg8(REG_A);

	uint8_t tempval = (val & 0x0f) << 4;
	tempval |= reg & 0x0f;
	const uint8_t tempa = (reg & 0xf0) | (val & 0xf0) >> 4;

	set_reg8(REG_A, tempa);
	WM8(srcaddr, tempval);
	set_JF();
}

void Tlcs870::do_RORD_A_insrc(uint8_t, uint8_t, uint16_t srcaddr) // RORD A,(src)  1 0 0 0  +7
{
	m_cycles += 7;
	const uint8_t val = RM8(srcaddr);
	const uint8_t reg = get_reg8(REG_A);

	uint8_t tempval = (val & 0xf0) >> 4;
	tempval |= ((reg & 0x0f) << 4);
	const uint8_t tempa = (reg & 0xf0) | (val & 0x0f);

	set_reg8(REG_A, tempa);
	WM8(srcaddr, tempval);
	set_JF();
}

void Tlcs870::do_LD_inx_insrc(uint8_t, uint8_t, uint16_t srcaddr) // LD (x),(src)  1 U - -  +5
{
	m_cycles += 5;
	const uint16_t x = READ8(); // get address x
	const uint8_t val = RM8(srcaddr);
	WM8(x, val);
	set_JF(); // z-flag is undefined, check real behavior
}

void Tlcs870::do_LD_inHL_insrc(uint8_t, uint8_t, uint16_t srcaddr) // LD (HL),(src)  1 Z - -  +4
{
	m_cycles += 4;
	const uint8_t val = RM8(srcaddr);
	const uint16_t dstaddr = get_reg16(REG_HL);
	WM8(dstaddr, val);
	set_JF();
	if (val == 0x00) set_ZF();
	else clear_ZF();
}

void Tlcs870::do_LD_r_insrc(uint8_t, uint8_t opbyte1, uint16_t srcaddr) // LD r, (src)  1 Z - -  +3
{
	m_cycles += 3;
	const uint8_t val = RM8(srcaddr);
	set_reg8(opbyte1 & 0x7, val);
	set_JF();
	if (val == 0x00) set_ZF();
	else clear_ZF();
}

void Tlcs870::do_MCMP_insrc_n(uint8_t, uint8_t, uint16_t srcaddr) // MCMP (src), n  Z Z C H  +5
{
	m_cycles += 5;
	const uint8_t n = READ8();
	const uint8_t val = RM8(srcaddr);
	const uint8_t temp = n & val;
	const uint8_t a = get_reg8(REG_A);

	if (a == temp)
	{
		set_ZF();
		set_JF();
	}
	else
	{
		clear_ZF();
		clear_JF();
	}

	if (a < temp) set_CF();
	else clear_CF();
	// apparently H gets set conditionally too? (like CMP?)  (not done in MAME)
}

void Tlcs870::do_XCH_r_insrc(uint8_t, uint8_t opbyte1, uint16_t srcaddr) // XCH r,(src)  1 Z - -  +4
{
	m_cycles += 4;
	m_read_input_port = 0; // reads output latch, not actual ports if accessing memory mapped ports
	const uint8_t val = RM8(srcaddr);
	const uint8_t temp = get_reg8(opbyte1 & 0x7);

	WM8(srcaddr, temp);
	set_reg8(opbyte1 & 0x7, val);

	set_JF();
	if (val == 0) set_ZF();
	else clear_ZF();
}

void Tlcs870::do_ALUOP_insrc_inHL(uint8_t, uint8_t opbyte1, uint16_t srcaddr) // (ALU) (src), (HL)  +6 (CMP), +5 otherwise
{
	m_cycles += 6;
	const int aluop = (opbyte1 & 0x7);
	if (aluop != 0x07)
		m_read_input_port = 0; // reads output latch, not actual ports if accessing memory mapped ports
	const uint8_t val = RM8(srcaddr);
	m_read_input_port = 1;

	const uint16_t HL = get_reg16(REG_HL);
	const uint8_t result = do_alu_8bit(aluop, val, RM8(HL));

	if (aluop != 0x07) // CMP doesn't write back
	{
		m_cycles -= 1; // one less for CMP here?  (sic, MAME)
		WM8(srcaddr, result);
	}
}

void Tlcs870::do_ALUOP_insrc_n(uint8_t, uint8_t opbyte1, uint16_t srcaddr) // (ALU) (src), n  +5 (CMP), +4 otherwise
{
	m_cycles += 5;
	const uint8_t n = READ8();
	const int aluop = (opbyte1 & 0x7);

	if (aluop != 0x07)
		m_read_input_port = 0;

	const uint8_t val = RM8(srcaddr);
	const uint8_t result = do_alu_8bit(aluop, val, n);

	if (aluop != 0x07)
	{
		m_cycles -= 1; // (sic, MAME)
		WM8(srcaddr, result);
	}
}

void Tlcs870::do_ALUOP_A_insrc(uint8_t, uint8_t opbyte1, uint16_t srcaddr) // (ALU) A, (src)  +3
{
	m_cycles += 3;
	const int aluop = (opbyte1 & 0x7);
	const uint8_t val = RM8(srcaddr);
	const uint8_t result = do_alu_8bit(aluop, get_reg8(REG_A), val);
	if (aluop != 0x07)
		set_reg8(REG_A, result);
}

void Tlcs870::do_CALL_insrc(uint8_t, uint8_t, uint16_t srcaddr) // CALL (src)  - - - -  +8
{
	m_cycles += 8;
	const uint16_t val = RM16(srcaddr);
	WM16(m_sp - 1, uint16_t(m_addr));
	m_sp -= 2;
	m_addr = val;
}

void Tlcs870::do_JP_insrc(uint8_t, uint8_t, uint16_t srcaddr) // JP (src)  1 - - -  +5
{
	m_cycles += 5;
	const uint16_t val = RM16(srcaddr);
	m_addr = val;
	set_JF();
}

void Tlcs870::do_XOR_CF_insrcbit(uint8_t, uint8_t opbyte1, uint16_t srcaddr) // XOR CF,(src).b  ~C - * -  +3
{
	m_cycles += 3;
	const uint8_t val = RM8(srcaddr);
	const uint8_t bit = val & (1 << (opbyte1 & 0x7));

	if (is_CF())
	{
		if (bit) clear_CF();
		else set_CF();
	}
	else
	{
		if (bit) set_CF();
		else clear_CF();
	}

	// JF ends up being whatever the new value of CF is
	if (is_CF()) set_JF();
	else clear_JF();
}

void Tlcs870::do_LD_insrcbit_CF(uint8_t, uint8_t opbyte1, uint16_t srcaddr) // LD (src).b,CF  1 - - -  +4
{
	m_cycles += 4;
	m_read_input_port = 0;
	uint8_t val = RM8(srcaddr);
	const int bitused = (1 << (opbyte1 & 0x7));

	if (is_CF()) val |= bitused;
	else val &= ~bitused;

	set_JF();
	WM8(srcaddr, val);
}

void Tlcs870::do_CPL_insrcbit(uint8_t, uint8_t opbyte1, uint16_t srcaddr) // CPL (src).b  Z * - -  +4
{
	m_cycles += 4;
	m_read_input_port = 0;
	uint8_t val = RM8(srcaddr);
	const int bitused = (1 << (opbyte1 & 0x7));

	if (val & bitused)
	{
		clear_ZF();
		clear_JF();
		val &= ~bitused;
	}
	else
	{
		set_ZF();
		set_JF();
		val |= bitused;
	}
	WM8(srcaddr, val);
}

void Tlcs870::do_LD_CF_insrcbit(uint8_t, uint8_t opbyte1, uint16_t srcaddr) // LD CF,(src).b  ~C - * -  +3
{
	m_cycles += 3;
	const uint8_t val = RM8(srcaddr);
	const uint8_t bit = val & (1 << (opbyte1 & 0x7));
	bit ? set_CF() : clear_CF();
	bit ? clear_JF() : set_JF();
}

void Tlcs870::do_SET_insrcbit(uint8_t, uint8_t opbyte1, uint16_t srcaddr) // SET (src).b  Z * - -  +4
{
	m_cycles += 4;
	m_read_input_port = 0;
	uint8_t val = RM8(srcaddr);
	const int bitused = (1 << (opbyte1 & 0x7));

	if (val & bitused)
	{
		clear_ZF();
		clear_JF();
	}
	else
	{
		set_ZF();
		set_JF();
	}
	val |= bitused;
	WM8(srcaddr, val);
}

void Tlcs870::do_CLR_insrcbit(uint8_t, uint8_t opbyte1, uint16_t srcaddr) // CLR (src).b  Z * - -  +4
{
	m_cycles += 4;
	m_read_input_port = 0;
	uint8_t val = RM8(srcaddr);
	const int bitused = (1 << (opbyte1 & 0x7));

	if (val & bitused)
	{
		clear_ZF();
		clear_JF();
	}
	else
	{
		set_ZF();
		set_JF();
	}
	val &= ~bitused;
	WM8(srcaddr, val);
}

/*************************************************************************************************************
    (dst) F0-F7
*************************************************************************************************************/

void Tlcs870::do_dstprefixtype_opcode(uint8_t opbyte0)
{
	m_cycles = uint8_t(get_base_srcdst_cycles(opbyte0 & 0x7));

	uint16_t dstaddr;
	if ((opbyte0 == 0xf0) || (opbyte0 == 0xf4))
		dstaddr = get_addr((opbyte0 & 0x7), READ8());
	else
		dstaddr = get_addr((opbyte0 & 0x7), 0);

	const uint8_t opbyte1 = READ8();

	// these are illegal prefixes for dst, undefined behavior at least (MAME then still executes the op)
	if ((opbyte0 == 0xf1) || (opbyte0 == 0xf5))
		do_f0_to_f7_oprand_illegal_opcode(opbyte0, opbyte1, dstaddr);

	switch (opbyte1)
	{
	case 0x10: case 0x11: case 0x12: case 0x13: do_LD_indst_rr(opbyte0, opbyte1, dstaddr); break;
	case 0x2c: do_LD_indst_n(opbyte0, opbyte1, dstaddr); break;
	case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
		do_LD_indst_r(opbyte0, opbyte1, dstaddr); break;
	default: do_f0_to_f7_oprand_illegal_opcode(opbyte0, opbyte1, dstaddr); break;
	}
}

void Tlcs870::do_f0_to_f7_oprand_illegal_opcode(uint8_t opbyte0, uint8_t opbyte1, uint16_t dstaddr)
{
	m_cycles += 1;
	m_stats.illegal_dst_opcode++;
	m_stats.last_illegal_pc = m_tmppc;
	logf("%04x: illegal dst prefix opcode %02x %02x (dst addr %04x)", m_tmppc, opbyte0, opbyte1, dstaddr);
}

void Tlcs870::do_LD_indst_rr(uint8_t, uint8_t opbyte1, uint16_t dstaddr) // LD (dst),rr  1 - - -  +4
{
	m_cycles += 4;
	const uint16_t val = get_reg16(opbyte1 & 0x3);
	WM16(dstaddr, val);
	set_JF();
}

void Tlcs870::do_LD_indst_n(uint8_t, uint8_t, uint16_t dstaddr) // LD (dst),n  1 - - -  +4
{
	m_cycles += 4;
	const uint16_t n = READ8();
	WM8(dstaddr, uint8_t(n));
	set_JF();
}

void Tlcs870::do_LD_indst_r(uint8_t, uint8_t opbyte1, uint16_t dstaddr) // LD (dst),r  1 - - -  +3
{
	m_cycles += 3;
	const uint8_t reg = get_reg8(opbyte1 & 0x7);
	WM8(dstaddr, reg);
	set_JF();
}

/*************************************************************************************************************
    (reg) E8-EF
*************************************************************************************************************/

void Tlcs870::do_regprefixtype_opcode(uint8_t opbyte0)
{
	const uint8_t opbyte1 = READ8();

	switch (opbyte1)
	{
	case 0x01: do_SWAP_g(opbyte0, opbyte1); break;
	case 0x02: do_MUL_gg(opbyte0, opbyte1); break;
	case 0x03: do_DIV_gg_C(opbyte0, opbyte1); break;
	case 0x04: do_RETN(opbyte0, opbyte1); break;
	case 0x06: do_POP_gg(opbyte0, opbyte1); break;
	case 0x07: do_PUSH_gg(opbyte0, opbyte1); break;
	case 0x0a: do_DAA_g(opbyte0, opbyte1); break;
	case 0x0b: do_DAS_g(opbyte0, opbyte1); break;
	case 0x10: case 0x11: case 0x12: case 0x13: do_XCH_rr_gg(opbyte0, opbyte1); break;
	case 0x14: case 0x15: case 0x16: case 0x17: do_LD_rr_gg(opbyte0, opbyte1); break;
	case 0x1c: do_SHLC_g(opbyte0, opbyte1); break;
	case 0x1d: do_SHRC_g(opbyte0, opbyte1); break;
	case 0x1e: do_ROLC_g(opbyte0, opbyte1); break;
	case 0x1f: do_RORC_g(opbyte0, opbyte1); break;
	case 0x82: case 0x83: do_SET_inppbit(opbyte0, opbyte1); break;
	case 0x8a: case 0x8b: do_CLR_inppbit(opbyte0, opbyte1); break;
	case 0x92: case 0x93: do_CPL_inpp_indirectbit(opbyte0, opbyte1); break;
	case 0x9a: case 0x9b: do_LD_inpp_indirectbit_CF(opbyte0, opbyte1); break;
	case 0x9e: case 0x9f: do_LD_CF_inpp_indirectbit(opbyte0, opbyte1); break;
	case 0xfa: do_LD_SP_gg(opbyte0, opbyte1); break;
	case 0xfb: do_LD_gg_SP(opbyte0, opbyte1); break;
	case 0xfc: do_CALL_gg(opbyte0, opbyte1); break;
	case 0xfe: do_JP_gg(opbyte0, opbyte1); break;
	default:
		if (opbyte1 >= 0x30 && opbyte1 <= 0x37) do_ALUOP_WA_gg(opbyte0, opbyte1);
		else if (opbyte1 >= 0x38 && opbyte1 <= 0x3f) do_ALUOP_gg_mn(opbyte0, opbyte1);
		else if (opbyte1 >= 0x40 && opbyte1 <= 0x47) do_SET_gbit(opbyte0, opbyte1);
		else if (opbyte1 >= 0x48 && opbyte1 <= 0x4f) do_CLR_gbit(opbyte0, opbyte1);
		else if (opbyte1 >= 0x58 && opbyte1 <= 0x5f) do_LD_r_g(opbyte0, opbyte1);
		else if (opbyte1 >= 0x60 && opbyte1 <= 0x67) do_ALUOP_A_g(opbyte0, opbyte1);
		else if (opbyte1 >= 0x68 && opbyte1 <= 0x6f) do_ALUOP_g_A(opbyte0, opbyte1);
		else if (opbyte1 >= 0x70 && opbyte1 <= 0x77) do_ALUOP_g_n(opbyte0, opbyte1);
		else if (opbyte1 >= 0xa8 && opbyte1 <= 0xaf) do_XCH_r_g(opbyte0, opbyte1);
		else if (opbyte1 >= 0xc0 && opbyte1 <= 0xc7) do_CPL_gbit(opbyte0, opbyte1);
		else if (opbyte1 >= 0xc8 && opbyte1 <= 0xcf) do_LD_gbit_CF(opbyte0, opbyte1);
		else if (opbyte1 >= 0xd0 && opbyte1 <= 0xd7) do_XOR_CF_gbit(opbyte0, opbyte1);
		else if (opbyte1 >= 0xd8 && opbyte1 <= 0xdf) do_LD_CF_gbit(opbyte0, opbyte1);
		else do_regprefixtype_oprand_illegal(opbyte0, opbyte1);
		break;
	}
}

void Tlcs870::do_regprefixtype_oprand_illegal(uint8_t opbyte0, uint8_t opbyte1)
{
	m_cycles = 1;
	m_stats.illegal_reg_opcode++;
	m_stats.last_illegal_pc = m_tmppc;
	logf("%04x: illegal reg prefix opcode %02x %02x", m_tmppc, opbyte0, opbyte1);
}

void Tlcs870::do_RETN(uint8_t opbyte0, uint8_t opbyte1) // RETN (E8 04)  * * * *  7
{
	if (opbyte0 == 0xe8)
	{
		m_cycles = 7;
		m_sp += 3;
		m_addr = RM16(m_sp - 2);
		set_PSW(RM8(fix_mame_bugs ? m_sp : m_sp - 1)); // MAME quirk: SP-1
	}
	else
	{
		do_regprefixtype_oprand_illegal(opbyte0, opbyte1);
	}
}

void Tlcs870::do_SWAP_g(uint8_t opbyte0, uint8_t) { m_cycles = 4; handle_swap(opbyte0 & 0x7); }                                           // 1 - - -  4
void Tlcs870::do_DAA_g(uint8_t opbyte0, uint8_t) { m_cycles = 3; set_reg8(opbyte0 & 0x7, handle_DAA(get_reg8(opbyte0 & 0x7))); }     // C Z C H  3
void Tlcs870::do_DAS_g(uint8_t opbyte0, uint8_t) { m_cycles = 3; set_reg8(opbyte0 & 0x7, handle_DAS(get_reg8(opbyte0 & 0x7))); }     // C Z C H  3
void Tlcs870::do_SHLC_g(uint8_t opbyte0, uint8_t) { m_cycles = 2; set_reg8(opbyte0 & 0x7, handle_SHLC(get_reg8(opbyte0 & 0x7))); }   // C Z * -  2
void Tlcs870::do_SHRC_g(uint8_t opbyte0, uint8_t) { m_cycles = 2; set_reg8(opbyte0 & 0x7, handle_SHRC(get_reg8(opbyte0 & 0x7))); }   // C Z * -  2
void Tlcs870::do_ROLC_g(uint8_t opbyte0, uint8_t) { m_cycles = 2; set_reg8(opbyte0 & 0x7, handle_ROLC(get_reg8(opbyte0 & 0x7))); }   // C Z * -  2
void Tlcs870::do_RORC_g(uint8_t opbyte0, uint8_t) { m_cycles = 2; set_reg8(opbyte0 & 0x7, handle_RORC(get_reg8(opbyte0 & 0x7))); }   // C Z * -  2

void Tlcs870::do_LD_r_g(uint8_t opbyte0, uint8_t opbyte1) // LD r, g  1 Z - -  2
{
	m_cycles = 2;
	const uint8_t val = get_reg8(opbyte0 & 0x7);
	set_reg8(opbyte1 & 0x7, val);
	set_JF();
	if (val == 0x00) set_ZF();
	else clear_ZF();
}

void Tlcs870::do_XCH_r_g(uint8_t opbyte0, uint8_t opbyte1) // XCH r, g  1 Z - -  3
{
	m_cycles = 3;
	const uint8_t val = get_reg8(opbyte0 & 0x7);
	const uint8_t r = get_reg8(opbyte1 & 0x7);
	set_reg8(opbyte1 & 0x7, val);
	set_reg8(opbyte0 & 0x7, r);
	set_JF();
	if (val == 0x00) set_ZF();
	else clear_ZF();
}

void Tlcs870::do_ALUOP_A_g(uint8_t opbyte0, uint8_t opbyte1) // (ALU) A, g  2
{
	m_cycles = 2;
	const int aluop = (opbyte1 & 0x7);
	const uint8_t result = do_alu_8bit(aluop, get_reg8(REG_A), get_reg8(opbyte0 & 0x7));
	if (aluop != 0x07)
		set_reg8(REG_A, result);
}

void Tlcs870::do_ALUOP_g_A(uint8_t opbyte0, uint8_t opbyte1) // (ALU) g, A  3
{
	m_cycles = 3;
	const int aluop = (opbyte1 & 0x7);
	const uint8_t result = do_alu_8bit(aluop, get_reg8(opbyte0 & 0x7), get_reg8(REG_A));
	if (aluop != 0x07)
		set_reg8(opbyte0 & 0x7, result);
}

void Tlcs870::do_ALUOP_g_n(uint8_t opbyte0, uint8_t opbyte1) // (ALU) g, n  3
{
	m_cycles = 3;
	const int aluop = (opbyte1 & 0x7);
	const uint8_t n = READ8();
	const uint8_t result = do_alu_8bit(aluop, get_reg8(opbyte0 & 0x7), n);
	if (aluop != 0x07)
		set_reg8(opbyte0 & 0x7, result);
}

void Tlcs870::do_ALUOP_WA_gg(uint8_t opbyte0, uint8_t opbyte1) // (ALU) WA, gg  4
{
	m_cycles = 4;
	const int aluop = (opbyte1 & 0x7);
	const uint16_t result = do_alu_16bit(aluop, get_reg16(REG_WA), get_reg16(opbyte0 & 0x3));
	if (aluop != 0x07)
		set_reg16(REG_WA, result);
}

void Tlcs870::do_ALUOP_gg_mn(uint8_t opbyte0, uint8_t opbyte1) // (ALU) gg, mn  4
{
	m_cycles = 4;
	const int aluop = (opbyte1 & 0x7);
	const uint16_t mn = READ16();
	const uint16_t result = do_alu_16bit(aluop, get_reg16(opbyte0 & 0x3), mn);
	if (aluop != 0x07)
		set_reg16(opbyte0 & 0x3, result);
}

// ops using (pp).g

void Tlcs870::do_SET_inppbit(uint8_t opbyte0, uint8_t opbyte1) // SET (pp).g  Z * - -  5
{
	m_cycles = 5;
	const uint8_t bitused = uint8_t(1 << (get_reg8(opbyte0 & 7) & 0x7));
	const uint16_t addr = get_reg16((opbyte1 & 1) + 2); // DE or HL
	uint8_t val = RM8(addr); // (MAME does not force the output-latch read here)

	if (val & bitused)
	{
		clear_ZF();
		clear_JF();
	}
	else
	{
		set_ZF();
		set_JF();
	}
	val |= bitused;
	WM8(addr, val);
}

void Tlcs870::do_CLR_inppbit(uint8_t opbyte0, uint8_t opbyte1) // CLR (pp).g  Z * - -  5
{
	m_cycles = 5;
	const uint8_t bitused = uint8_t(1 << (get_reg8(opbyte0 & 7) & 0x7));
	const uint16_t addr = get_reg16((opbyte1 & 1) + 2);
	m_read_input_port = 0;
	uint8_t val = RM8(addr);

	if (val & bitused)
	{
		clear_ZF();
		clear_JF();
	}
	else
	{
		set_ZF();
		set_JF();
	}
	val &= ~bitused;
	WM8(addr, val);
}

void Tlcs870::do_CPL_inpp_indirectbit(uint8_t opbyte0, uint8_t opbyte1) // CPL (pp).g  Z * - -  5
{
	m_cycles = 5;
	const uint8_t bitused = uint8_t(1 << (get_reg8(opbyte0 & 7) & 0x7));
	const uint16_t addr = get_reg16((opbyte1 & 1) + 2);
	m_read_input_port = 0;
	uint8_t val = RM8(addr);

	if (val & bitused)
	{
		clear_ZF();
		clear_JF();
		val &= ~bitused;
	}
	else
	{
		set_ZF();
		set_JF();
		val |= bitused;
	}
	WM8(addr, val);
}

void Tlcs870::do_LD_inpp_indirectbit_CF(uint8_t opbyte0, uint8_t opbyte1) // LD (pp).g, CF  1 - - -  5
{
	m_cycles = 5;
	const uint8_t bitused = uint8_t(1 << (get_reg8(opbyte0 & 7) & 0x7));
	const uint16_t addr = get_reg16((opbyte1 & 1) + 2);
	uint8_t val = RM8(addr); // (MAME does not force the output-latch read here)

	if (is_CF()) val |= bitused;
	else val &= ~bitused;

	set_JF();
	WM8(addr, val);
}

void Tlcs870::do_LD_CF_inpp_indirectbit(uint8_t opbyte0, uint8_t opbyte1) // LD CF, (pp).g  ~C - * -  4
{
	m_cycles = 4;
	const uint8_t bitused = uint8_t(1 << (get_reg8(opbyte0 & 7) & 0x7));
	const uint16_t addr = get_reg16((opbyte1 & 1) + 2);
	const uint8_t val = RM8(addr);
	const uint8_t bit = val & bitused;
	bit ? set_CF() : clear_CF();
	bit ? clear_JF() : set_JF();
}

// ops using g.b

void Tlcs870::do_SET_gbit(uint8_t opbyte0, uint8_t opbyte1) // SET g.b  Z * - -  3
{
	m_cycles = 3;
	uint8_t val = get_reg8(opbyte0 & 0x7);
	const int bitused = (1 << (opbyte1 & 0x7));

	if (val & bitused)
	{
		clear_ZF();
		clear_JF();
	}
	else
	{
		set_ZF();
		set_JF();
	}
	val |= bitused;
	set_reg8(opbyte0 & 0x7, val);
}

void Tlcs870::do_CLR_gbit(uint8_t opbyte0, uint8_t opbyte1) // CLR g.b  Z * - -  3
{
	m_cycles = 3;
	uint8_t val = get_reg8(opbyte0 & 0x7);
	const int bitused = (1 << (opbyte1 & 0x7));

	if (val & bitused)
	{
		clear_ZF();
		clear_JF();
	}
	else
	{
		set_ZF();
		set_JF();
	}
	val &= ~bitused;
	set_reg8(opbyte0 & 0x7, val);
}

void Tlcs870::do_CPL_gbit(uint8_t opbyte0, uint8_t opbyte1) // CPL g.b  Z * - -  3
{
	m_cycles = 3;
	uint8_t val = get_reg8(opbyte0 & 0x7);
	const int bitused = (1 << (opbyte1 & 0x7));

	if (val & bitused)
	{
		clear_ZF();
		clear_JF();
		val &= ~bitused;
	}
	else
	{
		set_ZF();
		set_JF();
		val |= bitused;
	}
	set_reg8(opbyte0 & 0x7, val);
}

void Tlcs870::do_LD_gbit_CF(uint8_t opbyte0, uint8_t opbyte1) // LD g.b, CF  1 - - -  2
{
	m_cycles = 2;
	uint8_t val = get_reg8(opbyte0 & 0x7);
	const int bitused = (1 << (opbyte1 & 0x7));

	if (is_CF()) val |= bitused;
	else val &= ~bitused;

	set_JF();
	set_reg8(opbyte0 & 0x7, val);
}

void Tlcs870::do_XOR_CF_gbit(uint8_t opbyte0, uint8_t opbyte1) // XOR CF, g.b  ~C - * -  2
{
	m_cycles = 2;
	const uint8_t bit = get_reg8(opbyte0 & 0x7) & (1 << (opbyte1 & 0x7));

	if (is_CF())
	{
		if (bit) clear_CF();
		else set_CF();
	}
	else
	{
		if (bit) set_CF();
		else clear_CF();
	}

	if (is_CF()) set_JF();
	else clear_JF();
}

void Tlcs870::do_LD_CF_gbit(uint8_t opbyte0, uint8_t opbyte1) // LD CF, g.b  ~C - * -  2
{
	m_cycles = 2;
	const uint8_t bit = get_reg8(opbyte0 & 0x7) & (1 << (opbyte1 & 0x7));
	bit ? set_CF() : clear_CF();
	bit ? clear_JF() : set_JF();
}

// 16-bit

void Tlcs870::do_MUL_gg(uint8_t opbyte0, uint8_t) // MUL ggH, ggL  Z Z - -  8
{
	m_cycles = 8;
	handle_mul(opbyte0 & 0x3); // opbyte0 & 4 = invalid?
}

void Tlcs870::do_DIV_gg_C(uint8_t opbyte0, uint8_t) // DIV gg, C  Z Z C -  8
{
	m_cycles = 8;
	handle_div(opbyte0 & 0x3);
}

void Tlcs870::do_POP_gg(uint8_t opbyte0, uint8_t) // POP gg  - - - -  5
{
	m_cycles = 5;
	m_sp += 2;
	const uint16_t val = RM16(m_sp - 1);
	set_reg16(opbyte0 & 3, val);
}

void Tlcs870::do_PUSH_gg(uint8_t opbyte0, uint8_t) // PUSH gg  - - - -  4
{
	m_cycles = 4;
	const uint16_t val = get_reg16(opbyte0 & 3);
	WM16(m_sp - 1, val);
	m_sp -= 2;
}

void Tlcs870::do_LD_SP_gg(uint8_t opbyte0, uint8_t) // LD SP, gg  1 - - -  3
{
	m_cycles = 3;
	m_sp = get_reg16(opbyte0 & 0x3);
	set_JF();
}

void Tlcs870::do_LD_gg_SP(uint8_t opbyte0, uint8_t) // LD gg, SP  1 - - -  3
{
	m_cycles = 3;
	set_reg16(opbyte0 & 0x3, uint16_t(m_sp));
	set_JF();
}

void Tlcs870::do_LD_rr_gg(uint8_t opbyte0, uint8_t opbyte1) // LD rr, gg  1 - - -  2   (MAME decodes this at 14-17)
{
	m_cycles = 2;
	const uint16_t gg = get_reg16(opbyte0 & 0x3);
	set_reg16(opbyte1 & 0x3, gg);
	set_JF();
}

void Tlcs870::do_XCH_rr_gg(uint8_t opbyte0, uint8_t opbyte1) // XCH rr, gg  1 - - -  3   (MAME decodes this at 10-13)
{
	m_cycles = 3;
	const uint16_t gg = get_reg16(opbyte0 & 0x3);
	const uint16_t rr = get_reg16(opbyte1 & 0x3);
	set_reg16(opbyte1 & 0x3, gg);
	set_reg16(opbyte0 & 0x3, rr);
	set_JF();
}

void Tlcs870::do_CALL_gg(uint8_t opbyte0, uint8_t) // CALL gg  - - - -  6
{
	m_cycles = 6;
	const uint16_t val = get_reg16(opbyte0 & 3);
	WM16(m_sp - 1, uint16_t(m_addr));
	m_sp -= 2;
	m_addr = val;
}

void Tlcs870::do_JP_gg(uint8_t opbyte0, uint8_t) // JP gg  1 - - -  3
{
	m_cycles = 3;
	m_addr = get_reg16(opbyte0 & 3);
	set_JF();
}
