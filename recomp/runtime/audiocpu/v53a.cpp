// license:BSD-3-Clause
// copyright-holders:Patrick Mackinlay
/*
 * NEC V5x devices consist of a V3x CPU core plus integrated peripherals. The
 * CPU cores within each device are as follows:
 *
 *   Device            CPU
 *   V40 (µPD70208)    V20 (µPD70108)
 *   V50 (µPD70216)    V30 (µPD70116)
 *   V53 (µPD70236)    V33 (µPD70136)
 *   V53A (µPD70236A)  V33A (µPD70136A)
 *
 * The peripherals are nearly identical between all four devices:
 *
 *   Name  Description             Device
 *   TCU   Timer/Counter Unit      µPD71054/i8254 subset
 *   DMAU  DMA Control Unit        µPD71071 equivalent
 *   ICU   Interrupt control Unit  µPD71059/i8259 equivalent
 *   SCU   Serial Control Unit     µPD71051/i8251 subset (async only)
 *
 * The V53/V53A DMAU also supports a configurable µPD71037/i8237A mode.
 *
 * Sources:
 *
 *   http://www.chipfind.net/datasheet/pdf/nec/upd70236.pdf
 *   https://datasheet.datasheetarchive.com/originals/scans/Scans-107/DSASCANS15-59637.pdf
 *
 * Standalone port of MAME src/devices/cpu/nec/v5x.cpp (v53_device / v53a_device
 * only) plus the scheduling glue that replaces MAME's device_scheduler.
 */

#include "v53a.h"

#include <cstdarg>
#include <cstdio>

