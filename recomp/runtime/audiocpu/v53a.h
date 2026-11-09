// license:BSD-3-Clause
// copyright-holders:Bryan McPhail, Patrick Mackinlay
/***************************************************************************

    NEC V53A (uPD70236A) - standalone port of MAME's v53a_device

    = V33A CPU core (MAME src/devices/cpu/nec/nec.cpp & friends, Bryan McPhail)
    + V53 on-chip peripherals glue (MAME src/devices/cpu/nec/v5x.cpp,
      Patrick Mackinlay):
        TCU  - uPD71054 / i8254 timer         (v53a_pit8254.*, Wilbert Pol, Nathan Woods)
        ICU  - uPD71059 / i8259 int. ctrl.    (v53a_pic8259.*, Wilbert Pol)
        DMAU - uPD71071 mode DMA controller   (v53a_dmau.*,    Curt Coder)
        SCU  - uPD71051 / i8251 subset        (v53a_scu.*,     smf, Robbbert)

    No MAME dependencies. Deterministic: time only advances inside run(),
    measured in V53A input clocks (1 clock == 1 CPU cycle on the V53, MAME
    doesn't divide the clock for this part).

    Scheduling model (replaces MAME's device_scheduler):
      run() executes the CPU in slices that end at the next peripheral event
      (TCU counter timer, DMAU clock, ICU re-evaluation, SCU baud rate
      timer); peripheral events are then fired at their exact times. A
      register access that moves an event before the end of the current
      slice aborts the slice after the current instruction, as MAME's
      abort_timeslice() does. The DMAU state machine is clocked at clock/4
      (MAME: DERIVED_CLOCK(1, 4)) and does not steal CPU cycles (as in MAME).

    Usage:
      v53::V53A cpu;            // 32 MHz by default
      cpu.cb.mem_read8 = ...;   // fill in callbacks (see Callbacks)
      cpu.reset();              // power-on/reset (must be called once)
      cpu.run(n);               // run >= n clocks, returns clocks actually run

***************************************************************************/

#ifndef BURIKI_AUDIOCPU_V53A_H
#define BURIKI_AUDIOCPU_V53A_H

#pragma once

#include <bit>
#include <cstdint>
#include <functional>
#include <string>

#include "v53a_pit8254.h"
#include "v53a_pic8259.h"
#include "v53a_dmau.h"
#include "v53a_scu.h"

static_assert(std::endian::native == std::endian::little, "V53A port assumes a little-endian host");

namespace v53 {

class V53A
{
public:
	// set_input_line() line numbers (MAME: INPUT_LINE_IRQ0..7, INPUT_LINE_NMI, NEC_INPUT_LINE_POLL)
	enum
	{
		LINE_IRQ0 = 0, LINE_IRQ1, LINE_IRQ2, LINE_IRQ3, LINE_IRQ4, LINE_IRQ5, LINE_IRQ6, LINE_IRQ7,
		LINE_NMI = 32,
		LINE_POLL = 33
	};

	// SCU output pins reported through cb.scu_pin
	enum ScuPin { SCU_TXD, SCU_RTS, SCU_DTR, SCU_RXRDY, SCU_TXRDY, SCU_TXEMPTY, SCU_SYNDET };

	struct Callbacks
	{
		// ---- CPU program space (16-bit bus). Addresses are physical, after the V33
		// address translation (XA mode, BRKXA/RETXA): 20 bits when XA is off, up to
		// 24 bits when on (MAME's program space is 24 bits wide). If mem_read16 /
		// mem_write16 are not set, word accesses are split into two byte calls.
		// Word callbacks are only called with even addresses; odd word accesses are
		// always split into two byte accesses (as MAME's read_word_unaligned does).
		std::function<uint8_t(uint32_t addr)> mem_read8;
		std::function<void(uint32_t addr, uint8_t data)> mem_write8;
		std::function<uint16_t(uint32_t addr)> mem_read16;
		std::function<void(uint32_t addr, uint16_t data)> mem_write16;

