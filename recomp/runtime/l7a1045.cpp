// L7A1045 L6028 DSP-A. Port of MAME src/devices/sound/l7a1045_l6028_dsp_a.cpp
// (BSD-3-Clause, copyright-holders R. Belmont, O. Galibert). See the MAME file for the
// register documentation; names here match it.
#include "l7a1045.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr int L6028_Start = 0, L6028_End = 1, L6028_Loop_Start = 2, L6028_Volume_Env = 3,
              L6028_Volume_Env_Target = 4, L6028_Filter_Env = 5, L6028_Filter_Env_Target = 6,
              L6028_Mixer_Params = 7;
constexpr int CONTROL_DMA_START = 6, CONTROL_KEY_ON = 8;

// Windowed sinc interpolator coefficients (MAME: textbook values, 8 taps, 4096 phases).
const std::array<std::array<int32_t, 8>, 0x1000>& coefficients() {
    static const auto table = [] {
        std::array<std::array<int32_t, 8>, 0x1000> result{};
        const double pi = 3.14159265358979323846;
        result[0][3] = 1 << 30;
        for (int phase = 1; phase <= 0x800; phase++) {
            std::array<double, 8> weights;
            double total = 0;
            for (int tap = 0; tap < 8; tap++) {
                const double x = pi * (double(phase) / 0x1000 - (tap - 3));
                weights[tap] = (std::sin(x) / x) * (1.0 + std::cos(x / 4.0)) / 2.0;
                total += weights[tap];
            }
            int32_t residual = 1 << 30;
            for (int tap = 0; tap < 8; tap++) {
                result[phase][tap] = (phase == 0x800 && tap >= 4) ? result[phase][7 - tap]
                                                                  : int32_t(std::lround(weights[tap] * (1 << 30) / total));
                residual -= result[phase][tap];
            }
            if (phase == 0x800) { result[phase][3] += residual / 2; result[phase][4] += residual / 2; }
            else {
                result[phase][3] += residual;
                for (int tap = 0; tap < 8; tap++) result[0x1000 - phase][7 - tap] = result[phase][tap];
            }
        }
        return result;
    }();
    return table;
}

} // namespace

void L7A1045::init(std::vector<uint8_t> sample_rom) {
    m_rom = std::move(sample_rom);
    m_ram.assign(0x20000, 0);
    coefficients();
    reset();
}

void L7A1045::reset() {
    m_key = 0;
    for (auto& v : m_voice) v.history_count = 0;
}

// MAME dsp_ram_map: 128 KB of RAM at 0, everything above reads as 0 (spurious reads).
uint16_t L7A1045::ram_word(uint32_t a) const {
    a &= 0x1ffffff;
    if (a + 1 >= m_ram.size()) return 0;
    return (uint16_t)(m_ram[a] | m_ram[a + 1] << 8);
}
void L7A1045::ram_write_word(uint32_t a, uint16_t v) {
    a &= 0x1ffffff;
    if (a + 1 >= m_ram.size()) return;
    m_ram[a] = (uint8_t)v; m_ram[a + 1] = (uint8_t)(v >> 8);
}

void L7A1045::advance_history(Voice& voice, uint32_t address, uint32_t count) {
    if (count >= 3) {
        for (int i = 0; i < 3; i++) voice.sample_history[i] = address + count - 1 - i;
        voice.history_count = 3;
    } else {
        for (uint32_t i = 0; i < count; i++) {
            voice.sample_history[2] = voice.sample_history[1];
            voice.sample_history[1] = voice.sample_history[0];
            voice.sample_history[0] = address + i;
            voice.history_count = (uint8_t)std::min<int>(voice.history_count + 1, 3);
        }
    }
}

int32_t L7A1045::read_sample(uint8_t sample_type, uint32_t address) const {
    if (sample_type == 0) return int16_t(ram_word(address << 1));   // 16-bit linear RAM
    const uint8_t data = rom_byte(address & 0x1ffffff);              // 12-bit non-linear ROM in 8 bits
    const int32_t mantissa = (data >> 2) - ((data & 0x80) ? 0x40 : 0);
    return mantissa * (1 << (4 + 2 * (~data & 3)));
}

