// license:BSD-3-Clause
// copyright-holders:David Haywood
/*************************************************************************************************************

    Toshiba TLCS-870 Series MCUs  (standalone port of MAME's tlcs870 core)

    direct opcodes, no prefix (MAME tlcs870_ops.cpp) + general helpers / ALU (MAME tlcs870_ops_helper.cpp)

    Flag / cycle columns in the comments:  JF ZF CF HF  cycles

*************************************************************************************************************/

#include "tlcs870.h"

// Main dispatch handlers for these

void Tlcs870::decode()
{
	const uint8_t opbyte0 = READ8();

	switch (opbyte0)
	{
	case 0x00: do_NOP(opbyte0); break;
	case 0x01: do_SWAP_A(opbyte0); break;
	case 0x02: do_MUL_W_A(opbyte0); break;
	case 0x03: do_DIV_WA_C(opbyte0); break;
	case 0x04: do_RETI(opbyte0); break;
	case 0x05: do_RET(opbyte0); break;
	case 0x06: do_POP_PSW(opbyte0); break;
	case 0x07: do_PUSH_PSW(opbyte0); break;
	case 0x0a: do_DAA_A(opbyte0); break;
	case 0x0b: do_DAS_A(opbyte0); break;
	case 0x0c: do_CLR_CF(opbyte0); break;
	case 0x0d: do_SET_CF(opbyte0); break;
	case 0x0e: do_CPL_CF(opbyte0); break;
	case 0x0f: do_LD_RBS_n(opbyte0); break;
	case 0x10: case 0x11: case 0x12: case 0x13: do_INC_rr(opbyte0); break;
	case 0x14: case 0x15: case 0x16: case 0x17: do_LD_rr_mn(opbyte0); break;
	case 0x18: case 0x19: case 0x1a: case 0x1b: do_DEC_rr(opbyte0); break;
	case 0x1c: do_SHLC_A(opbyte0); break;
	case 0x1d: do_SHRC_A(opbyte0); break;
	case 0x1e: do_ROLC_A(opbyte0); break;
	case 0x1f: do_RORC_A(opbyte0); break;
	case 0x20: do_INC_inx(opbyte0); break;
	case 0x21: do_INC_inHL(opbyte0); break;
	case 0x22: do_LD_A_inx(opbyte0); break;
	case 0x23: do_LD_A_inHL(opbyte0); break;
	case 0x24: do_LDW_inx_mn(opbyte0); break;
	case 0x25: do_LDW_inHL_mn(opbyte0); break;
	case 0x26: do_LD_inx_iny(opbyte0); break;
	case 0x28: do_DEC_inx(opbyte0); break;
	case 0x29: do_DEC_inHL(opbyte0); break;
	case 0x2a: do_LD_inx_A(opbyte0); break;
	case 0x2b: do_LD_inHL_A(opbyte0); break;
	case 0x2c: do_LD_inx_n(opbyte0); break;
	case 0x2d: do_LD_inHL_n(opbyte0); break;
	case 0x2e: do_CLR_inx(opbyte0); break;
	case 0x2f: do_CLR_inHL(opbyte0); break;
	default:
		if (opbyte0 >= 0x30 && opbyte0 <= 0x37) do_LD_r_n(opbyte0);
		else if (opbyte0 >= 0x40 && opbyte0 <= 0x47) do_SET_inxbit(opbyte0);
		else if (opbyte0 >= 0x48 && opbyte0 <= 0x4f) do_CLR_inxbit(opbyte0);
		else if (opbyte0 >= 0x50 && opbyte0 <= 0x57) do_LD_A_r(opbyte0);
		else if (opbyte0 >= 0x58 && opbyte0 <= 0x5f) do_LD_r_A(opbyte0);
		else if (opbyte0 >= 0x60 && opbyte0 <= 0x67) do_INC_r(opbyte0);
		else if (opbyte0 >= 0x68 && opbyte0 <= 0x6f) do_DEC_r(opbyte0);
		else if (opbyte0 >= 0x70 && opbyte0 <= 0x77) do_ALUOP_A_n(opbyte0);
		else if (opbyte0 >= 0x78 && opbyte0 <= 0x7f) do_ALUOP_A_inx(opbyte0);
		else if (opbyte0 >= 0x80 && opbyte0 <= 0x9f) do_JRS_T_a(opbyte0);
		else if (opbyte0 >= 0xa0 && opbyte0 <= 0xbf) do_JRS_F_a(opbyte0);
		else if (opbyte0 >= 0xc0 && opbyte0 <= 0xcf) do_CALLV_n(opbyte0);
		else if (opbyte0 >= 0xd0 && opbyte0 <= 0xd7) do_JR_cc_a(opbyte0);
		else if (opbyte0 >= 0xd8 && opbyte0 <= 0xdf) do_LD_CF_inxbit(opbyte0);
		else if (opbyte0 >= 0xe0 && opbyte0 <= 0xe7) do_srcprefixtype_opcode(opbyte0);
		else if (opbyte0 >= 0xe8 && opbyte0 <= 0xef) do_regprefixtype_opcode(opbyte0);
		else if (opbyte0 >= 0xf0 && opbyte0 <= 0xf7) do_dstprefixtype_opcode(opbyte0);
		else if (opbyte0 == 0xfa) do_LD_SP_mn(opbyte0);
		else if (opbyte0 == 0xfb) do_JR_a(opbyte0);
		else if (opbyte0 == 0xfc) do_CALL_mn(opbyte0);
		else if (opbyte0 == 0xfd) do_CALLP_n(opbyte0);
		else if (opbyte0 == 0xfe) do_JP_mn(opbyte0);
		else if (opbyte0 == 0xff) do_ff_opcode(opbyte0);
		else do_illegal(opbyte0); // 08, 09, 27, 38-3f, f8, f9
		break;
	}
}

