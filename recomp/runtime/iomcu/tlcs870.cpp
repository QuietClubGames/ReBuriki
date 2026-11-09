// license:BSD-3-Clause
// copyright-holders:David Haywood
/*************************************************************************************************************

    Toshiba TLCS-870 Series MCUs  (standalone port of MAME's tlcs870 core, TMP87PH40AN memory map)

    Original MAME code by David Haywood.  This file contains the memory map, the special function
    registers, timers, serial, interrupts and the execute loop (MAME tlcs870.cpp).  The opcode
    handlers are in tlcs870_ops*.cpp.

    Differences from MAME, all deliberate:
      - MAME timers (emu_timer) are replaced by absolute cycle deadlines that are checked after every
        instruction; a timer started by an instruction is measured from that instruction's start
        (the same point MAME's scheduler would use, as icount is only decremented after decode()).
      - The internal ROM is visible at 0x8000-0xffff (MAME maps only 0xc000-0xffff; the bottom half of
        the hng64 dump is all zero and MAME's unmapped reads also return 0, so this is equivalent).
      - reset() cancels running timers (MAME's device_reset leaves them scheduled).
      - fatalerror() on a 0-cycle instruction becomes a counter + 1 cycle charge.

    MAME quirks that are reproduced by default (fix_mame_bugs == false):
      - ADD/ADDC/SUB/SUBB (8 and 16 bit) end with "CF = ZF" where the comment says "JF = CF".
      - MUL computes the product but writes the original operands back.
      - RETN pops PSW from SP-1 instead of SP.
      - IL high byte write merges with the low byte of EIR instead of IL.
      Setting fix_mame_bugs = true corrects those four.
    MAME limitations reproduced as-is (regardless of fix_mame_bugs):
      - Maskable interrupts only test IMF (EIR bit 0), not the individual EF enable bits or INT0EN.
      - Non-maskable sources (SWI / WDT / RESET) are latched but never serviced.
      - TC2 fires INTTC2 every 1500 clocks once started (TREG2 / clock source ignored).
      - TC1 / TC3 / TC4 / time base timer / watchdog / SIO2 / SIO receive / IDLE+STOP modes do nothing.
      - SIO1 transmit sends 1 bit per 1000 clocks on serial_out[0].
      - ADC conversion is instantaneous.
      - Unmapped reads return 0; unmapped writes are ignored.

*************************************************************************************************************/

#include "tlcs870.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

Tlcs870::Tlcs870()
{
	std::memset(m_rom, 0, sizeof(m_rom));
	std::memset(m_intram, 0, sizeof(m_intram));
	std::memset(m_dbr, 0, sizeof(m_dbr));
	std::memset(m_port_out_latch, 0, sizeof(m_port_out_latch));
	for (auto& d : m_timer_deadline)
		d = NEVER;
	// device_start
	m_sp = 0x0000;
	m_F = 0;
}

void Tlcs870::load_rom(const uint8_t* data, size_t size)
{
	if (size > sizeof(m_rom))
	{
		// take the last 32 KB (internal ROM is the top of the address space)
		data += size - sizeof(m_rom);
		size = sizeof(m_rom);
	}
	std::memset(m_rom, 0, sizeof(m_rom));
	std::memcpy(m_rom + (sizeof(m_rom) - size), data, size);
}

void Tlcs870::logf(const char* fmt, ...)
{
	if (!log)
		return;
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	log(buf);
}

/*************************************************************************************************************
    Memory map (MAME tmp87ph40an_mem)
*************************************************************************************************************/

void Tlcs870::note_unmapped(bool write, uint32_t a)
{
	if (write)
		m_stats.unmapped_write++;
	else
		m_stats.unmapped_read++;
	if (a < 0x40)
	{
		if (write)
			m_stats.sfr_unmapped_write[a]++;
		else
			m_stats.sfr_unmapped_read[a]++;
	}
	m_stats.last_unmapped_addr = uint16_t(a);
	logf("%04x: unmapped %s %04x", m_tmppc, write ? "write" : "read", a);
}

uint8_t Tlcs870::RM8(uint32_t a)
{
	a &= 0xffff;
	if (a >= 0x8000)
	{
		if (a < 0xc000)
			m_stats.rom_lo_access++;
		return m_rom[a - 0x8000];
	}
	if (a < 0x0040)
		return sfr_read(uint16_t(a));
	if (a < 0x0240)
		return m_intram[a - 0x40];
	if (a >= 0x0f80 && a < 0x1000)
		return m_dbr[a - 0x0f80];
	note_unmapped(false, a);
	return 0x00;
}

