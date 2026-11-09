// license:BSD-3-Clause
// copyright-holders:David Haywood
//
// Toshiba TLCS-870 series MCU core (TMP87PH40AN variant).
//
// Standalone port of MAME's src/devices/cpu/tlcs870 (tlcs870.cpp, tlcs870_ops*.cpp),
// originally written by David Haywood, with no MAME dependencies.  Behaviour (including
// the known inaccuracies of the MAME core) is kept as close to MAME as possible; see the
// "MAME quirks" notes in tlcs870.cpp.  Everything is deterministic and advances only
// through run().
//
// Copyright (c) David Haywood and the MAME contributors.  BSD-3-Clause licence: see
// https://github.com/mamedev/mame/blob/master/COPYING ; redistribution of source and
// binary forms must retain this copyright notice, the list of conditions and the disclaimer.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

class Tlcs870
{
public:
	// External input lines (same numbering as MAME's INPUT_LINE_IRQ0..IRQ5).
	enum InputLine
	{
		INPUT_INT0 = 0,
		INPUT_INT1 = 1,
		INPUT_INT2 = 2,
		INPUT_INT3 = 3,
		INPUT_INT4 = 4,
		INPUT_INT5 = 5,
	};

	// Diagnostic counters for things MAME logs / leaves unimplemented.
	struct Stats
	{
		uint64_t illegal_opcode = 0;     // unknown first opcode byte
		uint64_t illegal_src_opcode = 0; // E0-E7 prefix with unknown 2nd byte
		uint64_t illegal_dst_opcode = 0; // F0-F7 prefix with unknown 2nd byte (or F1/F5 prefix)
		uint64_t illegal_reg_opcode = 0; // E8-EF prefix with unknown 2nd byte (or RETN with E9-EF)
		uint64_t unmapped_read = 0;      // read from a reserved / unmapped address (returns 0)
		uint64_t unmapped_write = 0;     // write to a reserved / unmapped / read-only address (ignored)
		uint64_t rom_write = 0;          // write into 0x8000-0xffff (ignored)
		uint64_t swi_executed = 0;       // SWI (0xff): latches INTSW, which MAME never services
		uint64_t zero_cycle_instr = 0;   // MAME would fatalerror(); we charge 1 cycle instead
		uint64_t rom_lo_access = 0;      // reads of 0x8000-0xbfff (unmapped in MAME, 0 in the dump)
		uint64_t interrupts_taken = 0;
		uint64_t wdt_clear = 0;          // WDTCR2 <- 0x4e
		uint64_t wdt_disable = 0;        // WDTCR2 <- 0xb1
		uint64_t idle_requests = 0;      // SYSCR2 IDLE bit / SYSCR1 STOP bit set (not emulated, as MAME)
		uint16_t last_unmapped_addr = 0;
		uint16_t last_illegal_pc = 0;
		uint64_t sfr_unmapped_read[0x40] = {};   // per-address count for 0x00-0x3f (reserved / write-only)
		uint64_t sfr_unmapped_write[0x40] = {};  // per-address count for 0x00-0x3f (reserved / read-only)
	};

	Tlcs870();

	// Port callbacks.  Unset port_in returns 0xff (MAME devcb default), unset port_out is a no-op.
	// Note: like MAME, read-modify-write bit ops (SET/CLR/CPL/LD (x).b,CF/XCH/ALU (src)) read the
	// output latch instead of calling port_in.
	std::function<uint8_t()> port_in[8];
	std::function<void(uint8_t)> port_out[8];
	std::function<uint8_t()> analog_in[8];    // AN0..AN7 for the ADC; unset = 0xff
	std::function<void(int)> serial_out[2];   // SIO1 / SIO2 serial data out (bit per call)
	std::function<void(const char*)> log;     // optional: receives a line per unusual event

	// Set true to fix the obvious MAME core bugs (see tlcs870.cpp).  Default false = MAME-faithful.
	bool fix_mame_bugs = false;

	// Copies up to 32 KB of internal ROM to 0x8000-0xffff (a 32 KB file covers it exactly;
	// a smaller image is placed so that it ends at 0xffff).
	void load_rom(const uint8_t* data, size_t size);
	void reset();

