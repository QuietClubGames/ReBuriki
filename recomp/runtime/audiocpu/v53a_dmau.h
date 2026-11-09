// license:BSD-3-Clause
// copyright-holders:Curt Coder
/***************************************************************************

    AMD AM9517A/8237A Multimode DMA Controller emulation
    (V53A DMAU: v5x_dmau_device -> upd71071_device -> am9517a_device)

    Standalone port of MAME src/devices/machine/am9517a.{h,cpp}, only the
    pieces used by the V53A DMAU in uPD71071 mode.

    Differences from MAME:
      - the device_execute_interface (own CPU-like scheduling at clock/4) is
        replaced by step(), called by the owning V53A once per DMA clock
        (every 4 V53A clocks) while the DMAU isn't waiting for a trigger;
        suspend_until_trigger(1) / trigger(1) become m_waiting / trigger()
      - devcb callbacks go through the V53A callback table

***************************************************************************/

#ifndef BURIKI_AUDIOCPU_V53A_DMAU_H
#define BURIKI_AUDIOCPU_V53A_DMAU_H

#pragma once

#include <cstdint>

namespace v53 {

class V53A;

class Dmau
{
public:
	explicit Dmau(V53A &host);

	void start();   // device_start
	void reset();   // device_reset (upd71071)

	uint8_t read(uint32_t offset);
	void write(uint32_t offset, uint8_t data);

	void hack_w(int state);
	void ready_w(int state) { m_ready = state; }
	void dreq_w(int channel, int state) { dma_request(channel, state != 0); }

	// one DMA clock of the am9517a state machine (one iteration of execute_run)
	void step();
	bool waiting() const { return m_waiting; }

	// debug
	uint32_t channel_address(int ch) const { return m_channel[ch].m_address; }
	uint32_t channel_count(int ch) const { return m_channel[ch].m_count; }
	uint8_t mask() const { return m_mask; }
	uint8_t status() const { return m_status; }
	int state() const { return m_state; }

private:
	void trigger();
	void suspend_until_trigger() { m_waiting = true; }

	void dma_request(int channel, bool state);
	void mask_channel(int channel, bool state);
	void set_mask_register(uint8_t mask);
	bool is_request_active(int channel);
	bool is_software_request_active(int channel);
	void set_hreq(int state);
	void set_dack();
	void set_eop(int state);
	int get_state1(bool msb_changed);
	void dma_advance();
	void end_of_process();
	void dma_read();
	void dma_write();
	int transfer_size(int const channel) const { return (m_channel[channel].m_mode & 0x1) ? 2 : 1; }
	void soft_reset();

	// am9517a base versions (8-bit transfers)
	void am9517a_dma_read();
	void am9517a_dma_write();

	V53A &m_host;

	bool m_waiting = false;
	uint32_t m_address_mask = 0x00ffffff;

	struct
	{
		uint32_t m_address;
		uint32_t m_count;
		uint32_t m_base_address;
		uint32_t m_base_count;
		uint8_t m_mode;
	} m_channel[4];

	int m_msb = 0;
	int m_hreq = 0;
	int m_hack = 0;
	int m_ready = 1;
	int m_eop = 1;
	int m_state = 0;
	int m_current_channel = -1;
	int m_last_channel = 3;
	uint8_t m_command = 0;
	uint8_t m_mask = 0x0f;
	uint8_t m_status = 0;
	uint16_t m_temp = 0;
	uint8_t m_request = 0;

	// upd71071
	int m_selected_channel = 0;
	int m_base = 0;
	uint8_t m_command_high = 0;
};

} // namespace v53

#endif // BURIKI_AUDIOCPU_V53A_DMAU_H
