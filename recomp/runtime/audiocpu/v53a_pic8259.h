// license:BSD-3-Clause
// copyright-holders:Wilbert Pol
/***************************************************************************

    Intel 8259A Programmable Interrupt Controller (V53A ICU)

    Standalone port of MAME src/devices/machine/pic8259.{h,cpp}, v5x_icu_device
    variant (always x86 mode). The emu_timer used to re-evaluate the INT
    output (m_irq_timer->adjust(attotime::zero)) is replaced by a request to
    the owning V53A, which runs irq_timer_tick() after the current
    instruction at the current time.

***************************************************************************/

#ifndef BURIKI_AUDIOCPU_V53A_PIC8259_H
#define BURIKI_AUDIOCPU_V53A_PIC8259_H

#pragma once

#include <cstdint>

namespace v53 {

class V53A;

class Pic8259
{
public:
	explicit Pic8259(V53A &host);

	uint8_t read(uint32_t offset);
	void write(uint32_t offset, uint8_t data);
	uint8_t acknowledge();

	void set_irq_line(int irq, int state);

	void start();
	void reset();

	void irq_timer_tick();

	// debug
	uint8_t irr() const { return m_irr; }
	uint8_t isr() const { return m_isr; }
	uint8_t imr() const { return m_imr; }

private:
	bool is_x86() const { return true; } // v5x_icu_device

	enum class state_t : uint8_t
	{
		ICW1,
		ICW2,
		ICW3,
		ICW4,
		READY
	};

	void irq_timer_adjust_zero();

	V53A &m_host;

	state_t m_state = state_t::READY;

	uint8_t m_isr = 0;
	uint8_t m_irr = 0;
	uint8_t m_prio = 0;
	uint8_t m_imr = 0;
	uint8_t m_irq_lines = 0;

	uint8_t m_input = 0;
	uint8_t m_ocw3 = 0;

	uint8_t m_master = 1;
	/* ICW1 state */
	uint8_t m_level_trig_mode = 0;
	uint8_t m_vector_size = 0;
	uint8_t m_cascade = 0;
	uint8_t m_icw4_needed = 0;
	uint32_t m_vector_addr_low = 0;
	/* ICW2 state */
	uint8_t m_base = 0;
	uint8_t m_vector_addr_high = 0;

	/* ICW3 state */
	uint8_t m_slave = 0;

	/* ICW4 state */
	uint8_t m_nested = 0;
	uint8_t m_mode = 0;
	uint8_t m_auto_eoi = 0;
	uint8_t m_is_x86 = 0;

	int8_t m_current_level = -1;
	uint8_t m_inta_sequence = 0;
};

} // namespace v53

#endif // BURIKI_AUDIOCPU_V53A_PIC8259_H