namespace v53 {

#define VBIT(x, n) (((x) >> (n)) & 1)

V53A::V53A(uint32_t clock)
	: m_clock(clock)
	, m_tcu(*this)
	, m_dmau(*this)
	, m_icu(*this)
	, m_scu(*this)
{
	// device_start (power on)
	core_start();
	m_tcu.start();
	m_dmau.start();
	m_icu.start();
	m_scu.start();
	m_OPSEL = m_SULA = m_TULA = m_IULA = m_DULA = m_OPHA = m_TCKS = m_BRC = 0;
	m_brc_enable = false;
	m_SCTL = 0;
}

//**************************************************************************
//  scheduler
//**************************************************************************

uint64_t V53A::now() const
{
	if (m_in_cpu)
		return m_slice_start + uint64_t(int64_t(m_slice_len) - int64_t(m_icount));
	return m_cur;
}

void V53A::abort_timeslice()
{
	if (m_in_cpu && m_icount > 0)
	{
		m_slice_len -= m_icount;
		m_icount = 0;
	}
}

void V53A::schedule_event(uint64_t when)
{
	// an event earlier than the end of the running slice ends the slice
	// after the current instruction (MAME: timer insertion aborts the timeslice)
	if (m_in_cpu && when < m_slice_start + uint64_t(m_slice_len))
		abort_timeslice();
}

void V53A::icu_request_eval()
{
	// pic8259 m_irq_timer->adjust(attotime::zero)
	const uint64_t t = now();
	if (!m_icu_eval_pending || t < m_icu_eval_time)
		m_icu_eval_time = t;
	m_icu_eval_pending = true;
	schedule_event(t);
}

void V53A::dma_resume()
{
	const uint64_t t = now();
	if (m_dma_next < t)
		m_dma_next = t;
	schedule_event(m_dma_next);
}

uint64_t V53A::next_event_time() const
{
	uint64_t t = m_tcu.next_event();
	if (m_icu_eval_pending && m_icu_eval_time < t)
		t = m_icu_eval_time;
	if (!m_dmau.waiting() && m_dma_next < t)
		t = m_dma_next;
	if (m_brc_hz && !brc_lazy())
	{
		const uint64_t b = m_brc_start + ((m_brc_k + 1) * uint64_t(m_clock) + m_brc_hz - 1) / m_brc_hz;
		if (b < t)
			t = b;
	}
	return t;
}

void V53A::process_events(uint64_t until)
{
	for (;;)
	{
		// find the earliest event; ties: ICU, TCU, DMAU, BRC
		uint64_t best = NEVER;
		int kind = -1;
		int which = -1;

		if (m_icu_eval_pending)
		{
			best = m_icu_eval_time;
			kind = 0;
		}
		uint64_t tt;
		const int ctr = m_tcu.next_counter(tt);
		if (ctr >= 0 && tt < best)
		{
			best = tt;
			kind = 1;
			which = ctr;
		}
		if (!m_dmau.waiting() && m_dma_next < best)
		{
			best = m_dma_next;
			kind = 2;
		}
		if (m_brc_hz && !brc_lazy())
		{
			const uint64_t b = m_brc_start + ((m_brc_k + 1) * uint64_t(m_clock) + m_brc_hz - 1) / m_brc_hz;
			if (b < best)
			{
				best = b;
				kind = 3;
			}
		}

		if (kind < 0 || best > until)
			break;

		m_cur = best;
		stat_events++;
		stat_kind[kind]++;
		switch (kind)
		{
		case 0:
			m_icu_eval_pending = false;
			m_icu.irq_timer_tick();
			break;
		case 1:
			m_tcu.fire(which);
			break;
		case 2:
			m_dma_next += DMAU_CLOCK_DIVIDER;
			m_dmau.step();
			break;
		case 3:
			m_brc_k++;
			brc_tick();
			break;
		}
	}
	m_cur = until;
}

int V53A::run(int cycles)
{
	const uint64_t start = m_time;
	const uint64_t target = m_time + uint64_t(cycles > 0 ? cycles : 0);

	m_cur = m_time;
	// events made due by calls between run()s (input lines, dreq, ...)
	process_events(m_time);
	while (m_time < target)
	{
		uint64_t next = next_event_time();
		if (next <= m_time)
			next = m_time + 1; // can't happen after process_events(), but always make progress
		if (next > target)
			next = target;

		if (!m_halt_line && !m_reset_line)
		{
			uint64_t len = next - m_time;
			if (len > 0x10000000)
				len = 0x10000000;
			m_slice_start = m_time;
			m_slice_len = int(len);
			m_icount = int(len);
			m_in_cpu = true;
			soft_cut_update();
			stat_slices++;
			execute_run();
			m_in_cpu = false;
			m_time = m_slice_start + uint64_t(int64_t(m_slice_len) - int64_t(m_icount));
		}
		else
		{
			m_time = next;
		}

		process_events(m_time);
	}
	m_cur = m_time;
	return int(m_time - start);
}

void V53A::logerror(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	m_log_count++;
	m_last_log = buf;
	if (cb.log)
		cb.log(buf);
}

//**************************************************************************
//  inputs
//**************************************************************************

// the external interface provides no external access to the usual IRQ line of the V33, everything goes through the interrupt controller
void V53A::set_input_line(int line, bool asserted)
{
	const int state = asserted ? 1 : 0;
	switch (line)
	{
		case LINE_IRQ0: case LINE_IRQ1: case LINE_IRQ2: case LINE_IRQ3:
		case LINE_IRQ4: case LINE_IRQ5: case LINE_IRQ6: case LINE_IRQ7:
			m_icu.set_irq_line(line, state);
			break;

		case LINE_NMI: set_nmi_line(state); break;
		case LINE_POLL: set_poll_line(state); break;

		default:
			logerror("set_input_line: unknown line %d\n", line);
			break;
	}
}

void V53A::set_halt_line(bool asserted)
{
	m_halt_line = asserted;
	if (asserted)
		abort_timeslice();
}

void V53A::set_reset_line(bool asserted)
{
	if (asserted)
	{
		m_reset_line = true;
		abort_timeslice();
	}
	else if (m_reset_line)
	{
		m_reset_line = false;
		reset();
	}
}

void V53A::dreq(int channel, bool state)
{
	// dreq0 could be wrong / nonexistent
	if (!(m_SCTL & 0x02))
	{
		m_dmau.dreq_w(channel & 3, state ? 1 : 0);
	}
	else
	{
		logerror("dreq%d not in 71071mode\n", channel);
	}
}

void V53A::hack_w(int state)
{
	if (!(m_SCTL & 0x02))
		m_dmau.hack_w(state);
	else
		logerror("hack_w not in 71071mode\n");
}

void V53A::set_tclk(uint32_t hz)
{
	m_tclk = hz;
	tcu_clock_update();
}

void V53A::tclk_w(int state)
{
	if (VBIT(m_TCKS, 2))
		m_tcu.write_clk(0, state);
	if (VBIT(m_TCKS, 3))
		m_tcu.write_clk(1, state);
	if (VBIT(m_TCKS, 4))
		m_tcu.write_clk(2, state);
}

//**************************************************************************
//  reset
//**************************************************************************

void V53A::interface_pre_reset()
{
	m_OPSEL= 0x00;

	// peripheral addresses
	m_SULA = 0x00;
	m_TULA = 0x00;
	m_IULA = 0x00;
	m_DULA = 0x00;
	m_OPHA = 0x00;
	m_BRC  = 0x00;

	m_TCKS = 0x00;
	m_brc_enable = false;
	tcu_clock_update();
}

void V53A::reset()
{
	const bool was_in_cpu = m_in_cpu;
	if (!was_in_cpu)
		m_cur = m_time;

	// device_t::reset(): interfaces pre-reset, device_reset, then child devices
	interface_pre_reset();

	// v53_device::device_reset
	core_reset();
	m_SCTL = 0x00;

	// children: tcu (and its counters), dmau, icu, scu (brc timer keeps running, as in MAME)
	m_tcu.reset();
	m_dmau.reset();
	m_icu.reset();
	brc_sync();
	m_scu.reset();
	brc_resched();
	m_icu_eval_pending = false;
	if (m_dma_next < now())
		m_dma_next = now();
}

//**************************************************************************
//  V53 internal registers
//**************************************************************************

void V53A::tcu_clock_update()
{
	for (int i = 0; i < 3; i++)
	{
		uint64_t period;
		if (VBIT(m_TCKS, i + 2))
			period = m_tclk ? (uint64_t(m_clock) + m_tclk / 2) / m_tclk : 0; // external TCLK (rounded to whole CPU clocks)
		else
			period = uint64_t(4) << (m_TCKS & 3); // clock / (4 << (TCKS & 3))
		m_tcu.set_clock_period(i, period);
	}
}

void V53A::brc_update()
{
	brc_sync(); // the old clock's ticks, before it is restarted
	if (m_brc_enable)
	{
		const int divider = (m_BRC < 2) ? 2 : m_BRC;
		const int period = int((m_clock / 2) / divider);

		// m_brc_timer->adjust(attotime::from_hz(period), 0, attotime::from_hz(period));
		m_brc_hz = period > 0 ? uint64_t(period) : 0;
		m_brc_start = now();
		m_brc_k = 0;
		m_soft_valid = false;
		brc_resched();
	}
}

void V53A::brc_sync()
{
	if (!m_brc_hz)
		return;
	// Tick k (k = 1, 2, ...) is due at m_brc_start + ceil(k * clock / hz). Ticked one at a time, every tick due
	// at or before the start of the current instruction has been applied when that instruction runs, and none
	// later. So: bring the count up to exactly those.
	const uint64_t t = m_in_cpu ? m_slice_start + uint64_t(m_instr_elapsed) : m_cur;
	if (t <= m_brc_start)
		return;
	const uint64_t due = (t - m_brc_start) * m_brc_hz / uint64_t(m_clock);
	if (due > m_brc_k)
	{
		m_scu.skip_idle_ticks(due - m_brc_k);
		m_brc_k = due;
	}
}

void V53A::brc_resched()
{
	if (m_brc_hz && !brc_lazy())
	{
		m_soft_icount = 0;
		schedule_event(m_brc_start + ((m_brc_k + 1) * uint64_t(m_clock) + m_brc_hz - 1) / m_brc_hz);
	}
	else if (m_in_cpu)
		soft_cut_update();
}

// While the baud-rate clock's ticks are not events (the serial unit is idle), the CPU's run is no longer cut
// at each tick. One thing in the core depends on those cuts: a REP string instruction stops at the end of a
// run and carries on in the next, and each stop costs a few clocks (the CLK(2) and the prefetch bookkeeping
// are charged again). MAME's timing has those stops, about 8,400 a frame here, so they are kept: REP loops
// stop when the clock count reaches m_soft_icount, the point in the current run where the next tick falls.
// Everything else in the core behaves the same with or without a cut between two instructions.
void V53A::soft_cut_update()
{
	if (!(m_brc_hz && brc_lazy()))
	{
		m_soft_icount = 0;
		return;
	}
	const uint64_t t = m_slice_start + uint64_t(int64_t(m_slice_len) - int64_t(m_icount));        // now
	const uint64_t clock = uint64_t(m_clock);
	// The next tick after now: tick j is at m_brc_start + ceil(j * clock / hz), kept as the whole part
	// m_soft_floor and the remainder m_soft_rem of j * clock / hz so that stepping to the following tick
	// needs no division (this runs once per tick).
	if (!m_soft_valid || t < m_soft_time)
	{
		const uint64_t k = t > m_brc_start ? (t - m_brc_start) * m_brc_hz / clock : 0;             // ticks due by now
		m_soft_floor = (k + 1) * clock / m_brc_hz;
		m_soft_rem = (k + 1) * clock % m_brc_hz;
		m_soft_q = clock / m_brc_hz;
		m_soft_r = clock % m_brc_hz;
		m_soft_valid = true;
	}
	const uint64_t q = m_soft_q, r = m_soft_r;
	while (m_brc_start + m_soft_floor + (m_soft_rem ? 1 : 0) <= t)
	{
		m_soft_floor += q;
		m_soft_rem += r;
		if (m_soft_rem >= m_brc_hz)
		{
			m_soft_rem -= m_brc_hz;
			m_soft_floor++;
		}
	}
	m_soft_time = t;
	const uint64_t b = m_brc_start + m_soft_floor + (m_soft_rem ? 1 : 0);
	const uint64_t end = m_slice_start + uint64_t(m_slice_len);
	m_soft_icount = b < end ? int(end - b) : 0;
}

void V53A::brc_tick()
{
	m_scu.write_txc(0);
	m_scu.write_txc(1);
	m_scu.write_rxc(0);
	m_scu.write_rxc(1);
}

void V53A::SCTL_w(uint8_t data)
{
	// bit 7: unused
	// bit 6: unused
	// bit 5: unused
	// bit 4: SCU input clock source
	// bit 3: uPD71037 DMA mode - Carry A20
	// bit 2: uPD71037 DMA mode - Carry A16
	// bit 1: uPD71037 DMA mode enable (otherwise in uPD71071 mode)
	// bit 0: Onboard pripheral I/O maps to 8-bit boundaries? (otherwise 16-bit)

	brc_sync();
	m_SCTL = data & 0x1f;
	m_brc_enable = VBIT(data, 4);

	if (m_SCTL & 0x02)
		logerror("SCTL_w %02x: uPD71037 DMA mode selected (not implemented, DMAU unmapped)\n", data);

	if (m_brc_enable)
	{
		brc_update();
	}
	else
	{
		m_brc_hz = 0; // m_brc_timer->adjust(attotime::never)
		m_soft_valid = false;
	}
}

void V53A::tout1_w(int state)
{
	if (!VBIT(m_SCTL, 4))
	{
		brc_sync();
		m_scu.write_rxc(state);
		m_scu.write_txc(state);
		brc_resched();
	}

	if (cb.tout)
		cb.tout(1, state);
}

void V53A::tcu_out(int n, int state)
{
	if (n == 1)
		tout1_w(state);
	else if (cb.tout)
		cb.tout(n, state);
}

// v53_device::internal_port_map + v33_base_device::v33_internal_port_map (0xff00-0xffff)
uint8_t V53A::fixed_reg_read(uint32_t port)
{
	port &= 0xffff;
	if (port < 0xff80)
	{
		// 0xff00-0xff7f: translation table RAM (16-bit words)
		const uint16_t w = m_v33_transtable[(port >> 1) & 63];
		return (port & 1) ? uint8_t(w >> 8) : uint8_t(w);
	}
	switch (port)
	{
		case 0xff80: return m_xa ? 1 : 0; // xam_r: only bit 0 is defined
		case 0xff81: return 0;
		case 0xfff0: return m_TCKS;
		case 0xfff8: return m_SULA;
		case 0xfff9: return m_TULA;
		case 0xfffa: return m_IULA;
		case 0xfffb: return m_DULA;
		case 0xfffc: return m_OPHA;
		case 0xfffd: return m_OPSEL;
		case 0xfffe: return m_SCTL;
		default: return 0; // unmapped or write-only
	}
}

void V53A::fixed_reg_write(uint32_t port, uint8_t v)
{
	port &= 0xffff;
	if (port < 0xff80)
	{
		uint16_t &w = m_v33_transtable[(port >> 1) & 63];
		if (port & 1)
			w = uint16_t((w & 0x00ff) | (v << 8));
		else
			w = uint16_t((w & 0xff00) | v);
		return;
	}
	switch (port)
	{
		case 0xffe0: break;  // BSEL_w: uPD71037 DMA mode bank selection register (no-op in MAME)
		case 0xffe1: break;  // BADR_w: uPD71037 DMA mode bank register peripheral mapping (no-op in MAME)
		case 0xffe9: BRC_w(v); break; // baud rate counter (used for serial peripheral)
		case 0xffea: break;  // WMB0_w waitstate control (no-op in MAME)
		case 0xffeb: break;  // WCY1_w
		case 0xffec: break;  // WCY0_w
		case 0xffed: break;  // WAC_w
		case 0xfff0: TCKS_w(v); break; // timer clocks
		case 0xfff1: break;  // SBCR_w internal clock divider, halt behavior etc. (no-op in MAME)
		case 0xfff2: break;  // RFC_w ram refresh control (no-op in MAME)
		case 0xfff3: break;  // WMB1_w
		case 0xfff4: break;  // WCY2_w
		case 0xfff5: break;  // WCY3_w
		case 0xfff6: break;  // WCY4_w
		case 0xfff8: m_SULA = v; break;  // scu mapping
		case 0xfff9: m_TULA = v; break;  // tcu mapping
		case 0xfffa: m_IULA = v; break;  // icu mapping
		case 0xfffb: m_DULA = v; break;  // dmau mapping
		case 0xfffc: m_OPHA = v; break;  // peripheral mapping (upper bits, common)
		case 0xfffd: OPSEL_w(v); break;  // peripheral enabling
		case 0xfffe: SCTL_w(v); break;   // peripheral configuration (& byte / word mapping)
		default: break; // unmapped (0xff80-0xff81 is read-only)
	}
}

uint8_t V53A::ext_io_read_byte(uint32_t port)
{
	port &= 0xffff;
	if (port >= 0xff00)
		return fixed_reg_read(port);
	return cb.io_read8 ? cb.io_read8(uint16_t(port)) : 0;
}

void V53A::ext_io_write_byte(uint32_t port, uint8_t v)
{
	port &= 0xffff;
	if (port >= 0xff00)
	{
		fixed_reg_write(port, v);
		return;
	}
	if (cb.io_write8)
		cb.io_write8(uint16_t(port), v);
}

uint16_t V53A::ext_io_read_word(uint32_t port)
{
	port &= 0xfffe;
	if (port >= 0xff00)
		return uint16_t(fixed_reg_read(port) | (fixed_reg_read(port + 1) << 8));
	if (cb.io_read16)
		return cb.io_read16(uint16_t(port));
	return uint16_t(ext_io_read_byte(port) | (ext_io_read_byte(port + 1) << 8));
}

void V53A::ext_io_write_word(uint32_t port, uint16_t v)
{
	port &= 0xfffe;
	if (port >= 0xff00)
	{
		fixed_reg_write(port, uint8_t(v));
		fixed_reg_write(port + 1, uint8_t(v >> 8));
		return;
	}
	if (cb.io_write16)
	{
		cb.io_write16(uint16_t(port), v);
		return;
	}
	ext_io_write_byte(port, uint8_t(v));
	ext_io_write_byte(port + 1, uint8_t(v >> 8));
}

// v53_device::install_peripheral_io(), evaluated at access time.
// Returns the device (0 = DMAU, 1 = ICU, 2 = TCU, 3 = SCU), -1 for the
// default handler (temp_io_byte_r/w: forwarded to the external I/O space at
// OPHA:offset), -2 for an unmapped location (read 0, write ignored).
// Later installs override earlier ones, so check in reverse install order.
int V53A::internal_decode(uint8_t a, uint32_t &offset) const
{
	// IOAG determines if the handlers used 8-bit or 16-bit access
	// the hng64.cpp games first set everything up in 8-bit mode, then
	// do the procedure again in 16-bit mode before using them?!
	bool const IOAG = m_SCTL & 1;

	auto try_dev = [&](uint8_t base, unsigned span8, unsigned span16, int dev) -> int
	{
		// span8: range mask in 8-bit mode (all lanes, byte offsets)
		// span16: range mask in 16-bit mode (one lane: base & 1, word offsets)
		if (IOAG)
		{
			const unsigned start = base & ~span8;
			if (a >= start && a <= (base | span8))
			{
				offset = a - start;
				return dev;
			}
		}
		else
		{
			const unsigned start = base & ~span16;
			if (a >= start && a <= (base | span16))
			{
				if ((a & 1) != (base & 1))
					return -2;
				offset = (a - start) >> 1;
				return dev;
			}
		}
		return -3; // not in range
	};

	int r;
	if (m_OPSEL & OPSEL_SS)
	{
		r = try_dev(m_SULA, 0x03, 0x07, 3);
		if (r != -3) return r;
	}
	if (m_OPSEL & OPSEL_TS)
	{
		r = try_dev(m_TULA, 0x03, 0x07, 2);
		if (r != -3) return r;
	}
	if (m_OPSEL & OPSEL_IS)
	{
		r = try_dev(m_IULA, 0x01, 0x03, 1);
		if (r != -3) return r;
	}
	if (m_OPSEL & OPSEL_DS)
	{
		const uint8_t base = m_DULA;
		if (m_SCTL & 0x02) // uPD71037 mode: range is unmapped
		{
			const unsigned span = IOAG ? 0x0f : 0x1f;
			if (a >= (base & ~span) && a <= (base | span))
				return -2;
		}
		else // uPD71071 mode, mask 0xffff: both lanes, byte offsets
		{
			const unsigned start = base & ~0x0f;
			if (a >= start && a <= (base | 0x0f))
			{
				offset = a - start;
				return 0;
			}
		}
	}
	return -1;
}

uint8_t V53A::internal_io_read_byte(uint32_t a)
{
	uint32_t offset = 0;
	switch (internal_decode(uint8_t(a), offset))
	{
		case 0: return m_dmau.read(offset);
		case 1: return m_icu.read(offset);
		case 2: return m_tcu.read(offset);
		case 3:
		{
			brc_sync();
			const uint8_t v = m_scu.read(offset);
			brc_resched();
			return v;
		}
		case -1: return nec_io_read_byte(OPHA() | (a & 0xff)); // temp_io_byte_r
		default: return 0;
	}
}

void V53A::internal_io_write_byte(uint32_t a, uint8_t v)
{
	uint32_t offset = 0;
	switch (internal_decode(uint8_t(a), offset))
	{
		case 0: m_dmau.write(offset, v); break;
		case 1: m_icu.write(offset, v); break;
		case 2: m_tcu.write(offset, v); break;
		case 3: brc_sync(); m_scu.write(offset, v); brc_resched(); break;
		case -1: nec_io_write_byte(OPHA() | (a & 0xff), v); break; // temp_io_byte_w
		default: break;
	}
}

uint16_t V53A::internal_io_read_word(uint32_t a)
{
	// m_internal_io->read_word_unaligned(a & 0xff): low byte lane first
	const uint8_t lo = internal_io_read_byte(a & 0xff);
	const uint8_t hi = internal_io_read_byte((a + 1) & 0xff);
	return uint16_t(lo | (hi << 8));
}

void V53A::internal_io_write_word(uint32_t a, uint16_t v)
{
	internal_io_write_byte(a & 0xff, uint8_t(v));
	internal_io_write_byte((a + 1) & 0xff, uint8_t(v >> 8));
}

uint8_t V53A::io_read_byte(uint32_t a)
{
	if (check_OPHA(a))
		return internal_io_read_byte(a);
	else
		return nec_io_read_byte(a);
}

uint16_t V53A::io_read_word(uint32_t a)
{
	if (check_OPHA(a))
	{
		if ((a & 0xff) == 0xff)
		{
			return (internal_io_read_byte(a) & 0x00ff)
				| ((nec_io_read_byte(a + 1) << 8) & 0xff00);
		}
		else
			return internal_io_read_word(a);
	}
	else
		return nec_io_read_word(a);
}

void V53A::io_write_byte(uint32_t a, uint8_t v)
{
	if (check_OPHA(a))
	{
		internal_io_write_byte(a, v);
	}
	else
		nec_io_write_byte(a, v);
}

void V53A::io_write_word(uint32_t a, uint16_t v)
{
	if (check_OPHA(a))
	{
		if ((a & 0xff) == 0xff)
		{
			internal_io_write_byte(a, v & 0xff);
			nec_io_write_byte(a + 1, (v >> 8) & 0xff);
		}
		else
		{
			internal_io_write_word(a, v);
		}
	}
	else
		nec_io_write_word(a, v);
}

} // namespace v53