void Tlcs870::WM8(uint32_t a, uint8_t v)
{
	a &= 0xffff;
	if (a >= 0x8000)
	{
		m_stats.rom_write++;
		logf("%04x: write to ROM %04x = %02x", m_tmppc, a, v);
		return;
	}
	if (a < 0x0040)
	{
		sfr_write(uint16_t(a), v);
		return;
	}
	if (a < 0x0240)
	{
		m_intram[a - 0x40] = v;
		return;
	}
	if (a >= 0x0f80 && a < 0x1000)
	{
		m_dbr[a - 0x0f80] = v;
		return;
	}
	note_unmapped(true, a);
}

uint8_t Tlcs870::peek(uint16_t a) const
{
	if (a >= 0x8000) return m_rom[a - 0x8000];
	if (a >= 0x40 && a < 0x240) return m_intram[a - 0x40];
	if (a >= 0xf80 && a < 0x1000) return m_dbr[a - 0xf80];
	switch (a)
	{
	case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
		return m_port_out_latch[a];
	case 0x0e: return m_ADCCR | 0x80;
	case 0x0f: return m_ADCDR;
	case 0x12: return m_TREG1B & 0xff;
	case 0x13: return (m_TREG1B >> 8) & 0xff;
	case 0x18: return m_TREG3A;
	case 0x19: return m_TREG3B;
	case 0x36: return m_TBTCR;
	case 0x38: return m_SYSCR1;
	case 0x39: return m_SYSCR2 | 0x0f;
	case 0x3a: return m_EIR & 0xff;
	case 0x3b: return (m_EIR >> 8) & 0xff;
	case 0x3c: return m_IL & 0xff;
	case 0x3d: return (m_IL >> 8) & 0xff;
	case 0x3f: return get_PSW();
	default: return 0x00;
	}
}

uint8_t Tlcs870::sfr_read(uint16_t a)
{
	switch (a)
	{
	case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
		return port_r(a);
	case 0x0e: return m_ADCCR | 0x80;            // adccr_r: return with 'finished' bit set
	case 0x0f: return m_ADCDR;                   // adcdr_r
	case 0x12: return m_TREG1B & 0xff;           // treg1b_l_r
	case 0x13: return (m_TREG1B >> 8) & 0xff;    // treg1b_h_r
	case 0x18: return m_TREG3A;                  // treg3a_r
	case 0x19: return m_TREG3B;                  // treg3b_r
	case 0x20: return 0x00;                      // sio1sr_r
	case 0x22: return 0x00;                      // sio2sr_r
	case 0x36: return m_TBTCR;                   // tbtcr_r
	case 0x37: return 0x00;                      // eintcr_r
	case 0x38: return m_SYSCR1;                  // syscr1_r (low 2 bits 'undefined')
	case 0x39: return m_SYSCR2 | 0x0f;           // syscr2_r (low bits always read as 1)
	case 0x3a: return m_EIR & 0xff;
	case 0x3b: return (m_EIR >> 8) & 0xff;
	case 0x3c: return m_IL & 0xff;
	case 0x3d: return (m_IL >> 8) & 0xff;
	case 0x3f: return get_PSW();                 // psw_r
	default:
		// reserved, or write-only register (MAME: unmapped read -> 0)
		note_unmapped(false, a);
		return 0x00;
	}
}

void Tlcs870::sfr_write(uint16_t a, uint8_t data)
{
	switch (a)
	{
	case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
		port_w(a, data); break;
	case 0x0a: m_port0_cr = data; break;         // Port 0 I/O control
	case 0x0b: m_port1_cr = data; break;         // Port 1 I/O control
	case 0x0c: m_port6_cr = data; break;         // Port 6 I/O control
	case 0x0d: m_port7_cr = data; break;         // Port 7 I/O control
	case 0x0e: adccr_w(data); break;
	case 0x10: m_TREG1A = (m_TREG1A & 0xff00) | data; break;
	case 0x11: m_TREG1A = (m_TREG1A & 0x00ff) | (data << 8); break;
	case 0x12: m_TREG1B = (m_TREG1B & 0xff00) | data; break;
	case 0x13: m_TREG1B = (m_TREG1B & 0x00ff) | (data << 8); break;
	case 0x14: tc1cr_w(data); break;
	case 0x15: tc2cr_w(data); break;
	case 0x16: m_TREG2 = (m_TREG2 & 0xff00) | data; break;
	case 0x17: m_TREG2 = (m_TREG2 & 0x00ff) | (data << 8); break;
	case 0x18: m_TREG3A = data; break;
	case 0x1a: m_TC3CR = data; break;
	case 0x1b: m_TREG4 = data; break;
	case 0x1c: m_TC4CR = data; break;
	case 0x20: sio1cr1_w(data); break;
	case 0x21: sio1cr2_w(data); break;
	case 0x22: m_SIOCR1[1] = data; break;        // sio2cr1_w (no transfer emulation in MAME)
	case 0x23: m_SIOCR2[1] = data; break;        // sio2cr2_w
	case 0x34: m_WDTCR1 = data; break;           // wdtcr1_w (WDTOUT cannot be set to 1 by software; not enforced in MAME)
	case 0x35: wdtcr2_w(data); break;
	case 0x36: m_TBTCR = data; break;            // tbtcr_w (time base timer not emulated in MAME)
	case 0x37: m_EINTCR = data; break;           // eintcr_w
	case 0x38: syscr1_w(data); break;
	case 0x39: syscr2_w(data); break;
	case 0x3a: m_EIR = (m_EIR & 0xff00) | data; break;
	case 0x3b: m_EIR = (m_EIR & 0x00ff) | (data << 8); break;
	case 0x3c: il_l_w(data); break;
	case 0x3d: il_h_w(data); break;
	case 0x3f: m_RBS = data & 0x0f; break;       // rbs_w: flags can't be written, only the register bank
	default:
		note_unmapped(true, a);                  // reserved / read-only
		break;
	}
}