	// Runs whole instructions until at least `cycles` clock cycles have elapsed.
	// Returns the number actually executed (>= cycles, overshoot <= one instruction).
	int run(int cycles);

	// Level of an external interrupt pin.  Like MAME, a rising edge latches the interrupt
	// (IL bit); deasserting just re-arms the edge detector.
	void set_input_line(int line, bool asserted);

	// Debug / introspection
	uint16_t pc() const { return uint16_t(m_pc); }
	uint16_t prev_pc() const { return uint16_t(m_prvpc); }
	uint16_t sp() const { return uint16_t(m_sp); }
	uint8_t psw() const { return uint8_t((m_F & 0xf0) | (m_RBS & 0x0f)); }
	uint16_t il() const { return m_IL; }
	uint16_t eir() const { return m_EIR; }
	uint8_t reg8(int r) const { return m_intram[((m_RBS & 0xf) * 8) + (r & 7)]; } // A,W,C,B,E,D,L,H
	uint64_t total_cycles() const { return m_total_cycles; }
	uint8_t peek(uint16_t addr) const;               // side-effect-free memory view (ports = latch)
	uint8_t port_latch(int port) const { return m_port_out_latch[port & 7]; }
	const Stats& stats() const { return m_stats; }
	void clear_stats() { m_stats = Stats(); }

private:
	enum
	{
		TLCS870_IRQ_INT5,    // FFE0 INT5    (External Interrupt 5) - priority 15 (lowest)
		TLCS870_IRQ_INTTC2,  // FFE2 INTTC2  (16-bit TC2 Interrupt)
		TLCS870_IRQ_INTSIO2, // FFE4 INTSIO2 (Serial Interface 2 Interrupt)
		TLCS870_IRQ_INT4,    // FFE6 INT4    (External Interrupt 4)
		TLCS870_IRQ_INT3,    // FFE8 INT3    (External Interrupt 3)
		TLCS870_IRQ_INTTC4,  // FFEA INTTC4  (8-bit TC4 Interrupt)
		TLCS870_IRQ_INTSIO1, // FFEC INTSIO1 (Serial Interface 1 Interrupt)
		TLCS870_IRQ_INTTC3,  // FFEE INTTC3  (8-bit TC3 Interrupt)
		TLCS870_IRQ_INT2,    // FFF0 INT2    (External Interrupt 2)
		TLCS870_IRQ_INTTBT,  // FFF2 INTTBT  (Time Base Timer Interrupt)
		TLCS870_IRQ_INT1,    // FFF4 INT1    (External Interrupt 1)
		TLCS870_IRQ_INTTC1,  // FFF6 INTTC1  (16-bit TC1 Interrupt)
		TLCS870_IRQ_INT0,    // FFF8 INT0    (External Interrupt 0)
		TLCS870_IRQ_INTWDT,  // FFFA INTWDT  (Watchdog Timer Interrupt)
		TLCS870_IRQ_INTSW,   // FFFC INTSW   (Software Interrupt)
		TLCS870_IRQ_RESET,   // FFFE RESET   (Reset Interrupt) - priority 0 (highest)
	};

	enum { REG_A, REG_W, REG_C, REG_B, REG_E, REG_D, REG_L, REG_H };
	enum { REG_WA, REG_BC, REG_DE, REG_HL };
	enum { COND_EQ_Z, COND_NE_NZ, COND_LT_CS, COND_GE_CC, COND_LE, COND_GT, COND_T, COND_F };
	enum
	{
		ADDR_IN_IMM_X, ADDR_IN_PC_PLUS_REG_A, ADDR_IN_DE, ADDR_IN_HL,
		ADDR_IN_HL_PLUS_IMM_D, ADDR_IN_HL_PLUS_REG_C, ADDR_IN_HLINC, ADDR_IN_DECHL
	};

	static constexpr int FLAG_J = 0x80;
	static constexpr int FLAG_Z = 0x40;
	static constexpr int FLAG_C = 0x20;
	static constexpr int FLAG_H = 0x10;

