// L7A1045 L6028 DSP-A sound chip (32 sample-playback voices), as used on the HNG64.
// Port of MAME src/devices/sound/l7a1045_l6028_dsp_a.cpp (BSD-3-Clause, R. Belmont,
// O. Galibert; original work by David Haywood, Angelo Salese, ElSemi).
#pragma once
#include <stdint.h>
#include <functional>
#include <vector>

class L7A1045 {
public:
    static constexpr int NUM_VOICES = 32;
    static constexpr int REGS_PER_VOICE = 16;
    static constexpr double SAMPLE_RATE = 33868800.0 / 768.0;   // 44100 Hz

    std::function<void(bool)> drq_handler;   // DMA request to the sound CPU (channel 3)

    void init(std::vector<uint8_t> sample_rom);   // 16 MB "l7a1045" ROM region
    void reset();

    // CPU I/O ports 0x00-0x0F (16-bit). offset is the byte offset within the block.
    void io_write(uint32_t offset, uint16_t data, uint16_t mem_mask);
    uint16_t io_read(uint32_t offset);

    // DMA channel 3 data path.
    uint16_t dma_r16_cb();
    void dma_w16_cb(uint16_t data);

    // Generate `count` stereo samples (int32 accumulators, 16-bit range) into out[2*count].
    void generate(int32_t* out, int count);

    // DMA timer (MAME: fires every 64 clocks = 8 output samples while DMA is started).
    bool dma_running() const { return (m_control >> 6) & 1; }

private:
    struct Voice {
        uint32_t loop_start = 0, start = 0, end = 0, step = 0, pos = 0, frac = 0;
        uint32_t sample_history[3] = {};
        uint8_t history_count = 0;
        uint16_t l_volume = 0, r_volume = 0, env_volume = 0, env_target = 0, env_step = 0;
        uint32_t env_pos = 0;
        uint16_t flt_freq = 0, flt_target = 0, flt_step = 0;
        uint32_t flt_pos = 0;
        uint8_t flt_resonance = 0;
        int32_t b = 0, l = 0;
        uint8_t send_dest = 0, send_level = 0, sample_type = 0;
    };

    Voice m_voice[NUM_VOICES];
    uint32_t m_key = 0;
    uint16_t m_control = 0;
    uint8_t m_cur_channel = 0, m_cur_register = 0;
    uint64_t m_regs[REGS_PER_VOICE][NUM_VOICES] = {};
    std::vector<uint8_t> m_rom;     // 16 MB sample ROM ("l6028_rom" space)
    std::vector<uint8_t> m_ram;     // 128 KB sample RAM ("l6028" space)

    uint16_t ram_word(uint32_t byteaddr) const;
    void ram_write_word(uint32_t byteaddr, uint16_t v);
    uint8_t rom_byte(uint32_t addr) const { return addr < m_rom.size() ? m_rom[addr] : 0; }
    uint16_t rom_word(uint32_t byteaddr) const { return (uint16_t)(rom_byte(byteaddr) | rom_byte(byteaddr + 1) << 8); }

    void voice_select_w(uint16_t data, uint16_t mem_mask);
    uint16_t voiceregs_r(int offset);
    void voiceregs_w(int offset, uint16_t data);
    void control_w(uint16_t data);
    void atomic_w(uint16_t data);
    void recalc_loop_start(Voice* v);
    static void advance_history(Voice& v, uint32_t address, uint32_t count);
    int32_t read_sample(uint8_t sample_type, uint32_t address) const;
    int32_t interpolated_sample(const Voice& v, uint32_t address, uint32_t frac) const;
};
