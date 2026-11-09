// license:BSD-3-Clause
// copyright-holders:Curt Coder
/***************************************************************************

    AMD AM9517A
    Intel 8237A
    NEC uPD71037

    NEC uPD71071 (extended version of above)

    a variant is used in the V53 CPU which offers subsets of both the
    uPD71071 and uPD71037 functionality depending on a mode bit.

    Multimode DMA Controller emulation

    Standalone port for the V53A DMAU (see v53a_dmau.h).

***************************************************************************/

/*

    When the V53 operates in uPD71071 compatible mode there are the following
    differences from a real uPD71071

                               V53     Real uPD71071
    Software Reqs              No      Yes
    Memory-to-Memory DMA       No      Yes
    DMARQ active level         High    programmable
    DMAAK active level         Low     programmable
    Bus Cycle                  4       4 or 3

    we don't currently handle the differences

*/

#include "v53a_dmau.h"
#include "v53a.h"

namespace v53 {

//**************************************************************************
//  MACROS / CONSTANTS
//**************************************************************************

#define DBIT(x, n) (((x) >> (n)) & 1)

#define COMMAND_MEM_TO_MEM          DBIT(m_command, 0)
#define COMMAND_CH0_ADDRESS_HOLD    DBIT(m_command, 1)
#define COMMAND_DISABLE             DBIT(m_command, 2)
#define COMMAND_COMPRESSED_TIMING   DBIT(m_command, 3)
#define COMMAND_ROTATING_PRIORITY   DBIT(m_command, 4)
#define COMMAND_EXTENDED_WRITE      DBIT(m_command, 5)
#define COMMAND_DREQ_ACTIVE_LOW     DBIT(m_command, 6)
#define COMMAND_DACK_ACTIVE_HIGH    DBIT(m_command, 7)


#define MODE_TRANSFER_MASK          (m_channel[m_current_channel].m_mode & 0x0c)
#define MODE_TRANSFER_VERIFY        0x00
#define MODE_TRANSFER_WRITE         0x04
#define MODE_TRANSFER_READ          0x08
#define MODE_TRANSFER_ILLEGAL       0x0c
#define MODE_AUTOINITIALIZE         DBIT(m_channel[m_current_channel].m_mode, 4)
#define MODE_ADDRESS_DECREMENT      DBIT(m_channel[m_current_channel].m_mode, 5)
#define MODE_MASK                   (m_channel[m_current_channel].m_mode & 0xc0)
#define MODE_DEMAND                 0x00
#define MODE_SINGLE                 0x40
#define MODE_BLOCK                  0x80
#define MODE_CASCADE                0xc0


enum
{
	STATE_SI,
	STATE_S0,
	STATE_SC,
	STATE_S1,
	STATE_S2,
	STATE_S3,
	STATE_SW,
	STATE_S4,
	STATE_S11,
	STATE_S12,
	STATE_S13,
	STATE_S14,
	STATE_S21,
	STATE_S22,
	STATE_S23,
	STATE_S24
};


Dmau::Dmau(V53A &host) : m_host(host)
{
	for (auto &elem : m_channel)
	{
		elem.m_address = 0;
		elem.m_count = 0;
		elem.m_base_address = 0;
		elem.m_base_count = 0;
		elem.m_mode = 0;
	}
}

void Dmau::trigger()
{
	if (m_waiting)
	{
		m_waiting = false;
		m_host.dma_resume();
	}
}

//**************************************************************************
//  INLINE HELPERS
//**************************************************************************

void Dmau::dma_request(int channel, bool state)
{
	if (state)
		m_status |= (1 << (channel + 4));
	else
		m_status &= ~(1 << (channel + 4));

	trigger();
}

void Dmau::mask_channel(int channel, bool state)
{
	if (state)
		m_mask |= 1 << channel;
	else
		m_mask &= ~(1 << channel);
}

void Dmau::set_mask_register(uint8_t mask)
{
	m_mask = mask;
}

inline bool Dmau::is_request_active(int channel)
{
	return (DBIT(COMMAND_DREQ_ACTIVE_LOW ? ~m_status : m_status, channel + 4) && !DBIT(m_mask, channel)) ? true : false;
}

inline bool Dmau::is_software_request_active(int channel)
{
	return DBIT(m_request, channel) && ((m_channel[channel].m_mode & 0xc0) == MODE_BLOCK);
}

inline void Dmau::set_hreq(int state)
{
	if (m_hreq != state)
	{
		m_host.dmau_hreq(state);

		m_hreq = state;
	}
}

inline void Dmau::set_dack()
{
	for (int channel = 0; channel < 4; channel++)
	{
		if ((channel == m_current_channel) && !COMMAND_MEM_TO_MEM)
			m_host.dmau_dack(channel, COMMAND_DACK_ACTIVE_HIGH);
		else
			m_host.dmau_dack(channel, !COMMAND_DACK_ACTIVE_HIGH);
	}
}

inline void Dmau::set_eop(int state)
{
	if (m_eop != state)
	{
		m_host.dmau_eop(state);

		m_eop = state;
	}
}

inline int Dmau::get_state1(bool msb_changed)
{
	if (COMMAND_MEM_TO_MEM)
	{
		return msb_changed ? STATE_S11 : STATE_S12;
	}
	else
	{
		return msb_changed ? STATE_S1 : STATE_S2;
	}
}

void Dmau::am9517a_dma_read()
{
	uint32_t offset = m_channel[m_current_channel].m_address;

	switch (MODE_TRANSFER_MASK)
	{
	case MODE_TRANSFER_VERIFY:
	case MODE_TRANSFER_WRITE:
		m_temp = m_host.dmau_ior(m_current_channel, offset);
		break;

	case MODE_TRANSFER_READ:
		m_temp = m_host.dmau_memr(offset);
		break;
	}
}

void Dmau::am9517a_dma_write()
{
	uint32_t offset = m_channel[m_current_channel].m_address;

	switch (MODE_TRANSFER_MASK)
	{
	case MODE_TRANSFER_VERIFY:
		{
			uint8_t v1 = m_host.dmau_memr(offset);
			(void)v1;
		}
		break;

	case MODE_TRANSFER_WRITE:
		m_host.dmau_memw(offset, uint8_t(m_temp));
		break;

	case MODE_TRANSFER_READ:
		m_host.dmau_iow(m_current_channel, offset, uint8_t(m_temp));
		break;
	}
}

// upd71071_device::dma_read
void Dmau::dma_read()
{
	if (m_channel[m_current_channel].m_mode & 0x1)
	{
		uint32_t const offset = m_channel[m_current_channel].m_address >> 1;

		switch (MODE_TRANSFER_MASK)
		{
		case MODE_TRANSFER_VERIFY:
		case MODE_TRANSFER_WRITE:
			m_temp = m_host.dmau_io16r(m_current_channel, offset);
			break;

		case MODE_TRANSFER_READ:
			m_temp = m_host.dmau_mem16r(offset);
			break;
		}
	}
	else
		am9517a_dma_read();
}

// upd71071_device::dma_write
void Dmau::dma_write()
{
	if (m_channel[m_current_channel].m_mode & 0x1)
	{
		uint32_t const offset = m_channel[m_current_channel].m_address >> 1;

		switch (MODE_TRANSFER_MASK)
		{
		case MODE_TRANSFER_VERIFY:
		{
			uint16_t const v1 = m_host.dmau_mem16r(offset);
			(void)v1;
		}
		break;

		case MODE_TRANSFER_WRITE:
			m_host.dmau_mem16w(offset, m_temp);
			break;

		case MODE_TRANSFER_READ:
			m_host.dmau_io16w(m_current_channel, offset, m_temp);
			break;
		}
	}
	else
		am9517a_dma_write();
}

inline void Dmau::dma_advance()
{
	bool msb_changed = false;

	if (m_current_channel || !COMMAND_MEM_TO_MEM || !COMMAND_CH0_ADDRESS_HOLD)
	{
		if (MODE_ADDRESS_DECREMENT)
		{
			m_channel[m_current_channel].m_address -= transfer_size(m_current_channel);
			m_channel[m_current_channel].m_address &= m_address_mask;

			if ((m_channel[m_current_channel].m_address & 0xff) == 0xff)
			{
				msb_changed = true;
			}
		}
		else
		{
			m_channel[m_current_channel].m_address += transfer_size(m_current_channel);
			m_channel[m_current_channel].m_address &= m_address_mask;

			if ((m_channel[m_current_channel].m_address & 0xff) == 0x00)
			{
				msb_changed = true;
			}
		}
	}

	if (m_channel[m_current_channel].m_count-- == 0)
	{
		end_of_process();
	}
	else
	{
		switch (MODE_MASK)
		{
		case MODE_DEMAND:
			if (!is_request_active(m_current_channel))
			{
				set_hreq(0);
				set_dack();
				m_state = STATE_SI;
			}
			else
			{
				m_state = get_state1(msb_changed);
			}
			break;

		case MODE_SINGLE:
			set_hreq(0);
			set_dack();
			m_state = STATE_SI;
			break;

		case MODE_BLOCK:
			m_state = get_state1(msb_changed);
			break;

		case MODE_CASCADE:
			break;
		}
	}
}

void Dmau::end_of_process()
{
	// terminal count
	if (COMMAND_MEM_TO_MEM)
	{
		m_status |= 1 << 0;
		m_status |= 1 << 1;
		m_request &= ~(1 << 0);
		m_request &= ~(1 << 1);
	}
	else
	{
		m_status |= 1 << m_current_channel;
		m_request &= ~(1 << m_current_channel);
	}

	if (MODE_AUTOINITIALIZE)
	{
		// autoinitialize
		m_channel[m_current_channel].m_address = m_channel[m_current_channel].m_base_address;
		m_channel[m_current_channel].m_count = m_channel[m_current_channel].m_base_count;
	}
	else
	{
		// mask out channel
		mask_channel(m_current_channel, true);
	}

	set_eop(0);
	set_hreq(0);

	m_current_channel = -1;
	set_dack();

	m_state = STATE_SI;
}


//-------------------------------------------------
//  device_start - device-specific startup
//-------------------------------------------------

void Dmau::start()
{
	for (auto &elem : m_channel)
	{
		elem.m_address = 0;
		elem.m_count = 0;
		elem.m_base_address = 0;
		elem.m_base_count = 0;
		elem.m_mode = 0;
	}

	// constructor values
	m_hack = 0;
	m_ready = 1;
	m_command = 0;
	m_status = 0;

	m_address_mask = 0x00ffffff; // upd71071

	// force clear upon initial reset
	m_eop = 1;

	m_selected_channel = 0;
	m_base = 0;
	m_command_high = 0; // uninitialised in MAME
	m_waiting = false;
}


//-------------------------------------------------
//  device_reset - device-specific reset
//-------------------------------------------------

void Dmau::reset()
{
	soft_reset();

	m_selected_channel = 0;
	m_base = 0;
}

void Dmau::soft_reset()
{
	m_state = STATE_SI;
	m_command = 0;
	m_status &= 0xf0;
	m_request = 0;
	m_mask = 0x0f;
	m_temp = 0;
	m_msb = 0;
	m_current_channel = -1;
	m_last_channel = 3;
	m_hreq = -1;

	set_hreq(0);
	set_eop(0);

	set_dack();
}

//-------------------------------------------------
//  step - one iteration of am9517a_device::execute_run
//-------------------------------------------------

void Dmau::step()
{
	switch (m_state)
	{
	case STATE_SI:
		if (!COMMAND_DISABLE)
		{
			int priority[] = { 0, 1, 2, 3 };

			if (COMMAND_ROTATING_PRIORITY)
			{
				int last_channel = m_last_channel;

				for (int channel = 3; channel >= 0; channel--)
				{
					priority[channel] = last_channel;
					last_channel--;
					if (last_channel < 0) last_channel = 3;
				}
			}

			for (int channel = 0; channel < 4; channel++)
			{
				if (is_request_active(priority[channel]) || is_software_request_active(priority[channel]))
				{
					m_current_channel = m_last_channel = priority[channel];
					m_state = STATE_S0;
					break;
				}
				else if (COMMAND_MEM_TO_MEM && DBIT(m_request, channel) && ((m_channel[channel].m_mode & 0xc0) == MODE_SINGLE))
				{
					m_current_channel = m_last_channel = priority[channel];
					m_state = STATE_S0;
					break;
				}
			}
		}
		if(m_state == STATE_SI)
		{
			suspend_until_trigger();
		}
		break;

	case STATE_S0:
		set_hreq(1);

		if (m_hack)
		{
			m_state = (MODE_MASK == MODE_CASCADE) ? STATE_SC : get_state1(true);
		}
		else
		{
			suspend_until_trigger();
		}
		break;

	case STATE_SC:
		if (!is_request_active(m_current_channel))
		{
			set_hreq(0);
			m_current_channel = -1;
			m_state = STATE_SI;
		}
		else
		{
			suspend_until_trigger();
		}

		set_dack();
		break;

	case STATE_S1:
		m_state = STATE_S2;
		break;

	case STATE_S2:
		set_dack();
		if (COMMAND_COMPRESSED_TIMING)
		{
			// signal end of process during last cycle
			if (m_channel[m_current_channel].m_count == 0)
				set_eop(1);

			m_state = STATE_S4;
		}
		else
			m_state = STATE_S3;
		break;

	case STATE_S3:
		// signal end of process during last cycle
		if (m_channel[m_current_channel].m_count == 0)
			set_eop(1);

		dma_read();

		if (COMMAND_EXTENDED_WRITE)
		{
			dma_write();
		}

		m_state = m_ready ? STATE_S4 : STATE_SW;
		break;

	case STATE_SW:
		m_state = m_ready ? STATE_S4 : STATE_SW;
		break;

	case STATE_S4:
		if (COMMAND_COMPRESSED_TIMING)
		{
			dma_read();
			dma_write();
		}
		else if (!COMMAND_EXTENDED_WRITE)
		{
			dma_write();
		}

		dma_advance();
		break;

	case STATE_S11:
		m_current_channel = 0;

		m_state = STATE_S12;
		break;

	case STATE_S12:
		m_state = STATE_S13;
		break;

	case STATE_S13:
		m_state = STATE_S14;
		break;

	case STATE_S14:
		dma_read();

		m_state = STATE_S21;
		break;

	case STATE_S21:
		m_current_channel = 1;

		m_state = STATE_S22;
		break;

	case STATE_S22:
		m_state = STATE_S23;
		break;

	case STATE_S23:
		// signal end of process during last cycle
		if (m_channel[m_current_channel].m_count == 0)
			set_eop(1);

		m_state = STATE_S24;
		break;

	case STATE_S24:
		dma_write();
		dma_advance();

		m_current_channel = 0;
		m_channel[m_current_channel].m_count--;
		if (MODE_ADDRESS_DECREMENT)
		{
			m_channel[m_current_channel].m_address -= transfer_size(m_current_channel);
			m_channel[m_current_channel].m_address &= m_address_mask;
		}
		else
		{
			m_channel[m_current_channel].m_address += transfer_size(m_current_channel);
			m_channel[m_current_channel].m_address &= m_address_mask;
		}

		break;
	}
}


//-------------------------------------------------
//  hack_w - hold acknowledge
//-------------------------------------------------

void Dmau::hack_w(int state)
{
	m_hack = state;
	trigger();
}

//-------------------------------------------------
//  upd71071 register layouts
//-------------------------------------------------

uint8_t Dmau::read(uint32_t offset)
{
	uint8_t ret = 0;
	int channel = m_selected_channel;

	switch (offset)
	{
		case 0x01:  // Channel
			ret = (1 << m_selected_channel);
			if (m_base != 0)
				ret |= 0x10;
			break;
		case 0x02:  // Count (low)
			if (m_base != 0)
				ret = m_channel[channel].m_base_count & 0xff;
			else
				ret = m_channel[channel].m_count & 0xff;
			break;
		case 0x03:  // Count (high)
			if (m_base != 0)
				ret = (m_channel[channel].m_base_count >> 8) & 0xff;
			else
				ret = (m_channel[channel].m_count >> 8) & 0xff;
			break;
		case 0x04:  // Address (low)
			if (m_base != 0)
				ret = m_channel[channel].m_base_address & 0xff;
			else
				ret = m_channel[channel].m_address & 0xff;
			break;
		case 0x05:  // Address (mid)
			if (m_base != 0)
				ret = (m_channel[channel].m_base_address >> 8) & 0xff;
			else
				ret = (m_channel[channel].m_address >> 8) & 0xff;
			break;
		case 0x06:  // Address (high)
			if (m_base != 0)
				ret = (m_channel[channel].m_base_address >> 16) & 0xff;
			else
				ret = (m_channel[channel].m_address >> 16) & 0xff;
			break;
		case 0x0a:  // Mode control
				ret = (m_channel[channel].m_mode);
			break;

		case 0x08:  // Device control (low)
			ret = m_command & 0xff;
			break;
		case 0x09:  // Device control (high) // UPD71071 only?
			ret = m_command_high & 0xff;
			break;
		case 0x0b:  // Status
			ret = m_status;
			// clear TC bits
			m_status &= 0xf0;
			break;
		case 0x0c:  // Temporary (low)
			ret = m_temp & 0xff;
			break;
		case 0x0d:  // Temporary (high) // UPD71071 only? (other doesn't do 16-bit?)
			ret = (m_temp >> 8 ) & 0xff;
			break;
		case 0x0e:  // Request
			ret = m_request;
			break;
		case 0x0f:  // Mask
			ret = m_mask;
			break;
	}

	return ret;
}

void Dmau::write(uint32_t offset, uint8_t data)
{
	int channel = m_selected_channel;

	// v5x_dmau_device::write filters these first
	switch (offset & 0xf)
	{
	case 0x0c:  // Temporary (low)
	case 0x0d:  // Temporary (high)
		return;

	case 0x0e:  // Request
		m_host.logerror("(invalid) DMA: Request set [%02x]\n", data); // no software requests on the v53 integrated version
		return;

	default:
		break;
	}

	// upd71071_device::write
	switch (offset & 0xf)
	{
		case 0x00:  // Initialise
			// bit 0 = soft reset
			if (DBIT(data, 0))
			{
				soft_reset();

				m_selected_channel = 0;
				m_base = 0;
			}
			break;
		case 0x01:  // Channel
			m_selected_channel = data & 0x03;
			m_base = data & 0x04;
			break;
		case 0x02:  // Count (low)
			m_channel[channel].m_base_count =
				(m_channel[channel].m_base_count & 0xff00) | data;
			if (m_base == 0)
				m_channel[channel].m_count =
				(m_channel[channel].m_count & 0xff00) | data;
			break;
		case 0x03:  // Count (high)
			m_channel[channel].m_base_count =
				(m_channel[channel].m_base_count & 0x00ff) | (data << 8);
			if (m_base == 0)
				m_channel[channel].m_count =
				(m_channel[channel].m_count & 0x00ff) | (data << 8);
			break;
		case 0x04:  // Address (low)
			m_channel[channel].m_base_address =
				(m_channel[channel].m_base_address & 0xffffff00) | data;
			if (m_base == 0)
				m_channel[channel].m_address =
				(m_channel[channel].m_address & 0xffffff00) | data;
			break;
		case 0x05:  // Address (mid)
			m_channel[channel].m_base_address =
				(m_channel[channel].m_base_address & 0xffff00ff) | (data << 8);
			if (m_base == 0)
				m_channel[channel].m_address =
				(m_channel[channel].m_address & 0xffff00ff) | (data << 8);
			break;
		case 0x06:  // Address (high)
			m_channel[channel].m_base_address =
				(m_channel[channel].m_base_address & 0xff00ffff) | (data << 16);
			if (m_base == 0)
				m_channel[channel].m_address =
				(m_channel[channel].m_address & 0xff00ffff) | (data << 16);
			break;
		case 0x0a:  // Mode control
			m_channel[channel].m_mode = data;
			// clear terminal count
			m_status &= ~(1 << channel);
			break;

		case 0x08:  // Device control (low)
			m_command = data;
			break;
		case 0x09:  // Device control (high)
			m_command_high = data;
			break;
		case 0x0c:  // Temporary (low)
			m_temp = (m_temp & 0xff00) | data;
			break;
		case 0x0d:  // Temporary (high)
			m_temp = (m_temp & 0x00ff) | (data << 8);
			break;
		case 0x0e:  // Request
			m_request = data;
			break;
		case 0x0f:  // Mask
			m_mask = data & 0x0f;
			break;
	}
	trigger();
}

} // namespace v53