void Tlcs870::do_illegal(uint8_t opbyte0)
{
	m_cycles = 1;
	m_stats.illegal_opcode++;
	m_stats.last_illegal_pc = m_tmppc;
	logf("%04x: illegal opcode %02x", m_tmppc, opbyte0);
}

void Tlcs870::do_NOP(uint8_t) { m_cycles = 1; } // NOP  - - - -  1

void Tlcs870::do_SWAP_A(uint8_t) // SWAP A  1 - - -  3
{
	m_cycles = 3;
	handle_swap(REG_A);
}

void Tlcs870::do_MUL_W_A(uint8_t) // MUL W, A  Z Z - -  7
{
	m_cycles = 7;
	handle_mul(REG_WA);
}

void Tlcs870::do_DIV_WA_C(uint8_t) // DIV WA, C  Z Z C -  7
{
	m_cycles = 7;
	handle_div(REG_WA);
}

void Tlcs870::do_RETI(uint8_t) // RETI  * * * *  6
{
	m_cycles = 6;
	m_sp += 3;
	m_addr = RM16(m_sp - 2);
	set_PSW(RM8(m_sp));
	// Interrupts always get reenabled after a RETI.  The RETN behavior is different
	m_EIR |= 1;
}

void Tlcs870::do_RET(uint8_t) // RET  - - - -  6
{
	m_cycles = 6;
	m_sp += 2;
	m_addr = RM16(m_sp - 1);
}

void Tlcs870::do_POP_PSW(uint8_t) // POP PSW  * * * *  3
{
	m_cycles = 3;
	m_sp += 1;
	const uint8_t val = RM8(m_sp);
	set_PSW(val);
}

void Tlcs870::do_PUSH_PSW(uint8_t) // PUSH PSW  - - - -  2
{
	m_cycles = 2;
	const uint8_t val = get_PSW();
	WM8(m_sp, val);
	m_sp -= 1;
}

void Tlcs870::do_DAA_A(uint8_t) // DAA A  C Z C H  2
{
	m_cycles = 2;
	set_reg8(REG_A, handle_DAA(get_reg8(REG_A)));
}

void Tlcs870::do_DAS_A(uint8_t) // DAS A  C Z C H  2
{
	m_cycles = 2;
	set_reg8(REG_A, handle_DAS(get_reg8(REG_A)));
}

void Tlcs870::do_CLR_CF(uint8_t) // CLR CF  1 - 0 -  1
{
	m_cycles = 1;
	clear_CF();
	set_JF();
}

void Tlcs870::do_SET_CF(uint8_t) // SET CF  0 - 1 -  1
{
	m_cycles = 1;
	set_CF();
	clear_JF();
}

void Tlcs870::do_CPL_CF(uint8_t) // CPL CF  * - * -  1
{
	m_cycles = 1;
	if (is_CF())
	{
		set_JF();
		clear_CF();
	}
	else
	{
		clear_JF();
		set_CF();
	}
}

