// license:BSD-3-Clause
// copyright-holders:Wilbert Pol, Nathan Woods
/*****************************************************************************
 *
 *  Programmable Interval Timer 8253/8254
 *
 *  Three Independent Timers
 *  (gate, clock, out pins)
 *
 *  8254 has an additional readback feature
 *
 *  Revision History
 *      1-Apr-2008 - WFP:   Changed the implementation into a device.
 *      8-Jul-2004 - AJ:    Fixed some bugs. Styx now runs correctly.
 *                          Implemented 8254 features.
 *      1-Mar-2004 - NPW:   Did an almost total rewrite and cleaned out much
 *                          of the ugliness in the previous design.  Bug #430
 *                          seems to be fixed
 *      1-Jul-2000 - PeT:   Split off from PC driver and componentized
 *
 *  Standalone port for the V53A TCU (see v53a_pit8254.h for differences).
 *
 *****************************************************************************/

#include "v53a_pit8254.h"
#include "v53a.h"

namespace v53 {

/***************************************************************************

    Structures & macros

***************************************************************************/

#define CTRL_ACCESS(control)        (((control) >> 4) & 0x03)
#define CTRL_MODE(control)          (((control) >> 1) & (((control) & 0x04) ? 0x03 : 0x07))
#define CTRL_BCD(control)           (((control) >> 0) & 0x01)

Pit8254::Pit8254(V53A &host) : m_host(host)
{
	for (int i = 0; i < 3; i++)
	{
		m_counter[i].m_owner = this;
		m_counter[i].m_index = i;
	}
}

void Pit8254::start()
{
	for (auto &c : m_counter)
		c.device_start();
}

void Pit8254::reset()
{
	for (auto &c : m_counter)
		c.device_reset();
}

uint64_t Pit8254::next_event() const
{
	uint64_t t = NEVER;
	for (auto const &c : m_counter)
		if (c.m_next_update < t)
			t = c.m_next_update;
	return t;
}

int Pit8254::next_counter(uint64_t &when) const
{
	int best = -1;
	uint64_t t = NEVER;
	for (int i = 0; i < 3; i++)
		if (m_counter[i].m_next_update < t)
		{
			t = m_counter[i].m_next_update;
			best = i;
		}
	when = t;
	return best;
}

uint64_t Pit8254::Counter::now() const
{
	return m_owner->m_host.now();
}

//-------------------------------------------------
//  device_start - device-specific startup
//-------------------------------------------------

void Pit8254::Counter::device_start()
{
	/* initialize timers */
	adjust_timer(NEVER);

	/* zerofill */
	m_gate = 1;
	m_gate_input = 1;
	m_gate_rose = 0;
	m_phase = 0;
	m_clock_signal = 0;

	m_control = m_status = 0x30;
	m_rmsb = m_wmsb = false;
	m_count = m_value = m_latch = 0;
	m_lowcount = 0;

	m_output = 0;
	m_output_pin = 0;
	m_latched_count = 0;
	m_latched_status = 0;
	m_null_count = 1;

	m_last_updated = now();
	m_started = true;
}

//-------------------------------------------------
//  device_reset - device-specific reset
//-------------------------------------------------

void Pit8254::Counter::device_reset()
{
	/* According to Intel's 8254 docs, the state of a timer is undefined
	 until the first mode control word is written. Here we define this
	 undefined behaviour */
	m_control = m_status = 0x30;
	m_rmsb = m_wmsb = false;
	m_count = m_value = m_latch = 0;
	m_lowcount = 0;

	m_output = 2; /* output is undetermined */
	m_output_pin = 2;
	m_latched_count = 0;
	m_latched_status = 0;
	m_null_count = 1;

	m_last_updated = now();

	update();
}


/***************************************************************************

    Functions

***************************************************************************/

void Pit8254::Counter::adjust_timer(uint64_t target)
{
	m_next_update = target;
	if (target != NEVER)
		m_owner->m_host.schedule_event(target);
}

uint32_t Pit8254::Counter::adjusted_count() const
{
	uint16_t val = m_value;

	if (!CTRL_BCD(m_control))
		return (val == 0) ? 0x10000 : val;
	else if (val == 0)
		return 10000;

	/* In BCD mode, a nybble loaded with value A-F counts down the same as in
	   binary mode, but wraps around to 9 instead of F after 0, so loading the
	   count register with 0xFFFF gives a period of
	          0xF  - for the units to count down to 0
	   +   10*0xF  - for the tens to count down to 0
	   +  100*0xF  - for the hundreds to count down to 0
	   + 1000*0xF  - for the thousands to count down to 0
	   = 16665 cycles
	*/
	return
		((val>>12) & 0xF) *  1000 +
		((val>> 8) & 0xF) *   100 +
		((val>> 4) & 0xF) *    10 +
		( val      & 0xF);
}


/* This function subtracts 1 from m_value "cycles" times, taking into
   account binary or BCD operation, and wrapping around from 0 to 0xFFFF or
   0x9999 as necessary. */
void Pit8254::Counter::decrease_counter_value(int64_t cycles)
{
	if (CTRL_BCD(m_control) == 0)
	{
		m_value -= (cycles & 0xffff);
		return;
	}

	uint8_t units     =  m_value        & 0xf;
	uint8_t tens      = (m_value >>  4) & 0xf;
	uint8_t hundreds  = (m_value >>  8) & 0xf;
	uint8_t thousands = (m_value >> 12) & 0xf;

	if (cycles <= units)
	{
		units -= cycles;
	}
	else
	{
		cycles -= units;
		units = (10 - cycles % 10) % 10;

		cycles = (cycles + 9) / 10; /* the +9 is so we get a carry if cycles%10 wasn't 0 */
		if (cycles <= tens)
		{
			tens -= cycles;
		}
		else
		{
			cycles -= tens;
			tens = (10 - cycles % 10) % 10;

			cycles = (cycles + 9) / 10;
			if (cycles <= hundreds)
			{
				hundreds -= cycles;
			}
			else
			{
				cycles -= hundreds;
				hundreds = (10 - cycles % 10) % 10;
				cycles = (cycles + 9) / 10;
				thousands = (10 + thousands - cycles % 10) % 10;
			}
		}
	}

	m_value = (thousands << 12) | (hundreds << 8) | (tens << 4) | units;
}


/* Counter loading: transfer of a count from the CR to the CE */
void Pit8254::Counter::load_counter_value()
{
	m_value = m_count;
	m_null_count = 0;
}


void Pit8254::Counter::set_output(int output)
{
	if (output != m_output)
	{
		m_output = output;
		flush_output();
	}
}

void Pit8254::Counter::flush_output()
{
	int new_output = m_output;
	if (m_clock_period == 0)
	{
		// TODO: The same logic should apply when m_clockin != 0, but that needs
		// to be verified.
		const int mode = CTRL_MODE(m_control);
		if ((mode == 2 || mode == 3) && m_gate_input == 0)
			new_output = 1;
	}

	if (new_output != m_output_pin)
	{
		m_output_pin = new_output;
		m_owner->m_host.tcu_out(m_index, new_output);
	}
}

/* This emulates timer "timer" for "elapsed_cycles" cycles and assumes no
   callbacks occur during that time. */
void Pit8254::Counter::simulate(int64_t elapsed_cycles)
{
	uint32_t adjusted_value;
	int mode = CTRL_MODE(m_control);
	static const uint32_t CYCLES_NEVER = (0xffffffff);
	uint32_t cycles_to_output = 0;

	switch (mode)
	{
	case 0:
		/* Mode 0: (Interrupt on Terminal Count)

		          +------------------
		          |
		----------+
		  <- n+1 ->

		  ^
		  +- counter load

		phase|output|length  |value|next|comment
		-----+------+--------+-----+----+----------------------------------
		    0|low   |infinity|     |1   |waiting for count
		    1|low   |1       |     |2   |internal delay when counter loaded
		    2|low   |n       |n..1 |3   |counting down
		    3|high  |infinity|0..1 |3   |counting down

		Gate level sensitive only. Low disables counting, high enables it. */

		if (m_phase == 0)
		{
			cycles_to_output = CYCLES_NEVER;
		}
		else
		{
			if (elapsed_cycles >= 0 && m_phase == 1)
			{
				/* Counter load cycle */
				if (elapsed_cycles > 0)
				{
					--elapsed_cycles;
					m_phase = 2;
				}
				load_counter_value();
			}

			if (m_gate == 0)
			{
				cycles_to_output = CYCLES_NEVER;
			}
			else
			{
				if (m_phase == 2)
				{
					adjusted_value = adjusted_count();
					if (elapsed_cycles >= adjusted_value)
					{
						/* Counter wrapped, output goes high */
						elapsed_cycles -= adjusted_value;
						m_phase = 3;
						m_value = 0;
						set_output(1);
					}
				}

				decrease_counter_value(elapsed_cycles);

				switch (m_phase)
				{
				case 1:  cycles_to_output = 1; break;
				case 2:  cycles_to_output = adjusted_count(); break;
				case 3:  cycles_to_output = adjusted_count(); break;
				}
			}
		}
		break;


	case 1:
		/* Mode 1: (Hardware Retriggerable One-Shot a.k.a. Programmable One-Shot)

		-----+       +------------------
		     |       |
		     +-------+
		     <-  n  ->

		  ^
		  +- trigger

		phase|output|length  |value|next|comment
		-----+------+--------+-----+----+----------------------------------
		    0|high  |infinity|     |1   |counting down
		    1|high  |1       |     |2   |internal delay to load counter
		    2|low   |n       |n..1 |3   |counting down
		    3|high  |infinity|0..1 |3   |counting down

		Gate rising-edge sensitive only.
		Rising edge initiates counting and resets output after next clock. */

		if (elapsed_cycles >= 0 && m_phase == 1)
		{
			/* Counter load cycle, output goes low */
			if (elapsed_cycles > 0)
			{
				--elapsed_cycles;
				m_phase = 2;
			}
			load_counter_value();
			set_output(0);
		}

		if (m_phase == 2)
		{
			adjusted_value = adjusted_count();
			if (elapsed_cycles >= adjusted_value)
			{
				/* Counter wrapped, output goes high */
				m_phase = 3;
				set_output(1);
			}
		}

		decrease_counter_value(elapsed_cycles);

		switch (m_phase)
		{
		case 1:   cycles_to_output = 1; break;
		case 2:   cycles_to_output = adjusted_count(); break;
		default:  cycles_to_output = CYCLES_NEVER; break;
		}
		break;


	case 2:
		/* Mode 2: (Rate Generator)

		--------------+ +---------+ +----
		              | |         | |
		              +-+         +-+
		   <-    n    -X-    n    ->
		              <1>
		^
		+- counter load or trigger

		phase|output|length  |value|next|comment
		-----+------+--------+-----+----+----------------------------------
		    0|high  |infinity|     |1   |waiting for count
		    1|high  |1       |     |2   |internal delay to load counter
		    2|high  |n       |n..2 |3   |counting down
		    3|low   |1       |1    |2   |reload counter

		Counter rewrite has no effect until repeated

		Gate rising-edge and level sensitive.
		Gate low disables counting and sets output immediately high.
		Rising-edge reloads count and initiates counting
		Gate high enables counting. */

		if (m_gate == 0 || m_phase == 0)
		{
			/* Gate low or mode control write forces output high */
			set_output(1);
			cycles_to_output = CYCLES_NEVER;
		}
		else
		{
			if (elapsed_cycles >= 0 && m_phase == 1)
			{
				if (elapsed_cycles > 0)
				{
					--elapsed_cycles;
					m_phase = 2;
				}
				load_counter_value();
			}

			adjusted_value = adjusted_count();

			do
			{
				if (m_phase == 2)
				{
					if (elapsed_cycles + 1 >= adjusted_value)
					{
						/* Counter hits 1, output goes low */
						m_phase = 3;
						set_output(0);
					}
				}

				if (elapsed_cycles >= adjusted_value && m_phase == 3)
				{
					/* Reload counter, output goes high */
					elapsed_cycles -= adjusted_value;
					m_phase = 2;
					load_counter_value();
					adjusted_value = adjusted_count();
					set_output(1);
				}
			}
			while (elapsed_cycles >= adjusted_value);

			/* Calculate counter value */
			decrease_counter_value(elapsed_cycles);

			switch (m_phase)
			{
			case 1:   cycles_to_output = 1; break;
			default:  cycles_to_output = (m_value == 1) ? 1 : (adjusted_count() - 1); break;
			}
		}
		break;


	case 3:
		/* Mode 3: (Square Wave Generator)

		----------------+           +-----------+           +----
		                |           |           |           |
		                +-----------+           +-----------+
		    <- (n+1)/2 -X-   n/2   ->
		 ^
		 +- counter load or trigger

		phase|output|length  |value|next|comment
		-----+------+--------+-----+----+----------------------------------
		    0|high  |infinity|     |1   |waiting for count
		    1|high  |1       |     |2   |internal delay to load counter
		    2|high  |n/2(+1) |n..0 |3   |counting down double speed, reload counter
		    3|low   |n/2     |n..0 |2   |counting down double speed, reload counter

		Counter rewrite has no effect until repeated (output falling or rising)

		Gate rising-edge and level sensitive.
		Gate low disables counting and sets output immediately high.
		Rising-edge reloads count and initiates counting
		Gate high enables counting. */

		if (m_gate == 0 || m_phase == 0)
		{
			/* Gate low or mode control write forces output high */
			set_output(1);
			cycles_to_output = CYCLES_NEVER;
		}
		else
		{
			if (elapsed_cycles >= 0 && m_phase == 1)
			{
				if (elapsed_cycles > 0)
				{
					--elapsed_cycles;
					m_phase = 2;
				}
				load_counter_value();
			}

			if (elapsed_cycles > 0)
			{
				adjusted_value = adjusted_count();

				do
				{
					if (m_phase == 2 && elapsed_cycles >= ((adjusted_value + 1) >> 1))
					{
						/* High phase expired, output goes low */
						elapsed_cycles -= ((adjusted_value + 1) >> 1);
						m_phase = 3;
						load_counter_value();
						adjusted_value = adjusted_count();
						set_output(0);
					}

					if (m_phase == 3 && elapsed_cycles >= (adjusted_value >> 1))
					{
						/* Low phase expired, output goes high */
						elapsed_cycles -= (adjusted_value >> 1);
						m_phase = 2;
						load_counter_value();
						adjusted_value = adjusted_count();
						set_output(1);
					}
				}
				while ((m_phase == 2 && elapsed_cycles >= ((adjusted_value + 1) >> 1)) ||
						(m_phase == 3 && elapsed_cycles >= (adjusted_value >> 1)));

				decrease_counter_value(elapsed_cycles * 2);

				switch (m_phase)
				{
				case 1:  cycles_to_output = 1; break;
				case 2:  cycles_to_output = (adjusted_count() + 1) >> 1; break;
				case 3:  cycles_to_output = adjusted_count() >> 1; break;
				}
			}
		}
		break;


	case 4:
	case 5:
		/* Mode 4: (Software Trigger Strobe)
		   Mode 5: (Hardware Trigger Strobe)

		--------------+ +--------------------
		              | |
		              +-+
		    <-  n+1  ->
		    ^         <1>
		    +- counter load (mode 4) or trigger (mode 5)

		phase|output|length  |value|next|comment
		-----+------+--------+-----+----+----------------------------------
		    0|high  |infinity|0..1 |0   |waiting for count/counting down
		    1|high  |1       |     |2   |internal delay when counter loaded
		    2|high  |n       |n..1 |3   |counting down
		    3|low   |1       |0    |0   |strobe

		Mode 4 only: counter rewrite loads new counter
		Mode 5 only: count not reloaded immediately.
		Mode control write doesn't stop count but sets output high

		Mode 4 only: Gate level sensitive only. Low disables counting, high enables it.
		Mode 5 only: Gate rising-edge sensitive only. Rising edge initiates counting */

		if (m_gate == 0 && mode == 4)
		{
			cycles_to_output = CYCLES_NEVER;
		}
		else
		{
			if (elapsed_cycles >= 0 && m_phase == 1)
			{
				if (elapsed_cycles > 0)
				{
					--elapsed_cycles;
					m_phase = 2;
				}
				load_counter_value();
			}

			if (m_value == 0 && m_phase == 2)
				adjusted_value = 0;
			else
				adjusted_value = adjusted_count();

			if (m_phase == 2 && elapsed_cycles >= adjusted_value)
			{
				/* Counter has hit zero, set output to low */
				elapsed_cycles -= adjusted_value;
				m_phase = 3;
				m_value = 0;
				set_output(0);
			}

			if (elapsed_cycles > 0 && m_phase == 3)
			{
				--elapsed_cycles;
				m_phase = 0;
				decrease_counter_value(1);
				set_output(1);
			}

			decrease_counter_value(elapsed_cycles);

			switch (m_phase)
			{
			case 1:  cycles_to_output = 1; break;
			case 2:  cycles_to_output = adjusted_count(); break;
			case 3:  cycles_to_output = 1; break;
			}
		}
		break;
	}

	if (cycles_to_output == CYCLES_NEVER || m_clock_period == 0)
		adjust_timer(NEVER);
	else
		adjust_timer(m_last_updated + uint64_t(cycles_to_output) * m_clock_period);
}

/* This brings timer "timer" up to date */
void Pit8254::Counter::update()
{
	const uint64_t t = now();
	int64_t elapsed_cycles = 0;
	if (m_clock_period != 0)
	{
		if (t > m_last_updated)
		{
			elapsed_cycles = int64_t((t - m_last_updated) / m_clock_period);
			m_last_updated += uint64_t(elapsed_cycles) * m_clock_period;
		}
	}
	else
		m_last_updated = t;

	/* This emulates timer "timer" for "elapsed_cycles" cycles, broken down into
	   sections punctuated by callbacks. */
	if (elapsed_cycles > 0)
		simulate(elapsed_cycles);
	else if (m_clock_period != 0)
		adjust_timer(m_last_updated + m_clock_period);
	else
		m_next_update = NEVER; // MAME: the timer stays where it was; with no clock nothing is pending
}


/* Since read commands in mode 3 always return even numbers,
   we need to mask bit 0 off. */
uint16_t Pit8254::Counter::masked_value() const
{
	if (CTRL_MODE(m_control) == 3)
		return m_value & 0xfffe;
	return m_value;
}

/* Reads only affect the following bits of the counter state:
     latched_status
     latched_count
     rmsb
  so they don't affect any timer operations except other reads. */
uint8_t Pit8254::Counter::read()
{
	uint8_t data;

	update();

	if (m_latched_status)
	{
		/* Read status register (8254 only) */
		data = m_status;
		m_latched_status = 0;
	}
	else
	{
		if (m_latched_count != 0)
		{
			/* Read back latched count */
			data = (m_latch >> (m_rmsb ? 8 : 0)) & 0xff;
			m_rmsb = !m_rmsb;
			--m_latched_count;
		}
		else
		{
			uint16_t value = masked_value();

			/* Read back current count */
			switch (CTRL_ACCESS(m_control))
			{
			case 0:
			default:
				/* This should never happen */
				data = 0; /* Appease compiler */
				break;

			case 1:
				/* read counter bits 0-7 only */
				data = (value >> 0) & 0xff;
				break;

			case 2:
				/* read counter bits 8-15 only */
				data = (value >> 8) & 0xff;
				break;

			case 3:
				/* read bits 0-7 first, then 8-15 */

				// reading back the current count while in the middle of a
				// 16-bit write returns a xor'ed version of the value written
				// (apricot diagnostic timer test tests this)
				if (m_wmsb)
					data = ~m_lowcount;
				else
					data = value >> (m_rmsb ? 8 : 0);

				m_rmsb = !m_rmsb;
				break;
			}
		}
	}

	return data;
}

uint8_t Pit8254::read(uint32_t offset)
{
	offset &= 3;

	if (offset == 3)
	{
		/* Reading mode control register is illegal according to docs */
		/* Experimentally determined: reading it returns 0 */
		return 0;
	}
	else
		return m_counter[offset].read();
}


/* Loads a new value from the bus to the count register (CR) */
void Pit8254::Counter::load_count(uint16_t newcount)
{
	int mode = CTRL_MODE(m_control);

	if (newcount == 1)
	{
		/* Count of 1 is illegal in modes 2 and 3. What happens here was
		   determined experimentally. */
		if (mode == 2)
			newcount = 2;
		if (mode == 3)
			newcount = 0;
	}

	m_count = newcount;

	if (mode == 2 || mode == 3)
	{
		if (m_phase == 0)
			m_phase = 1;
	}
	else
	{
		if (mode == 0 || mode == 4)
			m_phase = 1;
	}
}


void Pit8254::Counter::readback(int command)
{
	update();

	if ((command & 1) == 0)
	{
		/* readback status command */
		if (!m_latched_status)
		{
			m_status = (m_control & 0x3f) | ((m_output != 0) ? 0x80 : 0) | (m_null_count ? 0x40 : 0);
			m_latched_status = 1;
		}
	}

	/* Experimentally determined: the read latch command seems to have no
	   effect if we're halfway through a 16-bit read */
	if ((command & 2) == 0 && !m_rmsb)
	{
		/* readback count command */
		if (m_latched_count == 0)
		{
			uint16_t value = masked_value();
			switch (CTRL_ACCESS(m_control))
			{
			case 0:
				/* This should never happen */
				break;

			case 1:
				/* latch bits 0-7 only */
				m_latch = ((value << 8) & 0xff00) | (value & 0xff);
				m_latched_count = 1;
				break;

			case 2:
				/* read bits 8-15 only */
				m_latch = (value & 0xff00) | ((value >> 8) & 0xff);
				m_latched_count = 1;
				break;

			case 3:
				/* latch all 16 bits */
				m_latch = value;
				m_latched_count = 2;
				break;
			}
		}
	}
}


void Pit8254::readback_command(uint8_t data)
{
	/* Bit 0 of data must be 0. Todo: find out what the hardware does if it isn't. */
	int read_command = (data >> 4) & 3;
	for (int timer = 0; timer < 3; timer++)
		if (((data >> (timer + 1)) & 1) != 0)
			m_counter[timer].readback(read_command);
}

void Pit8254::Counter::control_w(uint8_t data)
{
	update();
	if (CTRL_ACCESS(data) == 0)
	{
		/* Latch current timer value */
		/* Experimentally verified: this command does not affect the mode control register */
		readback(1);
	}
	else
	{
		m_control = (data & 0x3f);
		m_null_count = 1;
		m_wmsb = m_rmsb = false;
		/* Phase 0 is always the phase after a mode control write */
		m_phase = 0;
		set_output(CTRL_MODE(m_control) ? 1 : 0);
	}
}

void Pit8254::Counter::count_w(uint8_t data)
{
	update();
	bool middle_of_a_cycle = (now() > m_last_updated && m_clock_period != 0);

	switch (CTRL_ACCESS(m_control))
	{
	case 0:
		/* This should never happen */
		break;

	case 1:
		/* read/write counter bits 0-7 only */

		/* check if we should compensate for not being on a cycle boundary */
		if (middle_of_a_cycle)
			m_last_updated += m_clock_period;

		load_count(data);
		if (m_clock_period != 0)
			simulate(0);

		if (CTRL_MODE(m_control) == 0)
			set_output(0);
		break;

	case 2:
		/* read/write counter bits 8-15 only */

		/* check if we should compensate for not being on a cycle boundary */
		if (middle_of_a_cycle)
			m_last_updated += m_clock_period;

		load_count(data << 8);
		if (m_clock_period != 0)
			simulate(0);

		if (CTRL_MODE(m_control) == 0)
			set_output(0);
		break;

	case 3:
		/* read/write bits 0-7 first, then 8-15 */
		if (m_wmsb)
		{
			/* check if we should compensate for not being on a cycle boundary */
			if (middle_of_a_cycle)
				m_last_updated += m_clock_period;

			load_count(m_lowcount | (data << 8));
			if (m_clock_period != 0)
				simulate(0);
		}
		else
		{
			m_lowcount = data;
			if (CTRL_MODE(m_control) == 0)
			{
				/* The Intel docs say that writing the MSB in mode 0, phase
				   2 won't stop the count, but this was experimentally
				   determined to be false. */
				m_phase = 0;
				set_output(0);
			}
		}
		m_wmsb = !m_wmsb;
		break;
	}
}

void Pit8254::write(uint32_t offset, uint8_t data)
{
	offset &= 3;

	if (offset == 3)
	{
		/* Write to mode control register */
		int timer = (data >> 6) & 3;
		if (timer == 3)
			readback_command(data);
		else
			m_counter[timer].control_w(data);
	}
	else
		m_counter[offset].count_w(data);
}

bool Pit8254::Counter::edge_sensitive_gate() const
{
	const int mode = CTRL_MODE(m_control);
	// TODO: The m_clockin check should not be necessary, but removing it needs
	// to be verified.
	return mode == 1 || mode == 2 || (mode == 3 && m_clock_period == 0) || mode == 5;
}

void Pit8254::Counter::gate_w(int state)
{
	update();

	if (state != m_gate_input)
	{
		update();
		m_gate_input = state;
		if (m_clock_period != 0)
			m_gate = state;
		if (state != 0 && edge_sensitive_gate())
		{
			if (m_clock_period != 0)
				m_phase = 1;
			else
				m_gate_rose = 1;
		}
		if (m_clock_period == 0)
			flush_output();
		update();
	}
}


/* ----------------------------------------------------------------------- */

void Pit8254::Counter::set_clockin(uint64_t period)
{
	if (m_started)
		update();
	m_clock_period = period;
	if (m_started)
		update();
}


void Pit8254::Counter::set_clock_signal(int state)
{
	/* Sample GATE and the GATE edge detection flipflop on low to high transition */
	if (!m_clock_signal && state)
	{
		m_gate = m_gate_input;
		if (m_gate_rose)
		{
			if (edge_sensitive_gate())
				m_phase = 1;
			m_gate_rose = 0;
		}
	}

	/* Trigger on high to low transition */
	if (m_clock_signal && !state)
	{
		/* Advance a cycle */
		simulate(1);
	}
	m_clock_signal = state;
}

} // namespace v53
