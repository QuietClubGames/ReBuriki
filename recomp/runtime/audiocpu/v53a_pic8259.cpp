// license:BSD-3-Clause
// copyright-holders:Wilbert Pol
/**********************************************************************

    8259 PIC interface and emulation

    The 8259 is a programmable interrupt controller used to multiplex
    interrupts for x86 and other computers.  The chip is set up by
    writing a series of Initialization Command Words (ICWs) after which
    the chip is operational and capable of dispatching interrupts.  After
    this, Operation Command Words (OCWs) can be written to control further
    behavior.

    Standalone port for the V53A ICU (see v53a_pic8259.h).

**********************************************************************/

#include "v53a_pic8259.h"
#include "v53a.h"

namespace v53 {

Pic8259::Pic8259(V53A &host) : m_host(host)
{
}

void Pic8259::irq_timer_adjust_zero()
{
	m_host.icu_request_eval();
}

void Pic8259::irq_timer_tick()
{
	/* check the various IRQs */
	for (int n = 0, irq = m_prio; n < 8; n++, irq = (irq + 1) & 7)
	{
		uint8_t mask = 1 << irq;

		/* is this IRQ in service and not cascading and sfnm? */
		if ((m_isr & mask) && !(m_master && m_cascade && m_nested && (m_slave & mask)))
		{
			break;
		}

		/* is this IRQ pending and enabled? */
		if ((m_state == state_t::READY) && (m_irr & mask) && !(m_imr & mask))
		{
			m_current_level = irq;
			m_host.icu_int_w(1);
			return;
		}
		// if sfnm and in-service don't continue
		if((m_isr & mask) && m_master && m_cascade && m_nested && (m_slave & mask))
			break;
	}
	m_current_level = -1;
	m_host.icu_int_w(0);
}

void Pic8259::set_irq_line(int irq, int state)
{
	uint8_t mask = (1 << irq);

	if (state && !(m_irq_lines & mask))
	{
		/* setting IRQ line */
		m_irr |= mask;
		m_irq_lines |= mask;
	}
	else if (!state && (m_irq_lines & mask))
	{
		/* clearing IRQ line */
		m_irq_lines &= ~mask;
		m_irr &= ~mask;
	}

	if (m_inta_sequence == 0)
		irq_timer_adjust_zero();
}


uint8_t Pic8259::acknowledge()
{
	if (is_x86())
	{
		/* is this IRQ pending and enabled? */
		if (m_current_level != -1)
		{
			uint8_t mask = 1 << m_current_level;
			if (!m_level_trig_mode && (!m_master || !(m_slave & mask)))
				m_irr &= ~mask;

			if (!m_auto_eoi)
				m_isr |= mask;

			irq_timer_adjust_zero();

			if ((m_cascade!=0) && (m_master!=0) && (mask & m_slave))
			{
				// it's from slave device (V53: read_slave_ack -> get_pic_ack() returns 0)
				return 0;
			}
			else
			{
				/* For x86 mode*/
				return m_current_level + m_base;
			}
		}
		else
		{
			m_host.logerror("Spurious INTA\n");
			return m_base + 7;
		}
	}
	return 0; // 8080 mode never used by the V5x ICU
}


uint8_t Pic8259::read(uint32_t offset)
{
	/* NPW 18-May-2003 - Changing 0xFF to 0x00 as per Ruslan */
	uint8_t data = 0x00;

	switch(offset)
	{
		case 0: /* PIC acknowledge IRQ */
			if ( m_ocw3 & 0x04 )
			{
				/* Polling mode */
				if (m_current_level != -1)
				{
					data = 0x80 | m_current_level;

					if (!m_level_trig_mode && (!m_master || !((m_slave >> m_current_level) & 1)))
						m_irr &= ~(1 << m_current_level);

					if (!m_auto_eoi)
						m_isr |= 1 << m_current_level;

					irq_timer_adjust_zero();
				}

				m_ocw3 &= 0xfb;
			}
			else
			{
				if ( m_ocw3 & 0x01 )
					data = m_isr & ~m_imr;
				else
					data = m_irr;
			}
			break;

		case 1: /* PIC mask register */
			data = m_imr;
			break;
	}
	return data;
}


void Pic8259::write(uint32_t offset, uint8_t data)
{
	switch(offset)
	{
		case 0:    /* PIC acknowledge IRQ */
			if (data & 0x10)
			{
				/* write ICW1 - this pretty much resets the chip */
				m_imr                = 0x00;
				m_isr                = 0x00;
				m_slave              = 0x00;
				m_level_trig_mode    = (data & 0x08) ? 1 : 0;
				m_vector_size        = (data & 0x04) ? 1 : 0;
				m_cascade            = (data & 0x02) ? 0 : 1;
				m_icw4_needed        = (data & 0x01) ? 1 : 0;
				m_vector_addr_low    = (data & 0xe0);
				m_state              = state_t::ICW2;
				m_current_level      = -1;
				m_inta_sequence      = 0;
				m_irr                = m_level_trig_mode ? m_irq_lines : 0;
				m_host.icu_int_w(0);
			}
			else if (m_state == state_t::READY)
			{
				if ((data & 0x98) == 0x08)
				{
					/* write OCW3 */
					if ((data >> 1) & 1)
						m_ocw3 = (m_ocw3 & 0xfe) | (data & 0x01);
					if ((data >> 2) & 1)
						m_ocw3 |= 0x04;
					// TODO: special mask mode
					if ((data >> 6) & 1)
						m_ocw3 = (m_ocw3 & 0xdf) | (data & 0x20);
				}
				else if ((data & 0x18) == 0x00)
				{
					int n = data & 7;
					uint8_t mask = 1 << n;

					/* write OCW2 */
					switch (data & 0xe0)
					{
						case 0x00:
							m_prio = 0;
							break;
						case 0x20:
							for (n = 0, mask = 1<<m_prio; n < 8; n++, mask = (mask<<1) | (mask>>7))
							{
								if (m_isr & mask)
								{
									m_isr &= ~mask;
									break;
								}
							}
							break;
						case 0x40:
							break;
						case 0x60:
							if( m_isr & mask )
							{
								m_isr &= ~mask;
							}
							break;
						case 0x80:
							m_prio = (m_prio + 1) & 7;
							break;
						case 0xa0:
							for (n = 0, mask = 1<<m_prio; n < 8; n++, mask = (mask<<1) | (mask>>7))
							{
								if( m_isr & mask )
								{
									m_isr &= ~mask;
									m_prio = (m_prio + 1) & 7;
									break;
								}
							}
							break;
						case 0xc0:
							m_prio = (n + 1) & 7;
							break;
						case 0xe0:
							if( m_isr & mask )
							{
								m_isr &= ~mask;
								m_prio = (n + 1) & 7;
							}
							break;
					}
				}
			}
			break;

		case 1:
			switch(m_state)
			{
				case state_t::ICW1:
					break;

				case state_t::ICW2:
					/* write ICW2 */
					m_base = data & 0xf8;
					m_vector_addr_high = data ;
					if (m_cascade)
					{
						m_state = state_t::ICW3;
					}
					else
					{
						m_state = m_icw4_needed ? state_t::ICW4 : state_t::READY;
					}
					break;

				case state_t::ICW3:
					/* write ICW3 */
					m_slave = data;
					m_state = m_icw4_needed ? state_t::ICW4 : state_t::READY;
					break;

				case state_t::ICW4:
					/* write ICW4 */
					m_nested = (data & 0x10) ? 1 : 0;
					m_mode = (data >> 2) & 3;
					m_auto_eoi = (data & 0x02) ? 1 : 0;
					m_is_x86 = (data & 0x01) ? 1 : 0;
					m_state = state_t::READY;
					break;

				case state_t::READY:
					/* write OCW1 - set interrupt mask register */
					m_imr = data;
					break;
			}
			break;
	}
	irq_timer_adjust_zero();
}


//-------------------------------------------------
//  device_start - device-specific startup
//-------------------------------------------------

void Pic8259::start()
{
	m_inta_sequence = 0;
	// constructor values (MAME): irr, irq_lines, level_trig_mode = 0
	m_irr = 0;
	m_irq_lines = 0;
	m_level_trig_mode = 0;
}


//-------------------------------------------------
//  device_reset - device-specific reset
//-------------------------------------------------

void Pic8259::reset()
{
	m_state = state_t::READY;
	m_isr = 0;
	m_irr = 0;
	m_irq_lines = 0;
	m_prio = 0;
	m_imr = 0;
	m_input = 0;
	m_ocw3 = 0;
	m_level_trig_mode = 0;
	m_vector_size = 0;
	m_cascade = 0;
	m_icw4_needed = 0;
	m_base = 0;
	m_slave = 0;
	m_nested = 0;
	m_mode = 0;
	m_auto_eoi = 0;
	m_is_x86 = 0;
	m_vector_addr_low = 0;
	m_vector_addr_high = 0;
	m_current_level = -1;
	m_inta_sequence = 0;

	m_master = 1; // V5x: in_sp_callback().set_constant(1)
}

} // namespace v53