	// timers (absolute cycle deadlines; NEVER = disabled)
	static constexpr uint64_t NEVER = ~uint64_t(0);
	enum { TMR_SIO0, TMR_SIO1, TMR_TC1, TMR_TC2, TMR_TC3, TMR_TC4, TMR_COUNT };
	uint64_t m_timer_deadline[TMR_COUNT];
	uint64_t m_now = 0;  // cycle count at the start of the current instruction
	void timer_adjust(int t, uint64_t delay) { m_timer_deadline[t] = m_now + delay; }
	void timer_cancel(int t) { m_timer_deadline[t] = NEVER; }
	void run_timers(uint64_t upto);
	void timer_fire(int t);

	// memory
	uint8_t m_rom[0x8000];   // 0x8000-0xffff
	uint8_t m_intram[0x200]; // 0x0040-0x023f (register banks + RAM)
	uint8_t m_dbr[0x80];     // 0x0f80-0x0fff (0ff0-0ff7 SIO1 buffer, 0ff8-0fff SIO2 buffer)

	uint8_t RM8(uint32_t a);
	void WM8(uint32_t a, uint8_t v);
	uint16_t RM16(uint32_t a) { return RM8(a) | (RM8((a + 1) & 0xffff) << 8); }
	void WM16(uint32_t a, uint16_t v) { WM8(a, uint8_t(v)); WM8((a + 1) & 0xffff, uint8_t(v >> 8)); }
	uint8_t READ8() { const uint8_t b0 = RM8(m_addr++); m_addr &= 0xffff; return b0; }
	uint16_t READ16() { const uint8_t b0 = READ8(); return b0 | (READ8() << 8); }
	uint8_t sfr_read(uint16_t a);
	void sfr_write(uint16_t a, uint8_t v);
	void note_unmapped(bool write, uint32_t a);
	void logf(const char* fmt, ...);

	// state
	uint32_t m_prvpc = 0, m_pc = 0, m_sp = 0;
	uint64_t m_total_cycles = 0;
	Stats m_stats;

	uint8_t m_port_out_latch[8];
	int m_read_input_port = 1;
	uint8_t m_port0_cr = 0, m_port1_cr = 0, m_port6_cr = 0, m_port7_cr = 0;

	uint8_t m_cycles = 0;
	uint16_t m_tmppc = 0;
	uint32_t m_addr = 0;
	uint16_t m_F = 0;

	uint8_t m_RBS = 0;
	uint16_t m_IL = 0;
	uint16_t m_EIR = 0;
	uint8_t m_EINTCR = 0;
	uint8_t m_ADCCR = 0;
	uint8_t m_ADCDR = 0;
	uint8_t m_SYSCR1 = 0;
	uint8_t m_SYSCR2 = 0;
	uint8_t m_TBTCR = 0;
	uint16_t m_TREG1A = 0;
	uint16_t m_TREG1B = 0;
	uint8_t m_TC1CR = 0;
	uint16_t m_TREG2 = 0;
	uint8_t m_TC2CR = 0;
	uint8_t m_TREG3A = 0;
	uint8_t m_TREG3B = 0;
	uint8_t m_TC3CR = 0;
	uint8_t m_TREG4 = 0;
	uint8_t m_TC4CR = 0;
	uint8_t m_SIOCR1[2] = {};
	uint8_t m_SIOCR2[2] = {};
	uint8_t m_WDTCR1 = 0;
	uint16_t m_irqstate = 0;

	uint8_t m_transfer_numbytes[2] = {};
	uint8_t m_transfer_pos[2] = {};
	uint8_t m_transfer_shiftreg[2] = {};
	uint8_t m_transfer_shiftpos[2] = {};
	uint8_t m_transfer_mode[2] = {};
	int m_transmit_bits[2] = {};
	int m_receive_bits[2] = {};