void Tlcs870::do_LD_RBS_n(uint8_t) // LD RBS, n  1 - - -  4
{
	m_cycles = 4;
	const uint8_t param = READ8();
	m_RBS = param & 0x0f;
	set_JF();
}

void Tlcs870::do_INC_rr(uint8_t opbyte0) // INC rr  C Z - -  2
{
	m_cycles = 2;
	const int reg = opbyte0 & 3;
	uint16_t temp = get_reg16(reg);
	temp++;
	set_reg16(reg, temp);

	if (temp == 0x0000)
	{
		set_ZF();
		set_JF();
	}
	else
	{
		clear_ZF();
		clear_JF();
	}
}

void Tlcs870::do_LD_rr_mn(uint8_t opbyte0) // LD rr,mn  1 - - -  3
{
	m_cycles = 3;
	const uint16_t val = READ16();
	set_reg16(opbyte0 & 3, val);
	set_JF();
}

void Tlcs870::do_DEC_rr(uint8_t opbyte0) // DEC rr  C Z - -  2
{
	m_cycles = 2;
	const int reg = opbyte0 & 3;
	uint16_t temp = get_reg16(reg);
	temp--;
	set_reg16(reg, temp);

	if (temp == 0xffff) set_JF();
	else clear_JF();

	if (temp == 0x0000) set_ZF();
	else clear_ZF();
}

void Tlcs870::do_SHLC_A(uint8_t) { m_cycles = 1; set_reg8(REG_A, handle_SHLC(get_reg8(REG_A))); } // C Z * -  1
void Tlcs870::do_SHRC_A(uint8_t) { m_cycles = 1; set_reg8(REG_A, handle_SHRC(get_reg8(REG_A))); } // C Z * -  1
void Tlcs870::do_ROLC_A(uint8_t) { m_cycles = 1; set_reg8(REG_A, handle_ROLC(get_reg8(REG_A))); } // C Z * -  1
void Tlcs870::do_RORC_A(uint8_t) { m_cycles = 1; set_reg8(REG_A, handle_RORC(get_reg8(REG_A))); } // C Z * -  1