int32_t L7A1045::interpolated_sample(const Voice& voice, uint32_t address, uint32_t frac) const {
    if (voice.sample_type > 1) return 0;
    const int32_t sample = read_sample(voice.sample_type, address);
    if (!frac) return sample;
    const uint32_t loop_length = voice.loop_start;
    const bool valid_loop = (voice.end > voice.start) && loop_length && (loop_length <= voice.end - voice.start);
    if (address < voice.start || address >= voice.end || (!valid_loop && (voice.end - address <= 4))) return sample;

    const auto& c = coefficients()[frac];
    int64_t result = int64_t(sample) * c[3];
    for (int i = 0; i < voice.history_count; i++)
        result += int64_t(read_sample(voice.sample_type, voice.sample_history[i])) * c[2 - i];
    for (int i = 4; i < 8; i++) {
        if (++address == voice.end) address = voice.end - loop_length;
        result += int64_t(read_sample(voice.sample_type, address)) * c[i];
    }
    return (result < 0) ? (int32_t)-((-result + (int64_t(1) << 29)) >> 30) : (int32_t)((result + (int64_t(1) << 29)) >> 30);
}

void L7A1045::generate(int32_t* out, int count) {
    for (int j = 0; j < count * 2; j++) out[j] = 0;
    for (int i = 0; i < NUM_VOICES; i++) {
        if (!(m_key & (1u << i))) continue;
        Voice* vptr = &m_voice[i];
        const uint32_t start = vptr->start, end = vptr->end, step = vptr->step;
        uint32_t pos = vptr->pos, frac = vptr->frac;
        for (int j = 0; j < count; j++) {
            const uint32_t previous_address = start + pos;
            const uint32_t advance = frac >> 12;
            pos += advance;
            frac &= 0xfff;
            if ((end > start) && ((start + pos) >= end)) {
                if (previous_address < end) advance_history(*vptr, previous_address, end - previous_address);
                else vptr->history_count = 0;
                pos = (vptr->end - vptr->start) - vptr->loop_start;
            } else {
                advance_history(*vptr, previous_address, advance);
            }
            const int32_t sample = interpolated_sample(*vptr, start + pos, frac);
            frac += step;

            vptr->env_pos += vptr->env_step;
            const int steps = (int)(vptr->env_pos / 0x100);
            if (steps > 0) {
                if (vptr->env_volume < vptr->env_target) vptr->env_volume += (uint16_t)std::min(steps, vptr->env_target - vptr->env_volume);
                else if (vptr->env_volume > vptr->env_target) vptr->env_volume -= (uint16_t)std::min(steps, vptr->env_volume - vptr->env_target);
            }
            vptr->env_pos &= 0xff;

            vptr->flt_pos += vptr->flt_step;
            const int flt_steps = (int)(vptr->flt_pos / 0x100);
            if (flt_steps > 0) {
                if (vptr->flt_freq < vptr->flt_target) vptr->flt_freq += (uint16_t)std::min(flt_steps, vptr->flt_target - vptr->flt_freq);
                else if (vptr->flt_freq > vptr->flt_target) vptr->flt_freq -= (uint16_t)std::min(flt_steps, vptr->flt_freq - vptr->flt_target);
            }
            vptr->flt_pos &= 0xff;

            // Chamberlin low-pass filter (see MAME)
            const int64_t h = int64_t(sample) - vptr->l - vptr->b + ((int64_t(vptr->flt_resonance) * vptr->b) >> 4);
            vptr->b += (int32_t)((vptr->flt_freq * h) >> 15);
            vptr->l += (int32_t)((int64_t(vptr->flt_freq) * vptr->b) >> 15);

            const int64_t fout = vptr->l;
            out[j * 2 + 0] += (int32_t)((fout * (int64_t)(uint64_t(vptr->l_volume) * uint64_t(vptr->env_volume))) >> 24);
            out[j * 2 + 1] += (int32_t)((fout * (int64_t)(uint64_t(vptr->r_volume) * uint64_t(vptr->env_volume))) >> 24);
            // Individual outputs (rear / subwoofer sends) aren't mixed into the stereo pair.
        }
        vptr->pos = pos;
        vptr->frac = frac;
    }
}