	// SFR handlers
	uint8_t port_r(int n);
	void port_w(int n, uint8_t data);
	void tc1cr_w(uint8_t data);
	void tc2cr_w(uint8_t data);
	void tc2_reload();
	void tc2_cancel();
	void sio1cr1_w(uint8_t data);
	void sio1cr2_w(uint8_t data);
	void sio0_transmit_cb();
	void wdtcr2_w(uint8_t data);
	void syscr1_w(uint8_t data);
	void syscr2_w(uint8_t data);
	void adccr_w(uint8_t data);
	void il_l_w(uint8_t data);
	void il_h_w(uint8_t data);

	// interrupts
	void set_irq_line(int irqline, int state);
	void check_interrupts();
	void take_interrupt(int priority);

	// helpers
	uint16_t get_addr(uint16_t opbyte0, uint16_t val);
	uint8_t get_reg8(int reg) { return m_intram[((m_RBS & 0xf) * 8) + (reg & 0x7)]; }
	void set_reg8(int reg, uint8_t val) { m_intram[((m_RBS & 0xf) * 8) + (reg & 0x7)] = val; }
	uint16_t get_reg16(int reg) { return uint16_t((get_reg8(((reg & 3) * 2) + 1) << 8) | get_reg8((reg & 3) * 2)); }
	void set_reg16(int reg, uint16_t val) { set_reg8(((reg & 3) * 2) + 1, uint8_t(val >> 8)); set_reg8((reg & 3) * 2, uint8_t(val)); }

	void clear_JF() { m_F &= ~FLAG_J; }
	void clear_ZF() { m_F &= ~FLAG_Z; }
	void clear_CF() { m_F &= ~FLAG_C; }
	void clear_HF() { m_F &= ~FLAG_H; }
	void set_JF() { m_F |= FLAG_J; }
	void set_ZF() { m_F |= FLAG_Z; }
	void set_CF() { m_F |= FLAG_C; }
	void set_HF() { m_F |= FLAG_H; }
	int is_JF() const { return (m_F & FLAG_J) ? 1 : 0; }
	int is_ZF() const { return (m_F & FLAG_Z) ? 1 : 0; }
	int is_CF() const { return (m_F & FLAG_C) ? 1 : 0; }
	int is_HF() const { return (m_F & FLAG_H) ? 1 : 0; }
	static int get_base_srcdst_cycles(int i) { static const int t[8] = { 1, 2, 0, 0, 2, 2, 1, 1 }; return t[i]; }

	uint8_t get_PSW() const { return uint8_t((m_F & 0xf0) | (m_RBS & 0x0f)); }
	void set_PSW(uint8_t data) { m_F = data & 0xf0; m_RBS = data & 0x0f; }

