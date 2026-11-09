// license:BSD-3-Clause
// copyright-holders:Wilbert Pol, Nathan Woods
/***************************************************************************

    Intel 8253/8254 Programmable Interval Timer (V53A TCU)

    Standalone port of MAME src/devices/machine/pit8253.{h,cpp} (8254
    variant only, as used by the V53A TCU).

    Differences from MAME:
      - time is measured in V53A clock cycles (uint64_t) instead of attotime;
        the input clock of each counter is given as an integer period in CPU
        clocks (0 = no clock, i.e. MAME's clockin == 0)
      - the scheduler synchronize() deferral of control/count/gate writes is
        replaced by an immediate call at the current CPU time
      - the per-counter emu_timer is replaced by m_next_update, which the
        owning V53A polls (next_event()/process())

***************************************************************************/

#ifndef BURIKI_AUDIOCPU_V53A_PIT8254_H
#define BURIKI_AUDIOCPU_V53A_PIT8254_H

#pragma once

#include <cstdint>

namespace v53 {

class V53A;

class Pit8254
{
public:
	static constexpr uint64_t NEVER = ~uint64_t(0);

	explicit Pit8254(V53A &host);

	void start();   // device_start (power on)
	void reset();   // device_reset (all three counters)

	uint8_t read(uint32_t offset);
	void write(uint32_t offset, uint8_t data);

	void write_gate(int n, int state) { m_counter[n].gate_w(state); }
	void write_clk(int n, int state) { m_counter[n].set_clock_signal(state); }

	// period in CPU clocks of the counter's input clock, 0 = no clock
	void set_clock_period(int n, uint64_t period) { m_counter[n].set_clockin(period); }

	// earliest pending counter update (NEVER if none)
	uint64_t next_event() const;
	// run the update of every counter whose timer expired at or before 'until'
	// (the host sets now() to the event time before each call)
	int next_counter(uint64_t &when) const;
	void fire(int n) { m_counter[n].update(); }

	// debug
	uint16_t counter_value(int n) const { return m_counter[n].m_value; }
	int counter_output(int n) const { return m_counter[n].m_output_pin; }

private:
	struct Counter
	{
		Pit8254 *m_owner = nullptr;
		int m_index = 0;                 // index number of the timer
		uint64_t m_clock_period = 0;     // input clock period in CPU clocks (0 = clockin is 0)
		int m_clock_signal = 0;          // clock signal when clockin is 0
		bool m_started = false;

		uint64_t m_last_updated = 0;     // time when last updated
		uint64_t m_next_update = NEVER;  // time of next update

		uint16_t m_value = 0;            // current counter value ("CE" in Intel docs)
		uint16_t m_latch = 0;            // latched counter value ("OL" in Intel docs)
		uint16_t m_count = 0;            // new counter value ("CR" in Intel docs)
		uint8_t m_control = 0;           // 6-bit control byte
		uint8_t m_status = 0;            // status byte - 8254 only
		uint8_t m_lowcount = 0;          // LSB of new counter value for 16-bit writes
		bool m_rmsb = false;             // true = Next read is MSB of 16-bit value
		bool m_wmsb = false;             // true = Next write is MSB of 16-bit value
		int m_output = 0;                // output value based on counter state (0 = low, 1 = high)
		int m_output_pin = 0;            // state of the output pin (0 = low, 1 = high)

		int m_gate = 1;                  // sampled gate (0 = low, 1 = high)
		int m_gate_input = 1;            // state of the gate input pin (0 = low, 1 = high)
		int m_gate_rose = 0;             // gate input had a low-to-high transition (0 = no, 1 = yes)
		int m_latched_count = 0;         // number of bytes of count latched
		int m_latched_status = 0;        // 1 = status latched (8254 only)
		int m_null_count = 1;            // 1 = mode control or count written, 0 = count loaded
		int m_phase = 0;                 // see phase definition tables in simulate()

		uint64_t now() const;
		void device_start();
		void device_reset();

		uint32_t adjusted_count() const;
		void adjust_timer(uint64_t target);
		void decrease_counter_value(int64_t cycles);
		void load_counter_value();
		void set_output(int output);
		void flush_output();
		void simulate(int64_t elapsed_cycles);
		void update();
		uint16_t masked_value() const;
		uint8_t read();
		void load_count(uint16_t newcount);
		void readback(int command);
		bool edge_sensitive_gate() const;
		void control_w(uint8_t data);
		void count_w(uint8_t data);
		void gate_w(int state);
		void set_clock_signal(int state);
		void set_clockin(uint64_t period);
	};

	void readback_command(uint8_t data);

	V53A &m_host;
	Counter m_counter[3];

	friend struct Counter;
};

} // namespace v53

#endif // BURIKI_AUDIOCPU_V53A_PIT8254_H