		// ---- CPU I/O space (16-bit bus, 16-bit port addresses). Ports 0xff00-0xffff
		// are the V53 fixed internal registers and never reach these callbacks; the
		// relocatable TCU/ICU/DMAU/SCU block (OPHA/xULA/OPSEL) is also decoded
		// internally, its unused holes are forwarded here like in MAME.
		// io_read16/io_write16 only get even ports; if unset, split into byte calls.
		std::function<uint8_t(uint16_t port)> io_read8;
		std::function<void(uint16_t port, uint8_t data)> io_write8;
		std::function<uint16_t(uint16_t port)> io_read16;
		std::function<void(uint16_t port, uint16_t data)> io_write16;

		// ---- DMAU (MAME devcb semantics: an unset read returns 0, an unset write
		// does nothing; nothing is composed from other callbacks).
		// 8-bit transfers (mode bit 0 = 0): 'addr' is the 24-bit DMA address.
		// 16-bit transfers (mode bit 0 = 1): 'offset' is the DMA address >> 1, as in MAME.
		std::function<uint8_t(uint32_t addr)> dma_mem_read8;                  // in_memr_cb
		std::function<void(uint32_t addr, uint8_t data)> dma_mem_write8;      // out_memw_cb
		std::function<uint16_t(uint32_t offset)> dma_mem_read16;              // in_mem16r_cb
		std::function<void(uint32_t offset, uint16_t data)> dma_mem_write16;  // out_mem16w_cb
		std::function<uint8_t(int ch, uint32_t addr)> dma_io_read8;                   // in_ior_cb<ch>
		std::function<void(int ch, uint32_t addr, uint8_t data)> dma_io_write8;       // out_iow_cb<ch>
		std::function<uint16_t(int ch, uint32_t offset)> dma_io_read16;               // in_io16r_cb<ch>
		std::function<void(int ch, uint32_t offset, uint16_t data)> dma_io_write16;   // out_io16w_cb<ch>
		std::function<void(int state)> dma_hreq;            // out_hreq_cb (HNG64: call hack_w(1) from here)
		std::function<void(int state)> dma_eop;             // out_eop_cb
		std::function<void(int ch, int state)> dma_dack;    // out_dack_cb<ch>

		// ---- TCU outputs (tout_handler<0..2>). Not connected to the ICU internally
		// on the V53: wire tout 2 to set_input_line(LINE_IRQ2, state) yourself.
		std::function<void(int timer, int state)> tout;

		// ---- SCU
		std::function<void(int pin, int state)> scu_pin;    // ScuPin outputs
		std::function<void(int state)> scu_sint;            // sint_handler_cb (not wired to the ICU on V53)

		// ---- log of unimplemented / unusual events (MAME logerror). Optional.
		std::function<void(const char *msg)> log;
	};

	Callbacks cb;

	explicit V53A(uint32_t clock = 32000000);

	// device reset (CPU core + V53 internal registers + TCU/ICU/DMAU/SCU), as MAME's
	// device_t::reset(). Also the required power-on initialisation.
	void reset();

	// run for at least 'cycles' clocks; returns the number of clocks actually run
	// (instructions are atomic, so the overshoot is carried into the next call)
	int run(int cycles);

	// interrupt inputs: LINE_IRQ0..7 go to the ICU IR0..7, LINE_NMI, LINE_POLL
	void set_input_line(int line, bool asserted);
	// MAME INPUT_LINE_HALT: suspend the CPU (peripherals keep running)
	void set_halt_line(bool asserted);
	// MAME INPUT_LINE_RESET: assert suspends the CPU, release resets the whole V53A
	void set_reset_line(bool asserted);

	// DMA request inputs (dreq_w<ch>), ignored with a log in uPD71037 mode (SCTL bit 1)
	void dreq(int channel, bool state);
	// DMA hold acknowledge (MAME hack_w)
	void hack_w(int state);

	// TCU external clock input (TCKS selects it per timer) and its frequency
	void tclk_w(int state);
	void set_tclk(uint32_t hz);

	// SCU inputs
	void scu_rxd_w(int state) { m_scu.write_rxd(state); }
	void scu_cts_w(int state) { m_scu.write_cts(state); }
	void scu_dsr_w(int state) { m_scu.write_dsr(state); }

