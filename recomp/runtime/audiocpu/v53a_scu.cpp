// license:BSD-3-Clause
// copyright-holders:smf, Robbbert
/*********************************************************************

    i8251.cpp

    Intel 8251 Universal Synchronous/Asynchronous Receiver Transmitter code
    NEC uPD71051 is a clone

    The V53/V53A use a customized version with only the Asynchronous mode
    and a split command / mode register

    Standalone port for the V53A SCU (see v53a_scu.h).

*********************************************************************/

#include "v53a_scu.h"
#include "v53a.h"

namespace v53 {

#define SBIT(x, n) (((x) >> (n)) & 1)

Scu::Scu(V53A &host) : m_host(host)
{
}

//-------------------------------------------------
//  device_start - device-specific startup
//-------------------------------------------------

void Scu::start()
{
	// constructor values
	m_cts = 1;
	m_dsr = 1;
	m_rxd = 1;
	m_rxc = 0;
	m_txc = 0;

	// zerofill
	m_flags = 0;
	m_sync_byte_count = 0;
	m_sync1 = 0;
	m_sync2 = 0;
	m_status = 0;
	m_command = 0;
	m_mode_byte = 0;
	m_delayed_tx_en = false;

	m_rxc_count = 0;
	m_txc_count = 0;
	m_br_factor = 0;

	m_rx_data = 0;
	m_tx_data = 0;
	m_sync8 = 0;
	m_sync16 = 0;
	m_tx_sync_shift = 0;
	m_syndet_pin = false;
	m_hunt_on = false;
	m_ext_syn_set = false;
	m_rxd_bits = 0;
	m_data_bits_count = 0;

	// v5x_scu_device::device_start
	m_sint = 0;
	m_simk = 0;

	transmit_register_reset();
	receive_register_reset();
}

//-------------------------------------------------
//  minimal serial shift register (replaces diserial)
//-------------------------------------------------

void Scu::set_data_frame(int start_bits, int data_bits, parity_t parity, int stop_bits)
{
	m_df_start_bits = start_bits;
	m_df_data_bits = data_bits;
	m_df_parity = parity;
	m_df_stop_bits = stop_bits;
}

void Scu::transmit_register_reset()
{
	m_tra_bits = 0;
	m_tra_left = 0;
}

void Scu::transmit_register_setup(uint8_t data)
{
	// LSB first: start bit(s), data bits, parity, stop bits
	uint32_t bits = 0;
	int n = 0;
	for (int i = 0; i < m_df_start_bits; i++)
		n++; // start bit = 0
	int ones = 0;
	for (int i = 0; i < m_df_data_bits; i++)
	{
		int b = (data >> i) & 1;
		ones += b;
		bits |= uint32_t(b) << n++;
	}
	if (m_df_parity != PARITY_NONE)
	{
		int p = (m_df_parity == PARITY_ODD) ? !(ones & 1) : (ones & 1);
		bits |= uint32_t(p) << n++;
	}
	for (int i = 0; i < m_df_stop_bits; i++)
		bits |= 1u << n++;
	m_tra_bits = uint16_t(bits);
	m_tra_left = n;
}

int Scu::transmit_register_get_data_bit()
{
	int b = m_tra_bits & 1;
	m_tra_bits >>= 1;
	m_tra_left--;
	return b;
}

void Scu::receive_register_reset()
{
	m_rcv_sync = false;
	m_rcv_full = false;
	m_rcv_data = 0;
	m_rcv_count = 0;
}

void Scu::receive_register_update_bit(int bit)
{
	if (m_rcv_sync)
	{
		m_rcv_data |= uint16_t(bit & 1) << m_rcv_count;
		m_rcv_count++;
		const int total = m_df_data_bits + (m_df_parity != PARITY_NONE ? 1 : 0) + 1; // first stop bit only
		if (m_rcv_count >= total)
		{
			m_rcv_sync = false;
			m_rcv_full = true;
		}
	}
	else if (!bit)
	{
		// start bit
		m_rcv_sync = true;
		m_rcv_full = false;
		m_rcv_data = 0;
		m_rcv_count = 0;
	}
}

void Scu::receive_register_extract()
{
	m_rcv_full = false;
	const uint8_t mask = uint8_t((1u << m_df_data_bits) - 1);
	m_rcv_char = uint8_t(m_rcv_data) & mask;
	int pos = m_df_data_bits;
	m_rcv_parity_error = false;
	if (m_df_parity != PARITY_NONE)
	{
		int ones = 0;
		for (int i = 0; i < m_df_data_bits; i++)
			ones += (m_rcv_char >> i) & 1;
		const int p = (m_rcv_data >> pos++) & 1;
		const int expect = (m_df_parity == PARITY_ODD) ? !(ones & 1) : (ones & 1);
		m_rcv_parity_error = (p != expect);
	}
	m_rcv_framing_error = ((m_rcv_data >> pos) & 1) == 0;
}


/*-------------------------------------------------
    update_rx_ready
-------------------------------------------------*/

void Scu::update_rx_ready()
{
	m_host.scu_pin(V53A::SCU_RXRDY, rxrdy_r());
	sint_bit_w(0, rxrdy_r());
}


/*-------------------------------------------------
    receive_clock
-------------------------------------------------*/

void Scu::receive_clock()
{
	/* receive enable? */
	if (SBIT(m_command, 2))
	{
		const bool sync = is_receive_register_synchronized();
		if (sync)
		{
			--m_rxc_count;
			if (m_rxc_count)
				return;
		}

		/* get bit received from other side and update receive register */
		receive_register_update_bit(m_rxd);
		if (is_receive_register_synchronized())
			m_rxc_count = sync ? m_br_factor : (3 * m_br_factor / 2);

		if (is_receive_register_full())
		{
			receive_register_extract();
			if (m_rcv_parity_error)
				m_status |= I8251_STATUS_PARITY_ERROR;
			if (m_rcv_framing_error)
				m_status |= I8251_STATUS_FRAMING_ERROR;
			receive_character(m_rcv_char);
		}
	}
}


/*-------------------------------------------------
    is_tx_enabled
-------------------------------------------------*/

bool Scu::is_tx_enabled() const
{
	return SBIT(m_command, 0) && !m_cts;
}


/*-------------------------------------------------
    check_for_tx_start
-------------------------------------------------*/

void Scu::check_for_tx_start()
{
	if (is_tx_enabled() && is_transmit_register_empty() && (m_status & (I8251_STATUS_TX_READY)) == 0)
		start_tx();
}


/*-------------------------------------------------
    start_tx
-------------------------------------------------*/

void Scu::start_tx()
{
	transmit_register_setup(m_tx_data);
	m_status &= ~I8251_STATUS_TX_EMPTY;
	m_status |= I8251_STATUS_TX_READY;
}


/*-------------------------------------------------
    transmit_clock
-------------------------------------------------*/

void Scu::transmit_clock()
{
	m_txc_count++;
	if (m_txc_count != m_br_factor)
		return;

	m_txc_count = 0;

	if (is_transmit_register_empty())
	{
		if ((m_status & I8251_STATUS_TX_READY) == 0 && (is_tx_enabled() || m_delayed_tx_en))
			start_tx();
		else
		{
			m_status |= I8251_STATUS_TX_EMPTY;
			if (m_sync_byte_count > 0 && is_tx_enabled())
			{
				// In synchronous mode, transmit sync character(s) during idle instead of mark state
				if (m_sync_byte_count == 2)
				{
					transmit_register_setup(uint8_t(m_sync16 >> m_tx_sync_shift));
					m_tx_sync_shift ^= 8;  // alternate between high byte (sync1) and low byte (sync2)
				}
				else
					transmit_register_setup(m_sync8);
			}
			else
			{
				// return TxD to marking state (high) if not sending break character
				m_host.scu_pin(V53A::SCU_TXD, !SBIT(m_command, 3));
			}
		}

		update_tx_ready();
		update_tx_empty();
	}

	/* if diserial has bits to send, make them so */
	if (!is_transmit_register_empty())
	{
		uint8_t data = transmit_register_get_data_bit();
		m_host.scu_pin(V53A::SCU_TXD, data);
	}
}


/*-------------------------------------------------
    update_tx_ready
-------------------------------------------------*/

void Scu::update_tx_ready()
{
	m_host.scu_pin(V53A::SCU_TXRDY, txrdy_r());
	sint_bit_w(1, txrdy_r());
}


/*-------------------------------------------------
    update_tx_empty
-------------------------------------------------*/

void Scu::update_tx_empty()
{
	m_host.scu_pin(V53A::SCU_TXEMPTY, (m_status & I8251_STATUS_TX_EMPTY) != 0);
}


/*---------------------------------------------------
    update_syndet
-----------------------------------------------------*/

void Scu::update_syndet(bool voltage)
{
	// Adjust status register
	if (voltage)
		m_status |= 0x40;
	else
		m_status &= ~0x40;

	// If syndet is set as an output pin, indicate new status
	if (!m_syndet_pin)
		m_host.scu_pin(V53A::SCU_SYNDET, voltage);
}


//-------------------------------------------------
//  device_reset - device-specific reset
//-------------------------------------------------

void Scu::reset()
{
	// v5x_scu_device::device_reset
	// FIXME: blindly copied from v53.cpp - not verified
	m_simk = 0x03;

	// i8251_device::device_reset

	/* i8251 datasheet explains the state of tx pin at reset */
	/* tx is set to 1 */
	m_host.scu_pin(V53A::SCU_TXD, 1);

	/* assumption */
	m_host.scu_pin(V53A::SCU_RTS, 1);
	m_host.scu_pin(V53A::SCU_DTR, 1);

	transmit_register_reset();
	receive_register_reset();
	/* expecting mode byte */
	m_flags = I8251_NEXT_MODE;

	/* no character to read by cpu */
	/* transmitter is ready and is empty */
	m_status = I8251_STATUS_TX_EMPTY | I8251_STATUS_TX_READY;
	m_mode_byte = 0;
	m_command = 0;
	m_rx_data = 0;
	m_tx_data = 0;
	m_rxc_count = m_txc_count = 0;
	m_br_factor = 1;

	/* update tx empty pin output */
	update_tx_empty();
	/* update rx ready pin output */
	update_rx_ready();
	/* update tx ready pin output */
	update_tx_ready();
}


/*-------------------------------------------------
    control_w
-------------------------------------------------*/

void Scu::command_w(uint8_t data)
{
	/* command */
	m_command = data;

	m_host.scu_pin(V53A::SCU_RTS, !SBIT(data, 5));
	m_host.scu_pin(V53A::SCU_DTR, !SBIT(data, 1));

	if (SBIT(data, 4))
	{
		m_status &= ~(I8251_STATUS_PARITY_ERROR | I8251_STATUS_OVERRUN_ERROR | I8251_STATUS_FRAMING_ERROR);
	}

	if (SBIT(data, 6))
	{
		// datasheet says "returns to mode format", not
		// completely resets the chip.  behavior of DEC Rainbow
		// backs this up.
		m_flags = I8251_NEXT_MODE;
	}

	// Hunt mode
	m_hunt_on = false;
	update_syndet(false);
	if (m_sync_byte_count)
		m_hunt_on = SBIT(data, 7);
	if (m_hunt_on)
		m_ext_syn_set = false;

	if (SBIT(data, 3))
		m_host.scu_pin(V53A::SCU_TXD, 0);

	check_for_tx_start();
	update_rx_ready();
	update_tx_ready();
	update_tx_empty();
}

void Scu::mode_w(uint8_t data)
{
	m_mode_byte = data;

	m_data_bits_count = ((data >> 2) & 0x03) + 5;

	parity_t parity = PARITY_NONE;
	switch (data & 0x30)
	{
		case 0x10:
			parity = PARITY_ODD;
			break;

		case 0x30:
			parity = PARITY_EVEN;
			break;

		default:
			break;
	}

	int stop_bits = 0;
	m_br_factor = 1;
	m_flags = I8251_NEXT_COMMAND;
	m_sync_byte_count = 0;
	m_syndet_pin = false;

	/* Synchronous or Asynchronous? */
	if (data & 0x03)
	{
		switch (data & 0xc0)
		{
			case 0x40: stop_bits = 1; break;
			case 0x80: stop_bits = 2; break; // 1.5 stop bits: sent as 2
			case 0xc0: stop_bits = 2; break;
			default: break; // inhibit
		}

		set_data_frame(1, m_data_bits_count, parity, stop_bits);

		switch (data & 0x03)
		{
			case 2:
				m_br_factor = 16;
				break;

			case 3:
				m_br_factor = 64;
				break;
		}
	}
	else
	{
		// synchronous mode is not supported by the V53 SCU (async only); MAME keeps the
		// i8251 synchronous paths, this port only keeps the register-level state.
		m_host.logerror("SCU: synchronous mode selected (not supported, mode %02x)\n", data);

		/* setup for sync byte(s) */
		m_flags = SBIT(data, 7) ? I8251_NEXT_SYNC2 : I8251_NEXT_SYNC1;
		m_sync_byte_count = SBIT(data, 7) ? 1 : 2;
		set_data_frame(0, m_data_bits_count, parity, stop_bits);
		m_syndet_pin = SBIT(data, 6);
		m_sync8 = 0;
		m_sync16 = 0;
		m_tx_sync_shift = 8;
		m_rxd_bits = 0;
		m_sync1 = 0;
	}

	receive_register_reset();
	m_txc_count = 0;
}

void Scu::sync1_w(uint8_t data)
{
	m_sync16 = (data << 8);
	m_flags = I8251_NEXT_SYNC2;
}

void Scu::sync2_w(uint8_t data)
{
	if (m_sync_byte_count == 2)
		m_sync16 |= data;
	else
		m_sync8 = data;

	m_flags = I8251_NEXT_COMMAND;
}

void Scu::control_w(uint8_t data)
{
	if (m_flags == I8251_NEXT_SYNC1)
		sync1_w(data);
	else if (m_flags == I8251_NEXT_SYNC2)
		sync2_w(data);
	else if (m_flags == I8251_NEXT_MODE)
		mode_w(data);
	else
		command_w(data);
}


/*-------------------------------------------------
    status_r
-------------------------------------------------*/

uint8_t Scu::status_r()
{
	uint8_t status = (m_dsr << 7) | m_status;

	// Syndet always goes off after status read
	update_syndet(false);

	return status;
}


/*-------------------------------------------------
    data_w
-------------------------------------------------*/

void Scu::data_w(uint8_t data)
{
	m_tx_data = data;

	/* writing clears */
	m_status &=~I8251_STATUS_TX_READY;
	update_tx_ready();

	// Store state of tx enable when writing to DB buffer
	m_delayed_tx_en = is_tx_enabled();
	check_for_tx_start();

	/* if transmitter is active, then tx empty will be signalled */
	update_tx_ready();
	update_tx_empty();
}


/*-------------------------------------------------
    receive_character - called when last
    bit of data has been received
-------------------------------------------------*/

void Scu::receive_character(uint8_t ch)
{
	m_rx_data = ch;

	/* char has not been read and another has arrived! */
	if (m_status & I8251_STATUS_RX_READY)
	{
		m_status |= I8251_STATUS_OVERRUN_ERROR;
	}

	m_status |= I8251_STATUS_RX_READY;

	update_rx_ready();
}


/*-------------------------------------------------
    data_r - read data
-------------------------------------------------*/

uint8_t Scu::data_r()
{
	/* reading clears */
	m_status &= ~I8251_STATUS_RX_READY;
	update_rx_ready();
	return m_rx_data;
}


void Scu::write_rxd(int state)
{
	m_rxd = state ? 1 : 0;
}

void Scu::write_cts(int state)
{
	m_cts = state ? 1 : 0;

	check_for_tx_start();
	update_tx_ready();
	update_tx_empty();
}

void Scu::write_dsr(int state)
{
	m_dsr = !state;
}

void Scu::write_rxc(int state)
{
	state = state ? 1 : 0;

	if (!m_rxc && state)
	{
		if (m_sync_byte_count == 0)
			receive_clock();
		// synchronous receive (sync1_rxc/sync2_rxc) not ported: V53 SCU is async only
	}

	m_rxc = state;
}

void Scu::write_txc(int state)
{
	state = state ? 1 : 0;

	if (m_txc != state)
	{
		m_txc = state;

		if (!m_txc)
			transmit_clock();
	}
}

int Scu::txrdy_r()
{
	return is_tx_enabled() && (m_status & I8251_STATUS_TX_READY) != 0;
}

int Scu::rxrdy_r()
{
	// masked?
	if (!SBIT(m_command, 2))
		return 0;

	return (m_status & I8251_STATUS_RX_READY) != 0;
}

void Scu::sint_bit_w(int bit, int state)
{
	if (state)
		m_sint |= (1 << bit);
	else
		m_sint &= ~(1 << bit);

	update_sint();
}

void Scu::update_sint()
{
	m_host.scu_sint((m_sint & ~m_simk) != 0);
}

uint8_t Scu::read(uint32_t offset)
{
	uint8_t data = 0;

	switch (offset)
	{
	case 0: data = data_r(); break;
	case 1: data = status_r(); break;
	case 2: break;
	case 3: data = m_simk; break;
	}

	return data;
}

void Scu::write(uint32_t offset, uint8_t data)
{
	switch (offset)
	{
	case 0: data_w(data); break;
	case 1: control_w(data); break;
	case 2: mode_w(data); break;
	case 3: m_simk = data; update_sint(); break;
	}
}

} // namespace v53