void L7A1045::io_write(uint32_t offset, uint16_t data, uint16_t mem_mask) {
    switch (offset & 0xe) {
    case 0x0: voice_select_w(data, mem_mask); break;
    case 0x2: case 0x4: case 0x6: voiceregs_w((int)((offset & 0xe) - 2) / 2, data); break;
    case 0x8: control_w(data); break;
    case 0xc: atomic_w(data); break;
    default: break;
    }
}

uint16_t L7A1045::io_read(uint32_t offset) {
    switch (offset & 0xe) {
    case 0x2: case 0x4: case 0x6: return voiceregs_r((int)((offset & 0xe) - 2) / 2);
    case 0x8: return m_control;
    default: return 0;
    }
}

void L7A1045::voice_select_w(uint16_t data, uint16_t mem_mask) {
    if (mem_mask & 0x00ff) m_cur_channel = (uint8_t)(data & 0x1f);
    if (mem_mask & 0xff00) m_cur_register = (uint8_t)((data >> 8) & 0x0f);
}

uint16_t L7A1045::voiceregs_r(int offset) {
    const Voice* vptr = &m_voice[m_cur_channel];
    switch (m_cur_register) {
    case L6028_Start: {
        const uint32_t current_addr = vptr->start + vptr->pos;
        m_regs[0][m_cur_channel] &= 0xfff000000000ULL;
        m_regs[0][m_cur_channel] |= (uint64_t(current_addr) << 12);
        m_regs[0][m_cur_channel] |= vptr->frac & 0x0fff;
        break;
    }
    case L6028_Volume_Env:
        m_regs[L6028_Volume_Env][m_cur_channel] &= 0xffff0000ffffULL;
        m_regs[L6028_Volume_Env][m_cur_channel] |= (uint64_t(vptr->env_volume) << 16);
        break;
    case L6028_Filter_Env:
        m_regs[L6028_Filter_Env][m_cur_channel] &= 0xffff0000ffffULL;
        m_regs[L6028_Filter_Env][m_cur_channel] |= (uint64_t(vptr->flt_freq) << 16);
        break;
    }
    return (uint16_t)((m_regs[m_cur_register][m_cur_channel] >> (offset * 16)) & 0xffff);
}