/*************************************************************************************************************
    Ports
*************************************************************************************************************/

uint8_t Tlcs870::port_r(int n)
{
	if (m_read_input_port)
	{
		uint8_t v = port_in[n] ? port_in[n]() : 0xff;
		if (n == 2) v |= 0xf8; // 3-bit port
		if (n == 5) v |= 0xe0; // 5-bit port
		return v;
	}
	return m_port_out_latch[n];
}

void Tlcs870::port_w(int n, uint8_t data)
{
	m_port_out_latch[n] = data;
	if (port_out[n])
		port_out[n](data);
}

/*************************************************************************************************************
    Timers
*************************************************************************************************************/

void Tlcs870::run_timers(uint64_t upto)
{
	for (;;)
	{
		int best = -1;
		uint64_t when = NEVER;
		for (int t = 0; t < TMR_COUNT; t++)
		{
			if (m_timer_deadline[t] <= upto && m_timer_deadline[t] < when)
			{
				when = m_timer_deadline[t];
				best = t;
			}
		}
		if (best < 0)
			return;
		const uint64_t saved = m_now;
		m_now = when; // a timer that re-arms itself does so relative to its own expiry time
		m_timer_deadline[best] = NEVER;
		timer_fire(best);
		m_now = saved;
	}
}

void Tlcs870::timer_fire(int t)
{
	switch (t)
	{
	case TMR_SIO0: sio0_transmit_cb(); break;
	case TMR_SIO1: break; // sio1_transmit_cb: empty in MAME
	case TMR_TC1: break;  // tc1_cb: empty
	case TMR_TC2:         // tc2_cb
		m_IL |= 1 << (15 - TLCS870_IRQ_INTTC2);
		tc2_reload();
		break;
	case TMR_TC3: break;  // tc3_cb: empty
	case TMR_TC4: break;  // tc4_cb: empty
	}
}

// 16-Bit Timer / Counter 1 (TC1) - control register only
void Tlcs870::tc1cr_w(uint8_t data)
{
	m_TC1CR = data;
}

// 16-Bit Timer / Counter 2 (TC2)
void Tlcs870::tc2_reload()
{
	timer_adjust(TMR_TC2, 1500); // TODO (MAME): use real value
}

void Tlcs870::tc2_cancel()
{
	timer_cancel(TMR_TC2);
}

void Tlcs870::tc2cr_w(uint8_t data)
{
	if (data & 0x20)
	{
		if (!(m_TC2CR & 0x20))
			tc2_reload();
	}
	else
	{
		tc2_cancel();
	}
	m_TC2CR = data;
}

/*************************************************************************************************************
    SIO
*************************************************************************************************************/

void Tlcs870::sio1cr1_w(uint8_t data)
{
	m_SIOCR1[0] = data;

	m_transfer_mode[0] = (m_SIOCR1[0] & 0x38) >> 3;
	switch (m_transfer_mode[0])
	{
	case 0x0: m_transmit_bits[0] = 8; m_receive_bits[0] = 0; break; // 8-bit transmit
	case 0x2: m_transmit_bits[0] = 4; m_receive_bits[0] = 0; break; // 4-bit transmit
	case 0x4: m_transmit_bits[0] = 8; m_receive_bits[0] = 8; break; // 8-bit transmit/receive
	case 0x5: m_transmit_bits[0] = 0; m_receive_bits[0] = 8; break; // 8-bit receive
	case 0x6: m_transmit_bits[0] = 0; m_receive_bits[0] = 4; break; // 4-bit receive
	default:  m_transmit_bits[0] = 0; m_receive_bits[0] = 0; break; // invalid
	}

	if ((m_SIOCR1[0] & 0xc0) == 0x80)
	{
		// start transfer
		m_transfer_shiftpos[0] = 0;
		m_transfer_shiftreg[0] = 0;
		m_transfer_pos[0] = 0;
		timer_adjust(TMR_SIO0, 0);
	}
}

