// license:BSD-3-Clause
// copyright-holders:smf, Robbbert
/*********************************************************************

    i8251 Universal Synchronous/Asynchronous Receiver Transmitter
    (V53A SCU: v5x_scu_device -> i8251_device)

    Standalone port of MAME src/devices/machine/i8251.{h,cpp}. The V53A SCU
    is async-only, so the synchronous receive paths (sync1_rxc/sync2_rxc,
    hunt mode, SYNDET) are kept only as far as the register model needs.

    MAME's device_serial_interface (emu/diserial) is not ported; a small
    built-in async shift register replaces it (start bit, 5-8 data bits,
    optional parity, 1/2 stop bits; 1.5 stop bits are sent as 2). Nothing is
    connected to the SCU on HNG64, so this only matters for the status
    register timing.

*********************************************************************/

#ifndef BURIKI_AUDIOCPU_V53A_SCU_H
#define BURIKI_AUDIOCPU_V53A_SCU_H

#pragma once

#include <cstdint>

namespace v53 {

class V53A;

class Scu
{
public:
	explicit Scu(V53A &host);

	void start();
	void reset();

	uint8_t read(uint32_t offset);
	void write(uint32_t offset, uint8_t data);

	void write_rxd(int state);
	void write_cts(int state);
	void write_dsr(int state);
	void write_txc(int state);
	void write_rxc(int state);

	int txrdy_r();
	int rxrdy_r();

private:
	enum
	{
		I8251_STATUS_FRAMING_ERROR = 0x20,
		I8251_STATUS_OVERRUN_ERROR = 0x10,
		I8251_STATUS_PARITY_ERROR = 0x08,
		I8251_STATUS_TX_EMPTY = 0x04,
		I8251_STATUS_RX_READY = 0x02,
		I8251_STATUS_TX_READY = 0x01
	};

	enum
	{
		I8251_NEXT_COMMAND = 0,
		I8251_NEXT_MODE,
		I8251_NEXT_SYNC1,
		I8251_NEXT_SYNC2,
	};

	enum parity_t { PARITY_NONE, PARITY_ODD, PARITY_EVEN };

	uint8_t data_r();
	void data_w(uint8_t data);
	uint8_t status_r();
	void control_w(uint8_t data);
	void command_w(uint8_t data);
	void mode_w(uint8_t data);
	void sync1_w(uint8_t data);
	void sync2_w(uint8_t data);

	void receive_character(uint8_t ch);
	void update_rx_ready();
	void update_tx_ready();
	void update_tx_empty();
	void update_syndet(bool voltage);
	void transmit_clock();
	void receive_clock();
	bool is_tx_enabled() const;
	void check_for_tx_start();
	void start_tx();

	void sint_bit_w(int bit, int state);
	void update_sint();

	// minimal replacement for device_serial_interface
	void set_data_frame(int start_bits, int data_bits, parity_t parity, int stop_bits);
	void transmit_register_reset();
	void transmit_register_setup(uint8_t data);
	bool is_transmit_register_empty() const { return m_tra_left == 0; }
	int transmit_register_get_data_bit();
	void receive_register_reset();
	void receive_register_update_bit(int bit);
	bool is_receive_register_synchronized() const { return m_rcv_sync; }
	bool is_receive_register_full() const { return m_rcv_full; }
	void receive_register_extract();

	V53A &m_host;

	uint8_t m_flags = 0;
	uint8_t m_sync_byte_count = 0;
	uint8_t m_sync1 = 0;
	uint16_t m_sync2 = 0;
	uint8_t m_status = 0;
	uint8_t m_command = 0;
	uint8_t m_mode_byte = 0;
	bool m_delayed_tx_en = false;

	int32_t m_cts = 1;
	int32_t m_dsr = 1;
	int32_t m_rxd = 1;
	int32_t m_rxc = 0;
	int32_t m_txc = 0;
	int m_rxc_count = 0;
	int m_txc_count = 0;
	int m_br_factor = 0;

	uint8_t m_rx_data = 0;
	uint8_t m_tx_data = 0;
	uint8_t m_sync8 = 0;
	uint16_t m_sync16 = 0;
	uint8_t m_tx_sync_shift = 0;
	bool m_syndet_pin = false;
	bool m_hunt_on = false;
	bool m_ext_syn_set = false;
	uint8_t m_rxd_bits = 0;
	uint8_t m_data_bits_count = 0;

	// v5x_scu
	uint8_t m_sint = 0;
	uint8_t m_simk = 0;

	// serial frame
	int m_df_start_bits = 1;
	int m_df_data_bits = 8;
	parity_t m_df_parity = PARITY_NONE;
	int m_df_stop_bits = 1;
	// transmit shift register
	uint16_t m_tra_bits = 0;
	int m_tra_left = 0;
	// receive shift register
	bool m_rcv_sync = false;
	bool m_rcv_full = false;
	uint16_t m_rcv_data = 0;
	int m_rcv_count = 0;
	uint8_t m_rcv_char = 0;
	bool m_rcv_parity_error = false;
	bool m_rcv_framing_error = false;
};

} // namespace v53

#endif // BURIKI_AUDIOCPU_V53A_SCU_H