	// ---- debugging
	uint16_t cs() const { return m_sregs[1]; }
	uint16_t ip() const { return m_ip; }
	uint16_t prev_ip() const { return m_prev_ip; }
	uint32_t pc() const { return (uint32_t(m_sregs[1]) << 4) + m_ip; } // linear PS:IP (untranslated)
	uint16_t ds0() const { return m_sregs[3]; }
	uint16_t ds1() const { return m_sregs[0]; }
	uint16_t ss() const { return m_sregs[2]; }
	// AW, CW, DW, BW, SP, BP, IX, IY (= AX, CX, DX, BX, SP, BP, SI, DI)
	uint16_t reg(int n) const { return m_regs.w[n & 7]; }
	uint16_t flags() const;
	bool xa() const { return m_xa; }
	bool in_hlt() const { return m_halted != 0; }
	// current time in clocks since construction; exact also inside callbacks
	// (CPU-local time during an instruction, event time during peripheral events)
	uint64_t total_cycles() const { return now(); }
	uint64_t log_count() const { return m_log_count; }
	const std::string &last_log() const { return m_last_log; }
	uint8_t opsel() const { return m_OPSEL; }
	uint8_t opha() const { return m_OPHA; }
	uint8_t sctl() const { return m_SCTL; }
	Pit8254 &tcu() { return m_tcu; }
	Pic8259 &icu() { return m_icu; }
	Dmau &dmau() { return m_dmau; }

	// ---- internal interface used by the peripheral ports
	uint64_t now() const;
	void schedule_event(uint64_t when);
	void logerror(const char *fmt, ...);
	void tcu_out(int n, int state);
	void icu_request_eval();
	void icu_int_w(int state) { set_int_line(state); }
	void dma_resume();
	void dmau_hreq(int state) { if (cb.dma_hreq) cb.dma_hreq(state); }
	void dmau_eop(int state) { if (cb.dma_eop) cb.dma_eop(state); }
	void dmau_dack(int ch, int state) { if (cb.dma_dack) cb.dma_dack(ch, state); }
	uint8_t dmau_memr(uint32_t a) { return cb.dma_mem_read8 ? cb.dma_mem_read8(a) : 0; }
	void dmau_memw(uint32_t a, uint8_t d) { if (cb.dma_mem_write8) cb.dma_mem_write8(a, d); }
	uint16_t dmau_mem16r(uint32_t o) { return cb.dma_mem_read16 ? cb.dma_mem_read16(o) : 0; }
	void dmau_mem16w(uint32_t o, uint16_t d) { if (cb.dma_mem_write16) cb.dma_mem_write16(o, d); }
	uint8_t dmau_ior(int ch, uint32_t a) { return cb.dma_io_read8 ? cb.dma_io_read8(ch, a) : 0; }
	void dmau_iow(int ch, uint32_t a, uint8_t d) { if (cb.dma_io_write8) cb.dma_io_write8(ch, a, d); }
	uint16_t dmau_io16r(int ch, uint32_t o) { return cb.dma_io_read16 ? cb.dma_io_read16(ch, o) : 0; }
	void dmau_io16w(int ch, uint32_t o, uint16_t d) { if (cb.dma_io_write16) cb.dma_io_write16(ch, o, d); }
	void scu_pin(int pin, int state) { if (cb.scu_pin) cb.scu_pin(pin, state); }
	void scu_sint(int state) { if (cb.scu_sint) cb.scu_sint(state); }

private:
	// ======================= scheduler =======================
	static constexpr uint64_t NEVER = ~uint64_t(0);
	static constexpr uint64_t DMAU_CLOCK_DIVIDER = 4;

	uint64_t next_event_time() const;
	void process_events(uint64_t until);
	void abort_timeslice();

	uint32_t m_clock;
	uint64_t m_time = 0;          // global time (clocks) when not executing
	uint64_t m_cur = 0;           // "now" while firing events / outside run()
	bool m_in_cpu = false;
	uint64_t m_slice_start = 0;
	int m_slice_len = 0;

	bool m_halt_line = false;
	bool m_reset_line = false;