void Tlcs870::sio1cr2_w(uint8_t data)
{
	m_SIOCR2[0] = data;
	m_transfer_numbytes[0] = (m_SIOCR2[0] & 0x7);
}

void Tlcs870::sio0_transmit_cb()
{
	if (m_transmit_bits[0]) // TODO (MAME): handle receive cases
	{
		int finish = 0;
		if (m_transfer_shiftpos[0] == 0)
			m_transfer_shiftreg[0] = m_dbr[0x70 + (m_transfer_pos[0] & 0x0f)]; // DBR 0x0ff0+

		const int dataout = m_transfer_shiftreg[0] & 0x01;
		if (serial_out[0])
			serial_out[0](dataout);

		m_transfer_shiftreg[0] >>= 1;
		m_transfer_shiftpos[0]++;

		if (m_transfer_shiftpos[0] == 8)
		{
			m_transfer_shiftpos[0] = 0;
			m_transfer_pos[0]++;

			if (m_transfer_pos[0] > m_transfer_numbytes[0])
			{
				// end of transmission
				m_SIOCR1[0] &= ~0x80;
				m_IL |= 1 << (15 - TLCS870_IRQ_INTSIO1);
				finish = 1;
			}
		}

		if (!finish)
			timer_adjust(TMR_SIO0, 1000); // TODO (MAME): use real speed
	}
}

/*************************************************************************************************************
    WDT / system control / ADC / interrupt registers
*************************************************************************************************************/

void Tlcs870::wdtcr2_w(uint8_t data)
{
	if (data == 0x4e)
	{
		m_stats.wdt_clear++; // clear watchdog counter (watchdog not emulated in MAME)
	}
	else if (data == 0xb1)
	{
		m_stats.wdt_disable++; // disable code (only effective if WDTEN cleared first)
	}
}

void Tlcs870::syscr1_w(uint8_t data)
{
	m_SYSCR1 = data;
	if (data & 0x80)
	{
		m_stats.idle_requests++;
		logf("%04x: SYSCR1 STOP requested (not emulated)", m_tmppc);
	}
}

void Tlcs870::syscr2_w(uint8_t data)
{
	m_SYSCR2 = data;
	if (data & 0x10)
	{
		m_stats.idle_requests++; // hng64 sets this in case of ram test failures
		logf("%04x: SYSCR2 IDLE requested (not emulated)", m_tmppc);
	}
}

void Tlcs870::adccr_w(uint8_t data)
{
	m_ADCCR = data;
	if (data & 0x40)
		m_ADCDR = analog_in[data & 0x07] ? analog_in[data & 0x07]() : 0xff;
}

/*
    the READ/WRITE/MODIFY operations cannot be used to clear interrupt latches
    also you can't set a latch by writing '1' to it, only clear a latch by writing 0 to it
    (MAME: "probably not this logic")
*/
void Tlcs870::il_l_w(uint8_t data)
{
	m_IL = (m_IL & 0xff00) | data;
}

void Tlcs870::il_h_w(uint8_t data)
{
	if (fix_mame_bugs)
		m_IL = (m_IL & 0x00ff) | (data << 8);
	else
		m_IL = (m_EIR & 0x00ff) | (data << 8); // MAME quirk
}

/*************************************************************************************************************
    Interrupts
*************************************************************************************************************/

void Tlcs870::set_input_line(int line, bool asserted)
{
	int irqline = -1;
	switch (line)
	{
	case INPUT_INT5: irqline = 15; break;
	case INPUT_INT4: irqline = 12; break;
	case INPUT_INT3: irqline = 11; break;
	case INPUT_INT2: irqline = 7; break;
	case INPUT_INT1: irqline = 5; break;
	case INPUT_INT0: irqline = 3; break;
	}
	if (irqline != -1)
		set_irq_line(irqline, asserted ? 1 : 0);
}