void Tlcs870::do_INC_inx(uint8_t) // INC (x)  C Z - -  5
{
	m_cycles = 5;
	const uint16_t srcaddr = READ8();
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

void Tlcs870::do_INC_inHL(uint8_t) // INC (HL)  C Z - -  4
{
	m_cycles = 4;
	const uint16_t addr = get_reg16(REG_HL);
	uint8_t val = RM8(addr);
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
	WM8(addr, val);
}

void Tlcs870::do_LD_A_inx(uint8_t) // LD A, (x)  1 Z - -  3
{
	m_cycles = 3;
	const uint16_t srcaddr = READ8();
	const uint8_t val = RM8(srcaddr);
	set_reg8(REG_A, val);
	set_JF();
	if (val == 0x00) set_ZF();
	else clear_ZF();
}

void Tlcs870::do_LD_A_inHL(uint8_t) // LD A, (HL)  1 Z - -  2
{
	m_cycles = 2;
	const uint16_t srcaddr = get_reg16(REG_HL);
	const uint8_t val = RM8(srcaddr);
	set_reg8(REG_A, val);
	set_JF();
	if (val == 0x00) set_ZF();
	else clear_ZF();
}

void Tlcs870::do_LD_inx_iny(uint8_t) // LD (x), (y)  1 Z - -  5
{
	m_cycles = 5;
	const uint16_t srcaddr = READ8();
	const uint16_t dstaddr = READ8();
	const uint8_t val = RM8(srcaddr);
	set_JF();
	if (val == 0x00) set_ZF();
	else clear_ZF();
	WM8(dstaddr, val);
}

void Tlcs870::do_DEC_inx(uint8_t) // DEC (x)  C Z - -  5
{
	m_cycles = 5;
	const uint16_t addr = READ8();
	uint8_t temp = RM8(addr);
	temp--;
	WM8(addr, temp);

	if (temp == 0xff) set_JF();
	else clear_JF();

	if (temp == 0x00) set_ZF();
	else clear_ZF();
}

void Tlcs870::do_DEC_inHL(uint8_t) // DEC (HL)  C Z - -  4
{
	m_cycles = 4;
	const uint16_t addr = get_reg16(REG_HL);
	uint8_t temp = RM8(addr);
	temp--;
	WM8(addr, temp);

	if (temp == 0xff) set_JF();
	else clear_JF();

	if (temp == 0x00) set_ZF();
	else clear_ZF();
}

void Tlcs870::do_LD_inx_A(uint8_t) // LD (x), A  1 - - -  3   (MAME also updates ZF)
{
	m_cycles = 3;
	const uint16_t dstaddr = READ8();
	const uint8_t val = get_reg8(REG_A);
	set_JF();
	if (val == 0x00) set_ZF();
	else clear_ZF();
	WM8(dstaddr, val);
}

void Tlcs870::do_LD_inHL_A(uint8_t) // LD (HL), A  1 - - -  2
{
	m_cycles = 2;
	const uint8_t val = get_reg8(REG_A);
	set_JF();
	const uint16_t addr = get_reg16(REG_HL);
	WM8(addr, val);
}

void Tlcs870::do_LD_inx_n(uint8_t) // LD (x), n  1 - - -  4
{
	m_cycles = 4;
	const uint16_t dstaddr = READ8();
	const uint8_t val = READ8();
	set_JF();
	WM8(dstaddr, val);
}

void Tlcs870::do_LD_inHL_n(uint8_t) // LD (HL), n  1 - - -  3
{
	m_cycles = 3;
	const uint8_t val = READ8();
	const uint16_t addr = get_reg16(REG_HL);
	set_JF();
	WM8(addr, val);
}

void Tlcs870::do_CLR_inx(uint8_t) // CLR (x)  1 - - -  4
{
	m_cycles = 4;
	const uint16_t addr = READ8();
	WM8(addr, 0);
	set_JF();
}

void Tlcs870::do_CLR_inHL(uint8_t) // CLR (HL)  1 - - -  2
{
	m_cycles = 2;
	const uint16_t addr = get_reg16(REG_HL);
	WM8(addr, 0);
	set_JF();
}

void Tlcs870::do_LD_r_n(uint8_t opbyte0) // LD r,n  1 - - -  2
{
	m_cycles = 2;
	const uint8_t param1 = opbyte0 & 7;
	const uint8_t param2 = READ8();
	set_reg8(param1, param2);
	set_JF();
}

void Tlcs870::do_SET_inxbit(uint8_t opbyte0) // SET (x).b  Z * - -  5   ((opbyte0 == 0x40) && (x == 0x3a) is EI)
{
	m_cycles = 5;
	const uint8_t srcaddr = READ8();

	m_read_input_port = 0; // reads output latch, not actual ports if accessing memory mapped ports
	uint8_t val = RM8(srcaddr);
	const uint8_t bitpos = opbyte0 & 0x7;
	const int bitused = (1 << bitpos);

	if (val & bitused) // Zero flag gets set based on original value of bit?
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

void Tlcs870::do_CLR_inxbit(uint8_t opbyte0) // CLR (x).b  Z * - -  5   ((opbyte0 == 0x48) && (x == 0x3a) is DI)
{
	m_cycles = 5;
	const uint8_t srcaddr = READ8();

	m_read_input_port = 0; // not sure, might read
	uint8_t val = RM8(srcaddr);
	const uint8_t bitpos = opbyte0 & 0x7;
	const int bitused = (1 << bitpos);

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

void Tlcs870::do_LD_A_r(uint8_t opbyte0) // LD A, r  1 Z - -  1
{
	m_cycles = 1;
	const uint8_t val = get_reg8(opbyte0 & 0x7);
	set_reg8(REG_A, val);
	set_JF();
	if (val == 0x00) set_ZF();
	else clear_ZF();
}

void Tlcs870::do_LD_r_A(uint8_t opbyte0) // LD r, A  1 Z - -  1
{
	m_cycles = 1;
	const uint8_t val = get_reg8(REG_A);
	set_reg8(opbyte0 & 0x7, val);
	set_JF();
	if (val == 0x00) set_ZF();
	else clear_ZF();
}

void Tlcs870::do_INC_r(uint8_t opbyte0) // INC r  C Z - -  1
{
	m_cycles = 1;
	const int reg = opbyte0 & 7;
	uint8_t temp = get_reg8(reg);
	temp++;
	set_reg8(reg, temp);

	if (temp == 0x00)
	{
		set_ZF();
		set_JF();
	}
	else
	{
		clear_ZF();
		clear_JF();
	}
}

void Tlcs870::do_DEC_r(uint8_t opbyte0) // DEC r  C Z - -  1
{
	m_cycles = 1;
	const int reg = opbyte0 & 7;
	uint8_t temp = get_reg8(reg);
	temp--;
	set_reg8(reg, temp);

	if (temp == 0xff) set_JF();
	else clear_JF();

	if (temp == 0x00) set_ZF();
	else clear_ZF();
}

void Tlcs870::do_JRS_T_a(uint8_t opbyte0) // JRS T, a  1 - - -  4 (2 if not taken)
{
	m_cycles = 2;
	int val = opbyte0 & 0x1f;
	if (val & 0x10) val -= 0x20;

	if (check_jump_condition(COND_T))
	{
		m_cycles += 2;
		m_addr = (m_tmppc + 2 + val) & 0xffff;
	}
	set_JF(); // always gets set?
}

void Tlcs870::do_JRS_F_a(uint8_t opbyte0) // JRS F, a  1 - - -  4 (2 if not taken)
{
	m_cycles = 2;
	int val = opbyte0 & 0x1f;
	if (val & 0x10) val -= 0x20;

	if (check_jump_condition(COND_F))
	{
		m_cycles += 2;
		m_addr = (m_tmppc + 2 + val) & 0xffff;
	}
	set_JF();
}

void Tlcs870::do_CALLV_n(uint8_t opbyte0) // CALLV n  - - - -  7
{
	m_cycles = 7;
	const uint16_t addr = 0xffc0 + ((opbyte0 & 0xf) * 2);
	WM16(m_sp - 1, uint16_t(m_addr));
	m_sp -= 2;
	m_addr = addr;
}

void Tlcs870::do_JR_cc_a(uint8_t opbyte0) // JR cc, a  1 - - -  4 (2 if not taken)
{
	m_cycles = 2;
	const int param1 = opbyte0 & 0x7;
	int val = READ8();
	if (val & 0x80) val -= 0x100;

	if (check_jump_condition(param1))
	{
		m_cycles += 2;
		m_addr = (m_tmppc + 2 + val) & 0xffff;
	}
	set_JF();
}

void Tlcs870::do_LD_CF_inxbit(uint8_t opbyte0) // LD CF, (x).b  ~C - * -  4   (aka TEST (x).b)
{
	m_cycles = 4;
	const uint16_t srcaddr = READ8();
	const uint8_t val = RM8(srcaddr);
	const uint8_t bit = val & (1 << (opbyte0 & 0x7));
	bit ? set_CF() : clear_CF();
	bit ? clear_JF() : set_JF();
}

void Tlcs870::do_LD_SP_mn(uint8_t) // LD SP ,mn  1 - - -  3
{
	m_cycles = 3;
	m_sp = READ16();
	set_JF();
}

void Tlcs870::do_JR_a(uint8_t) // JR a  1 - - -  4
{
	m_cycles = 4;
	int val = READ8();
	if (val & 0x80) val -= 0x100;
	m_addr = (m_tmppc + 2 + val) & 0xffff;
	set_JF();
}

void Tlcs870::do_CALL_mn(uint8_t) // CALL mn  - - - -  6
{
	m_cycles = 6;
	const uint16_t addr = READ16();
	WM16(m_sp - 1, uint16_t(m_addr));
	m_sp -= 2;
	m_addr = addr;
}

void Tlcs870::do_CALLP_n(uint8_t) // CALLP n  - - - -  6
{
	m_cycles = 6;
	const uint16_t addr = READ8() + 0xff00;
	WM16(m_sp - 1, uint16_t(m_addr));
	m_sp -= 2;
	m_addr = addr;
}

void Tlcs870::do_JP_mn(uint8_t) // JP mn  1 - - -  4
{
	m_cycles = 4;
	m_addr = READ16();
	set_JF();
}

void Tlcs870::do_ff_opcode(uint8_t) // SWI  - - - -  9 (1 if already in NMI)
{
	m_cycles = 9; // TODO (MAME): 1 if in NMI this acts as a NOP
	m_stats.swi_executed++;
	logf("%04x: SWI (INTSW latched; MAME never services it)", m_tmppc);
	m_IL |= 1 << (15 - TLCS870_IRQ_INTSW); // set interrupt latch
}

void Tlcs870::do_ALUOP_A_n(uint8_t opbyte0) // (ALU) A, n  2
{
	m_cycles = 2;
	const int aluop = (opbyte0 & 0x7);
	const uint8_t val = READ8();
	const uint8_t result = do_alu_8bit(aluop, get_reg8(REG_A), val);
	if (aluop != 0x07) // CMP doesn't write back
		set_reg8(REG_A, result);
}

void Tlcs870::do_ALUOP_A_inx(uint8_t opbyte0) // (ALU) A, (x)  4
{
	m_cycles = 4;
	const int aluop = (opbyte0 & 0x7);
	const uint16_t addr = READ8();
	const uint8_t val = RM8(addr);
	const uint8_t result = do_alu_8bit(aluop, get_reg8(REG_A), val);
	if (aluop != 0x07)
		set_reg8(REG_A, result);
}

void Tlcs870::do_LDW_inx_mn(uint8_t) // LDW (x), mn  1 - - -  6
{
	m_cycles = 6;
	const uint16_t dstaddr = READ8();
	const uint16_t val = READ16();
	WM16(dstaddr, val);
	set_JF();
}

void Tlcs870::do_LDW_inHL_mn(uint8_t) // LDW (HL), mn  1 - - -  5
{
	m_cycles = 5;
	const uint16_t dstaddr = get_reg16(REG_HL);
	const uint16_t val = READ16();
	WM16(dstaddr, val);
	set_JF();
}

/*************************************************************************************************************
    General helpers (MAME tlcs870_ops_helper.cpp)
*************************************************************************************************************/

void Tlcs870::handle_div(int reg)
{
	const uint16_t temp16 = get_reg16(reg);
	const uint8_t temp8 = get_reg8(REG_C);

	if (!temp8)
	{
		set_CF(); // divide by zero; does the ZF change too?
	}
	else
	{
		const uint16_t tempres16 = temp16 / temp8;
		const uint8_t tempres8 = temp16 % temp8;
		const uint16_t tempfull = (tempres8 << 8) | (tempres16 & 0x00ff);

		set_reg16(reg, tempfull);

		if (tempres16 & 0xff00) set_CF(); // result is also 'undefined' in this case
		else clear_CF();

		if (!tempres8)
		{
			set_ZF();
			set_JF();
		}
		else
		{
			clear_ZF();
			clear_JF();
		}
	}
}

void Tlcs870::handle_mul(int reg)
{
	const uint16_t temp16 = get_reg16(reg);
	const uint16_t tempres = (temp16 & 0xff) * ((temp16 & 0xff00) >> 8);
	// MAME quirk: writes the operands back instead of the product
	set_reg16(reg, fix_mame_bugs ? tempres : temp16);

	if (!(tempres & 0xff00))
	{
		set_ZF();
		set_JF();
	}
	else
	{
		clear_ZF();
		clear_JF();
	}
}

void Tlcs870::handle_swap(int reg)
{
	uint8_t temp = get_reg8(reg);
	temp = ((temp & 0x0f) << 4) | ((temp & 0xf0) >> 4);
	set_reg8(reg, temp);
	set_JF();
}

uint16_t Tlcs870::get_addr(uint16_t opbyte0, uint16_t val)
{
	uint16_t addr = 0x0000;

	switch (opbyte0)
	{
	case ADDR_IN_IMM_X: addr = val; break;
	case ADDR_IN_PC_PLUS_REG_A: addr = m_tmppc + 2 + get_reg8(REG_A); break;
	case ADDR_IN_DE: addr = get_reg16(REG_DE); break;
	case ADDR_IN_HL: addr = get_reg16(REG_HL); break;
	case ADDR_IN_HL_PLUS_IMM_D: addr = get_reg16(REG_HL) + val; break; // note: d is unsigned in MAME
	case ADDR_IN_HL_PLUS_REG_C: addr = get_reg16(REG_HL) + get_reg8(REG_C); break;
	case ADDR_IN_HLINC:
	{
		uint16_t tmpHL = get_reg16(REG_HL);
		addr = tmpHL;
		tmpHL++;
		set_reg16(REG_HL, tmpHL);
		break;
	}
	case ADDR_IN_DECHL:
	{
		uint16_t tmpHL = get_reg16(REG_HL);
		tmpHL--;
		set_reg16(REG_HL, tmpHL);
		addr = tmpHL;
		break;
	}
	}
	return addr;
}

bool Tlcs870::check_jump_condition(int param1)
{
	switch (param1)
	{
	case COND_EQ_Z: return is_ZF() == 1;
	case COND_NE_NZ: return is_ZF() == 0;
	case COND_LT_CS: return is_CF() == 1;
	case COND_GE_CC: return is_CF() == 0;
	case COND_LE: return (is_CF() || is_ZF()) == 1;
	case COND_GT: return (is_CF() || is_ZF()) == 0;
	case COND_T: return is_JF() == 1;
	case COND_F: return is_JF() == 0;
	}
	return true;
}

// MAME: "// JF is copied from CF" followed by  is_ZF() ? set_CF() : clear_CF();
void Tlcs870::copy_jf_from_cf_after_arith()
{
	if (fix_mame_bugs)
		is_CF() ? set_JF() : clear_JF();
	else
		is_ZF() ? set_CF() : clear_CF(); // MAME quirk
}

/*
    All 16-bit ALU ops that would set the 'H' flag list the behavior as undefined.  (see MAME notes)
*/

uint8_t Tlcs870::do_add_8bit(uint16_t param1, uint16_t param2)
{
	const uint16_t result = param1 + param2;

	if (result & 0x100) set_CF();
	else clear_CF();

	if ((result & 0xff) == 0x00) set_ZF();
	else clear_ZF();

	const uint8_t temp = (param1 & 0xf) + (param2 & 0xf);
	if (temp & 0x10) set_HF();
	else clear_HF();

	copy_jf_from_cf_after_arith();
	return uint8_t(result);
}

uint16_t Tlcs870::do_add_16bit(uint32_t param1, uint32_t param2)
{
	const uint32_t result = param1 + param2;

	if (result & 0x10000) set_CF();
	else clear_CF();

	if ((result & 0xffff) == 0x00) set_ZF();
	else clear_ZF();

	// unknown, manual says undefined
	const uint16_t temp = (param1 & 0xff) + (param2 & 0xff);
	if (temp & 0x100) set_HF();
	else clear_HF();

	copy_jf_from_cf_after_arith();
	return uint16_t(result);
}

uint8_t Tlcs870::do_sub_8bit(uint16_t param1, uint16_t param2)
{
	const uint16_t result = param1 - param2;

	if (param1 < param2) set_CF();
	else clear_CF();

	if ((param1 & 0xf) < (param2 & 0xf)) set_HF();
	else clear_HF();

	if ((result & 0xff) == 0x00) set_ZF();
	else clear_ZF();

	copy_jf_from_cf_after_arith();
	return uint8_t(result);
}

uint16_t Tlcs870::do_sub_16bit(uint32_t param1, uint32_t param2)
{
	const uint32_t result = param1 - param2;

	if (param1 < param2) set_CF();
	else clear_CF();

	// unknown, manual says undefined
	if ((param1 & 0xff) < (param2 & 0xff)) set_HF();
	else clear_HF();

	if ((result & 0xffff) == 0x0000) set_ZF();
	else clear_ZF();

	copy_jf_from_cf_after_arith();
	return uint16_t(result);
}

void Tlcs870::do_cmp_8bit(uint16_t param1, uint16_t param2)
{
	if (param1 < param2) set_CF();
	else clear_CF();

	if ((param1 & 0xf) < (param2 & 0xf)) set_HF();
	else clear_HF();

	if (param1 == param2) set_ZF();
	else clear_ZF();

	is_ZF() ? set_JF() : clear_JF(); // JF is copied from ZF
}

void Tlcs870::do_cmp_16bit(uint32_t param1, uint32_t param2)
{
	if (param1 < param2) set_CF();
	else clear_CF();

	if ((param1 & 0xff) < (param2 & 0xff)) set_HF();
	else clear_HF();

	if (param1 == param2) set_ZF();
	else clear_ZF();

	is_ZF() ? set_JF() : clear_JF();
}

uint16_t Tlcs870::do_and(uint16_t param1, uint16_t param2)
{
	const uint16_t result = param1 & param2;
	if (result == 0x00) set_ZF();
	else clear_ZF();
	is_ZF() ? set_JF() : clear_JF();
	return result;
}

uint16_t Tlcs870::do_xor(uint16_t param1, uint16_t param2)
{
	const uint16_t result = param1 ^ param2;
	if (result == 0x00) set_ZF();
	else clear_ZF();
	is_ZF() ? set_JF() : clear_JF();
	return result;
}

uint16_t Tlcs870::do_or(uint16_t param1, uint16_t param2)
{
	const uint16_t result = param1 | param2;
	if (result == 0x00) set_ZF();
	else clear_ZF();
	is_ZF() ? set_JF() : clear_JF();
	return result;
}

uint8_t Tlcs870::do_alu_8bit(int op, uint16_t param1, uint16_t param2)
{
	uint16_t result = 0x00;

	switch (op)
	{
	case 0x0: // ADDC
		param2 += is_CF();
		result = do_add_8bit(param1, param2);
		break;
	case 0x1: // ADD
		result = do_add_8bit(param1, param2);
		break;
	case 0x2: // SUBB
		param2 += is_CF();
		result = do_sub_8bit(param1, param2);
		break;
	case 0x3: // SUB
		result = do_sub_8bit(param1, param2);
		break;
	case 0x4: result = do_and(param1, param2); break;
	case 0x5: result = do_xor(param1, param2); break;
	case 0x6: result = do_or(param1, param2); break;
	case 0x7: do_cmp_8bit(param1, param2); break;
	}
	return uint8_t(result);
}

uint16_t Tlcs870::do_alu_16bit(int op, uint32_t param1, uint32_t param2)
{
	uint32_t result = 0x0000;

	switch (op)
	{
	case 0x0: // ADDC
		param2 += is_CF();
		result = do_add_16bit(param1, param2);
		break;
	case 0x1: // ADD
		result = do_add_16bit(param1, param2);
		break;
	case 0x2: // SUBB
		param2 += is_CF();
		result = do_sub_16bit(param1, param2);
		break;
	case 0x3: // SUB
		result = do_sub_16bit(param1, param2);
		break;
	case 0x4: result = do_and(uint16_t(param1), uint16_t(param2)); break;
	case 0x5: result = do_xor(uint16_t(param1), uint16_t(param2)); break;
	case 0x6: result = do_or(uint16_t(param1), uint16_t(param2)); break;
	case 0x7: do_cmp_16bit(param1, param2); break;
	}
	return uint16_t(result);
}

uint8_t Tlcs870::handle_SHLC(uint8_t val)
{
	if (val & 0x80) set_CF();
	else clear_CF();

	val = uint8_t(val << 1);

	if (is_CF()) set_JF();
	else clear_JF();

	if (val == 0) set_ZF();
	else clear_ZF();
	return val;
}

uint8_t Tlcs870::handle_SHRC(uint8_t val)
{
	if (val & 0x01) set_CF();
	else clear_CF();

	val = (val >> 1);

	if (is_CF()) set_JF();
	else clear_JF();

	if (val == 0) set_ZF();
	else clear_ZF();
	return val;
}

uint8_t Tlcs870::handle_DAS(uint8_t val)
{
	if (((val & 0x0f) > 9) || (is_HF() == 1))
	{
		val = val - 0x06;
		set_HF();
	}
	else
	{
		clear_HF();
	}

	if ((val > 0x9f) || (is_CF() == 1))
	{
		val = val - 0x60;
		set_CF();
	}
	else
	{
		clear_CF();
	}
	return val;
}

uint8_t Tlcs870::handle_DAA(uint8_t val)
{
	if (((val & 0x0f) > 9) || (is_HF() == 1))
	{
		val = val + 0x06;
		set_HF();
	}
	else
	{
		clear_HF();
	}

	if ((val > 0x9f) || (is_CF() == 1))
	{
		val = val + 0x60;
		set_CF();
	}
	else
	{
		clear_CF();
	}
	return val;
}

uint8_t Tlcs870::handle_ROLC(uint8_t val)
{
	const int tempcarry = is_CF();

	if (val & 0x80) set_CF();
	else clear_CF();

	val = uint8_t((val << 1) | tempcarry);

	if (is_CF()) set_JF();
	else clear_JF();

	if (val == 0) set_ZF();
	else clear_ZF();
	return val;
}

uint8_t Tlcs870::handle_RORC(uint8_t val)
{
	const int tempcarry = (is_CF()) << 7;

	if (val & 0x01) set_CF();
	else clear_CF();

	val = uint8_t((val >> 1) | tempcarry);

	if (is_CF()) set_JF();
	else clear_JF();

	if (val == 0) set_ZF();
	else clear_ZF();
	return val;
}