	bool m_icu_eval_pending = false;
	uint64_t m_icu_eval_time = 0;
	uint64_t m_dma_next = 0;
	uint64_t m_brc_start = 0;     // BRC timer: tick k at m_brc_start + ceil(k * clock / hz)
	uint64_t m_brc_k = 0;
	uint64_t m_brc_hz = 0;        // 0 = timer stopped

	uint64_t m_log_count = 0;
	std::string m_last_log;

	// ======================= V53 glue (v5x.cpp) =======================
	enum opsel_mask
	{
		OPSEL_DS = 0x01, // dmau enabled
		OPSEL_IS = 0x02, // icu enabled
		OPSEL_TS = 0x04, // tcu enabled
		OPSEL_SS = 0x08, // scu enabled
		OPSEL_MASK = OPSEL_DS | OPSEL_IS | OPSEL_TS | OPSEL_SS
	};

	uint32_t m_tclk = 0;
	uint8_t m_OPSEL = 0;
	uint8_t m_SULA = 0;
	uint8_t m_TULA = 0;
	uint8_t m_IULA = 0;
	uint8_t m_DULA = 0;
	uint8_t m_OPHA = 0;
	uint8_t m_TCKS = 0;
	uint8_t m_BRC = 0;
	bool m_brc_enable = false;
	uint8_t m_SCTL = 0;
	uint16_t m_v33_transtable[64] = {};

	Pit8254 m_tcu;
	Dmau m_dmau;
	Pic8259 m_icu;
	Scu m_scu;

	void interface_pre_reset();
	void tcu_clock_update();
	void brc_update();
	void brc_tick();
	void SCTL_w(uint8_t data);
	void TCKS_w(uint8_t data) { m_TCKS = data; tcu_clock_update(); }
	void BRC_w(uint8_t data) { m_BRC = data; brc_update(); }
	void OPSEL_w(uint8_t data) { m_OPSEL = data & 0x0f; }
	void tout1_w(int state);

	uint16_t OPHA() const { return (uint16_t(m_OPHA) << 8) & 0xff00; }
	bool check_OPHA(uint32_t a) const
	{
		return ((m_OPSEL & OPSEL_MASK) != 0) && (m_OPHA != 0xff) && ((a & 0xff00) == OPHA()); // 256 bytes boundary, ignore system io area
	}

	// internal (relocatable) peripheral block, byte granular
	uint8_t internal_io_read_byte(uint32_t a);
	void internal_io_write_byte(uint32_t a, uint8_t v);
	uint16_t internal_io_read_word(uint32_t a);
	void internal_io_write_word(uint32_t a, uint16_t v);
	int internal_decode(uint8_t a, uint32_t &offset) const;

	// external I/O space (incl. fixed internal registers at 0xff00-0xffff)
	uint8_t ext_io_read_byte(uint32_t port);
	void ext_io_write_byte(uint32_t port, uint8_t v);
	uint16_t ext_io_read_word(uint32_t port);
	void ext_io_write_word(uint32_t port, uint16_t v);
	uint8_t fixed_reg_read(uint32_t port);
	void fixed_reg_write(uint32_t port, uint8_t v);

	// MAME nec_common_device::io_* (base class versions)
	uint8_t nec_io_read_byte(uint32_t a) { return ext_io_read_byte(a & 0xffff); }
	void nec_io_write_byte(uint32_t a, uint8_t v) { ext_io_write_byte(a & 0xffff, v); }
	uint16_t nec_io_read_word(uint32_t a);
	void nec_io_write_word(uint32_t a, uint16_t v);

	// v53_device::io_* overrides (the ones the core uses)
	uint8_t io_read_byte(uint32_t a);
	uint16_t io_read_word(uint32_t a);
	void io_write_byte(uint32_t a, uint8_t v);
	void io_write_word(uint32_t a, uint16_t v);

	// program space
	uint8_t mem_rb(uint32_t a) { a &= 0xffffff; return cb.mem_read8 ? cb.mem_read8(a) : 0; }
	void mem_wb(uint32_t a, uint8_t d) { a &= 0xffffff; if (cb.mem_write8) cb.mem_write8(a, d); }
	uint16_t mem_rw(uint32_t a);
	void mem_ww(uint32_t a, uint16_t d);