	// tlcs870_ops.cpp
	void decode();
	void do_illegal(uint8_t opbyte0);
	void do_NOP(uint8_t opbyte0);
	void do_SWAP_A(uint8_t opbyte0);
	void do_MUL_W_A(uint8_t opbyte0);
	void do_DIV_WA_C(uint8_t opbyte0);
	void do_RETI(uint8_t opbyte0);
	void do_RET(uint8_t opbyte0);
	void do_POP_PSW(uint8_t opbyte0);
	void do_PUSH_PSW(uint8_t opbyte0);
	void do_DAA_A(uint8_t opbyte0);
	void do_DAS_A(uint8_t opbyte0);
	void do_CLR_CF(uint8_t opbyte0);
	void do_SET_CF(uint8_t opbyte0);
	void do_CPL_CF(uint8_t opbyte0);
	void do_LD_RBS_n(uint8_t opbyte0);
	void do_INC_rr(uint8_t opbyte0);
	void do_LD_rr_mn(uint8_t opbyte0);
	void do_DEC_rr(uint8_t opbyte0);
	void do_SHLC_A(uint8_t opbyte0);
	void do_SHRC_A(uint8_t opbyte0);
	void do_ROLC_A(uint8_t opbyte0);
	void do_RORC_A(uint8_t opbyte0);
	void do_INC_inx(uint8_t opbyte0);
	void do_INC_inHL(uint8_t opbyte0);
	void do_LD_A_inx(uint8_t opbyte0);
	void do_LD_A_inHL(uint8_t opbyte0);
	void do_LDW_inx_mn(uint8_t opbyte0);
	void do_LDW_inHL_mn(uint8_t opbyte0);
	void do_LD_inx_iny(uint8_t opbyte0);
	void do_DEC_inx(uint8_t opbyte0);
	void do_DEC_inHL(uint8_t opbyte0);
	void do_LD_inx_A(uint8_t opbyte0);
	void do_LD_inHL_A(uint8_t opbyte0);
	void do_LD_inx_n(uint8_t opbyte0);
	void do_LD_inHL_n(uint8_t opbyte0);
	void do_CLR_inx(uint8_t opbyte0);
	void do_CLR_inHL(uint8_t opbyte0);
	void do_LD_r_n(uint8_t opbyte0);
	void do_SET_inxbit(uint8_t opbyte0);
	void do_CLR_inxbit(uint8_t opbyte0);
	void do_LD_A_r(uint8_t opbyte0);
	void do_LD_r_A(uint8_t opbyte0);
	void do_INC_r(uint8_t opbyte0);
	void do_DEC_r(uint8_t opbyte0);
	void do_ALUOP_A_n(uint8_t opbyte0);
	void do_ALUOP_A_inx(uint8_t opbyte0);
	void do_JRS_T_a(uint8_t opbyte0);
	void do_JRS_F_a(uint8_t opbyte0);
	void do_CALLV_n(uint8_t opbyte0);
	void do_JR_cc_a(uint8_t opbyte0);
	void do_LD_CF_inxbit(uint8_t opbyte0);
	void do_LD_SP_mn(uint8_t opbyte0);
	void do_JR_a(uint8_t opbyte0);
	void do_CALL_mn(uint8_t opbyte0);
	void do_CALLP_n(uint8_t opbyte0);
	void do_JP_mn(uint8_t opbyte0);
	void do_ff_opcode(uint8_t opbyte0);