void L7A1045::voiceregs_w(int offset, uint16_t data) {
    Voice* const vptr = &m_voice[m_cur_channel];
    const uint64_t offset_mask[3] = {0xffffffff0000ULL, 0xffff0000ffffULL, 0x0000ffffffffULL};
    uint64_t& r = m_regs[m_cur_register][m_cur_channel];
    r &= offset_mask[offset];
    r |= (uint64_t(data) << (offset * 16));

    switch (m_cur_register) {
    case L6028_Start:
        vptr->start = (uint32_t)((r >> 12) & 0x00ffffff);
        vptr->sample_type = (uint8_t)((r >> 36) & 0xf);
        vptr->pos = 0; vptr->frac = 0; vptr->history_count = 0; vptr->env_pos = 0;
        vptr->flt_pos = 0; vptr->l = vptr->b = 0;
        if (offset == 2) m_regs[L6028_Loop_Start][m_cur_channel] = 0;
        break;
    case L6028_End:
        vptr->end = (uint32_t)((r >> 12) & 0x00fffff0);
        vptr->step = (uint32_t)(m_regs[1][m_cur_channel] & 0xffff);
        if (offset == 2) recalc_loop_start(vptr);
        break;
    case L6028_Loop_Start:
        recalc_loop_start(vptr);
        break;
    case L6028_Volume_Env:
        vptr->env_volume = (uint16_t)((r & 0xffff0000) >> 16);
        vptr->env_pos = 0;
        if (offset == 0 && data == 0) m_key &= ~(1u << m_cur_channel);
        break;
    case L6028_Volume_Env_Target:
        vptr->env_target = (uint16_t)((r & 0xffff0000) >> 16);
        vptr->env_step = (uint16_t)(r & 0xffff);
        break;
    case L6028_Filter_Env:
        if (vptr->flt_pos == 0) vptr->flt_freq = (uint16_t)((r & 0xffff0000) >> 16);
        break;
    case L6028_Filter_Env_Target:
        vptr->flt_target = (uint16_t)((r & 0xfff00000) >> 16);
        vptr->flt_resonance = (uint8_t)((r & 0x000f0000) >> 16);
        vptr->flt_step = (uint16_t)(m_regs[6][m_cur_channel] & 0xffff);
        break;
    case L6028_Mixer_Params:
        vptr->r_volume = (uint16_t)(r & 0xff);
        vptr->l_volume = (uint16_t)((r >> 8) & 0xff);
        vptr->send_dest = (uint8_t)((r >> 16) & 0xff);
        vptr->send_level = (uint8_t)((r >> 24) & 0xff);
        break;
    }
}

void L7A1045::recalc_loop_start(Voice* vptr) {
    if ((m_regs[L6028_End][m_cur_channel] >> (8 + 32)) & 1) {
        const uint32_t length = vptr->end - vptr->start;
        vptr->loop_start = (uint32_t)((m_regs[L6028_Loop_Start][m_cur_channel] & 0xfffff000) >> 12);
        vptr->loop_start |= (uint32_t)((m_regs[L6028_Loop_Start][m_cur_channel] & 0x000f) << 20);
        vptr->loop_start = vptr->end - vptr->loop_start;
        if (vptr->loop_start > length) vptr->loop_start = length;
    } else {
        const uint32_t multiplier = (uint32_t)((((m_regs[L6028_Loop_Start][m_cur_channel] & 0xffff0000) >> 16) ^ 0xffff) + 1);
        const uint32_t base = (uint32_t)(m_regs[L6028_Loop_Start][m_cur_channel] & 0xffff);
        vptr->loop_start = (base * multiplier) >> 12;
    }
}

void L7A1045::control_w(uint16_t data) {
    m_control = data;
    if ((data >> CONTROL_KEY_ON) & 1) {
        Voice* const vptr = &m_voice[m_cur_channel];
        vptr->frac = 0; vptr->pos = 0; vptr->history_count = 0; vptr->flt_pos = 0;
        vptr->l = vptr->b = 0; vptr->env_pos = 0;
        m_key |= 1u << m_cur_channel;
        recalc_loop_start(vptr);
    }
    // DMA start/stop: the owner (sound.cpp) checks dma_running() and raises DRQ every 8 samples.
}

void L7A1045::atomic_w(uint16_t) { m_regs[m_cur_register][m_cur_channel] = 0; }

uint16_t L7A1045::dma_r16_cb() {
    const uint32_t byteoffs = (m_voice[0].start << 1) + (m_voice[0].pos << 1);
    if (drq_handler) drq_handler(false);
    m_voice[0].pos++;
    m_voice[0].history_count = 0;
    return (m_voice[0].sample_type == 1) ? rom_word(byteoffs & 0x1ffffff) : ram_word(byteoffs);
}

void L7A1045::dma_w16_cb(uint16_t data) {
    const uint32_t byteoffs = (m_voice[0].start << 1) + (m_voice[0].pos << 1);
    if (drq_handler) drq_handler(false);
    if (m_voice[0].sample_type == 1) { /* ROM: writes ignored */ }
    else ram_write_word(byteoffs, data);
    m_voice[0].pos++;
    m_voice[0].history_count = 0;
}