	// ======================= V33A core (nec.cpp) =======================
	union necbasicregs
	{                   /* eight general registers */
		uint16_t w[8];    /* viewed as 16 bits registers */
		uint8_t  b[16];   /* or as 8 bit registers */
	};

	necbasicregs m_regs;
	uint16_t  m_sregs[4];

	uint16_t  m_ip;
	uint16_t  m_prev_ip;
	uint16_t  m_rep_ip;

	/* PSW flags */
	int32_t   m_SignVal;
	uint32_t  m_AuxVal;   /* 0 or non-0 valued flags */
	uint32_t  m_OverVal;
	uint32_t  m_ZeroVal;
	uint32_t  m_CarryVal;
	uint32_t  m_ParityVal;
	uint8_t   m_TF; /* 0 or 1 valued flags */
	uint8_t   m_IF;
	uint8_t   m_DF;
	uint8_t   m_MF;

	/* interrupt related */
	uint32_t  m_pending_irq;
	uint32_t  m_nmi_state;
	uint32_t  m_irq_state;
	uint32_t  m_poll_state;
	uint8_t   m_no_interrupt;
	uint8_t   m_halted;
	uint32_t  m_rep_params;
	bool      m_io_retry;
	bool      m_io_word_pending;
	uint8_t   m_io_word_latch;

	int     m_icount;

	int32_t   m_cur_cycles;
	static constexpr uint8_t m_prefetch_size = 6;     // v33_base_device: 6
	static constexpr int32_t m_prefetch_cycles = 1;   // v33_base_device: 1
	int32_t   m_prefetch_count;
	uint8_t   m_prefetch_reset;
	static constexpr uint32_t m_chip_type = 0;        // V33_TYPE
	static constexpr bool m_has_v30_quirks = false;
	static constexpr bool m_has_div_quirk = false;

	uint32_t  m_prefix_base;    /* base address of the latest prefix segment */
	uint8_t   m_seg_prefix;     /* prefix segment indicator */

	uint32_t m_EA;
	uint16_t m_EO;
	uint16_t m_E16;

	uint8_t m_em;

	bool m_xa;

	typedef void (V53A::*nec_ophandler)();
	typedef uint32_t (V53A::*nec_eahandler)();
	static const nec_ophandler s_nec_instruction[256];
	static const nec_eahandler s_GetEA[192];

	uint32_t v33_translate(uint32_t addr) const
	{
		if (m_xa)
			return uint32_t(m_v33_transtable[(addr >> 14) & 63]) << 14 | (addr & 0x03fff);
		else
			return addr & 0xfffff;
	}

	void core_start();
	void core_reset();
	void execute_run();

	void set_int_line(int state);
	void set_nmi_line(int state);
	void set_poll_line(int state);
	uint8_t standard_irq_callback() { return m_icu.acknowledge(); }

	// MAME's retry mechanism for I/O accesses is not used
	static constexpr bool access_to_be_redone() { return false; }
	static constexpr bool access_to_be_redone_noclear() { return false; }

	inline void prefetch();
	void do_prefetch();
	inline uint8_t fetch();
	inline uint16_t fetchword();
	uint8_t fetchop();
	void nec_interrupt(unsigned int_num, int source);
	void nec_trap();
	void nec_brk(unsigned int_num);
	void external_int();

	uint8_t start_rep();
	void cont_rep();
	void do_repnc(uint8_t next);
	void do_repc(uint8_t next);
	void do_repne(uint8_t next);
	void do_repe(uint8_t next);
	bool has_v30_quirks() const { return m_has_v30_quirks; }
	void decimal_adjust(bool subtract);
	void ascii_adjust(bool subtract);
	void packed_decimal(bool subtract, bool store);
	void bitfield_flags(unsigned position, unsigned length, bool extract);
	void mul_flags(uint32_t low, bool word);
	void divu_flags(uint32_t remainder, uint32_t divisor, bool word, bool overflow);
	void div_flags(int32_t quotient, bool word, bool overflow);
	void shift_rotate(uint32_t value, uint8_t modrm, uint8_t count, bool word, bool count_one);

#include "v53a_necdecl.h"
};

} // namespace v53

#endif // BURIKI_AUDIOCPU_V53A_H