	// tlcs870_ops_src.cpp
	void do_srcprefixtype_opcode(uint8_t opbyte0);
	void do_e0_to_e7_oprand_illegal(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_ROLD_A_insrc(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_RORD_A_insrc(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_LD_rr_insrc(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_INC_insrc(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_LD_inx_insrc(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_LD_inHL_insrc(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_DEC_insrc(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_MCMP_insrc_n(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_SET_insrcbit(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_CLR_insrcbit(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_LD_r_insrc(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_ALUOP_insrc_inHL(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_ALUOP_insrc_n(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_ALUOP_A_insrc(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_XCH_r_insrc(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_CPL_insrcbit(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_LD_insrcbit_CF(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_XOR_CF_insrcbit(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_LD_CF_insrcbit(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_CALL_insrc(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);
	void do_JP_insrc(uint8_t opbyte0, uint8_t opbyte1, uint16_t srcaddr);

	// tlcs870_ops_dst.cpp
	void do_dstprefixtype_opcode(uint8_t opbyte0);
	void do_f0_to_f7_oprand_illegal_opcode(uint8_t opbyte0, uint8_t opbyte1, uint16_t dstaddr);
	void do_LD_indst_rr(uint8_t opbyte0, uint8_t opbyte1, uint16_t dstaddr);
	void do_LD_indst_n(uint8_t opbyte0, uint8_t opbyte1, uint16_t dstaddr);
	void do_LD_indst_r(uint8_t opbyte0, uint8_t opbyte1, uint16_t dstaddr);

	// tlcs870_ops_reg.cpp
	void do_regprefixtype_opcode(uint8_t opbyte0);
	void do_regprefixtype_oprand_illegal(uint8_t opbyte0, uint8_t opbyte1);
	void do_SWAP_g(uint8_t opbyte0, uint8_t opbyte1);
	void do_MUL_gg(uint8_t opbyte0, uint8_t opbyte1);
	void do_DIV_gg_C(uint8_t opbyte0, uint8_t opbyte1);
	void do_RETN(uint8_t opbyte0, uint8_t opbyte1);
	void do_POP_gg(uint8_t opbyte0, uint8_t opbyte1);
	void do_PUSH_gg(uint8_t opbyte0, uint8_t opbyte1);
	void do_DAA_g(uint8_t opbyte0, uint8_t opbyte1);
	void do_DAS_g(uint8_t opbyte0, uint8_t opbyte1);
	void do_XCH_rr_gg(uint8_t opbyte0, uint8_t opbyte1);
	void do_LD_rr_gg(uint8_t opbyte0, uint8_t opbyte1);
	void do_SHLC_g(uint8_t opbyte0, uint8_t opbyte1);
	void do_SHRC_g(uint8_t opbyte0, uint8_t opbyte1);
	void do_ROLC_g(uint8_t opbyte0, uint8_t opbyte1);
	void do_RORC_g(uint8_t opbyte0, uint8_t opbyte1);
	void do_ALUOP_WA_gg(uint8_t opbyte0, uint8_t opbyte1);
	void do_ALUOP_gg_mn(uint8_t opbyte0, uint8_t opbyte1);
	void do_SET_gbit(uint8_t opbyte0, uint8_t opbyte1);
	void do_CLR_gbit(uint8_t opbyte0, uint8_t opbyte1);
	void do_LD_r_g(uint8_t opbyte0, uint8_t opbyte1);
	void do_ALUOP_A_g(uint8_t opbyte0, uint8_t opbyte1);
	void do_ALUOP_g_A(uint8_t opbyte0, uint8_t opbyte1);
	void do_ALUOP_g_n(uint8_t opbyte0, uint8_t opbyte1);
	void do_SET_inppbit(uint8_t opbyte0, uint8_t opbyte1);
	void do_CLR_inppbit(uint8_t opbyte0, uint8_t opbyte1);
	void do_CPL_inpp_indirectbit(uint8_t opbyte0, uint8_t opbyte1);
	void do_LD_inpp_indirectbit_CF(uint8_t opbyte0, uint8_t opbyte1);
	void do_LD_CF_inpp_indirectbit(uint8_t opbyte0, uint8_t opbyte1);
	void do_XCH_r_g(uint8_t opbyte0, uint8_t opbyte1);
	void do_CPL_gbit(uint8_t opbyte0, uint8_t opbyte1);
	void do_LD_gbit_CF(uint8_t opbyte0, uint8_t opbyte1);
	void do_XOR_CF_gbit(uint8_t opbyte0, uint8_t opbyte1);
	void do_LD_CF_gbit(uint8_t opbyte0, uint8_t opbyte1);
	void do_LD_SP_gg(uint8_t opbyte0, uint8_t opbyte1);
	void do_LD_gg_SP(uint8_t opbyte0, uint8_t opbyte1);
	void do_CALL_gg(uint8_t opbyte0, uint8_t opbyte1);
	void do_JP_gg(uint8_t opbyte0, uint8_t opbyte1);

	// ALU (tlcs870_ops_helper.cpp)
	uint16_t do_or(uint16_t param1, uint16_t param2);
	uint16_t do_xor(uint16_t param1, uint16_t param2);
	uint16_t do_and(uint16_t param1, uint16_t param2);
	uint8_t do_add_8bit(uint16_t param1, uint16_t param2);
	uint8_t do_sub_8bit(uint16_t param1, uint16_t param2);
	void do_cmp_8bit(uint16_t param1, uint16_t param2);
	uint8_t do_alu_8bit(int op, uint16_t param1, uint16_t param2);
	uint16_t do_add_16bit(uint32_t param1, uint32_t param2);
	uint16_t do_sub_16bit(uint32_t param1, uint32_t param2);
	void do_cmp_16bit(uint32_t param1, uint32_t param2);
	uint16_t do_alu_16bit(int op, uint32_t param1, uint32_t param2);
	void copy_jf_from_cf_after_arith();
	void handle_div(int reg);
	void handle_mul(int reg);
	void handle_swap(int reg);
	uint8_t handle_SHLC(uint8_t val);
	uint8_t handle_SHRC(uint8_t val);
	uint8_t handle_DAS(uint8_t val);
	uint8_t handle_DAA(uint8_t val);
	uint8_t handle_ROLC(uint8_t val);
	uint8_t handle_RORC(uint8_t val);
	bool check_jump_condition(int param1);
};