void Tlcs870::set_irq_line(int irqline, int state)
{
	if (!(m_irqstate & (1 << irqline)))
	{
		// rising edge
		if (state)
		{
			m_irqstate |= 1 << irqline;
			// TODO (MAME): check if interrupt pin(s) are configured, and if they're in rising edge mode
			m_IL |= 1 << irqline;
		}
	}
	else
	{
		if (!state)
			m_irqstate &= ~(1 << irqline);
	}
}

void Tlcs870::check_interrupts()
{
	// priority 0-2 are non-maskable, and should have already been processed before we get here
	for (int priority = 0; priority <= 15; priority++)
	{
		if (priority >= 3) // only priorities 0,1,2 are non-maskable
		{
			if (!(m_EIR & 1))
				continue; // maskable interrupts are disabled

			if (m_IL & (1 << priority))
			{
				take_interrupt(priority);
				return;
			}
		}
		else
		{
			// TODO (MAME): handle non-maskable here
		}
	}
}

void Tlcs870::take_interrupt(int priority)
{
	m_IL &= ~(1 << priority);
	m_EIR &= ~1;
	m_stats.interrupts_taken++;

	const uint16_t vector = RM16(0xffe0 + ((15 - priority) * 2));

	WM8(m_sp, get_PSW());
	WM16(m_sp - 2, uint16_t(m_addr));
	m_sp -= 3;

	m_pc = vector;
}

/*************************************************************************************************************
    Execution
*************************************************************************************************************/

int Tlcs870::run(int cycles)
{
	if (cycles <= 0)
		return 0;

	int executed = 0;
	while (executed < cycles)
	{
		check_interrupts();

		m_prvpc = m_pc;

		m_addr = m_pc;
		m_tmppc = uint16_t(m_addr); // used for jumps etc.
		m_cycles = 0;
		m_read_input_port = 1; // some operations force the output latches to read from the memory mapped ports, not input ports
		decode();
		m_pc = m_addr;

		int c = m_cycles;
		if (!c)
		{
			// MAME: fatalerror("m_cycles == 0 after PC %04x\n", m_tmppc);
			m_stats.zero_cycle_instr++;
			logf("%04x: instruction took 0 cycles", m_tmppc);
			c = 1;
		}
		// MAME: m_icount -= m_cycles; (1 machine cycle counted as 1 clock)
		executed += c;
		m_total_cycles += c;
		m_now = m_total_cycles;
		run_timers(m_now);
	}
	return executed;
}

void Tlcs870::reset()
{
	m_pc = RM16(0xfffe);
	m_prvpc = m_pc;
	m_addr = m_pc;
	m_tmppc = uint16_t(m_pc);

	m_RBS = 0x00;
	m_EIR = 0x0000;
	m_IL = 0x0000;
	m_EINTCR = 0x00;
	m_ADCCR = 0x00;
	m_ADCDR = 0x00;
	m_SYSCR1 = 0x00;
	m_SYSCR2 = 0x80;
	m_TBTCR = 0x00;

	m_TREG1A = 0x1234; // not initialized?
	m_TREG1B = 0x4321; // not initialized?
	m_TC1CR = 0x00;

	m_TREG2 = 0x2301; // not initialized?
	m_TC2CR = 0x00;

	m_TREG3A = 0x10; // not initialized?
	m_TREG3B = 0x32; // not initialized?
	m_TC3CR = 0x00;

	m_TREG4 = 0x30; // not initialized?
	m_TC3CR = 0x00; // (sic, MAME resets TC3CR twice and never TC4CR)

	m_SIOCR1[0] = 0x00;
	m_SIOCR1[1] = 0x00;
	m_SIOCR2[0] = 0x00;
	m_SIOCR2[1] = 0x00;

	m_WDTCR1 = 0x09;

	m_irqstate = 0;
	for (int i = 0; i < 2; i++)
	{
		m_transfer_numbytes[i] = 0;
		m_transfer_mode[i] = 0;
		m_transfer_pos[i] = 0;
		m_transfer_shiftreg[i] = 0;
		m_transfer_shiftpos[i] = 0;
	}

	m_port_out_latch[0] = 0x00;
	m_port_out_latch[1] = 0x00;
	m_port_out_latch[2] = 0xff;
	m_port_out_latch[3] = 0xff;
	m_port_out_latch[4] = 0xff;
	m_port_out_latch[5] = 0xff;
	m_port_out_latch[6] = 0x00;
	m_port_out_latch[7] = 0x00;

	m_port0_cr = 0xff;
	m_port1_cr = 0xff;
	m_port6_cr = 0xff;
	m_port7_cr = 0xff;

	m_read_input_port = 1;

	for (auto& d : m_timer_deadline)
		d = NEVER;
	m_now = m_total_cycles;
}
