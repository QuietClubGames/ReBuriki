// HNG64 I/O board: the TMP87PH40AN microcontroller (TLCS-870, 8 MHz) running its real firmware
// (hng64.zip: tmp87ph40an.bin). It reads the 8 input ports (selected via port 1), talks to the
// main CPU through the dual-port RAM at 0x1F808000 (via an address counter on port 7, data on
// port 0), and raises main-CPU interrupt level 17 (bit 0x20000).
//
// Wiring follows MAME src/mame/snk/hng64.cpp (ioport0/1/3/7 handlers, tempio irq timers).
#include "hw.h"
#include "input.h"
#include "iomcu/tlcs870.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

Tlcs870 g_mcu;
bool g_enabled = false;

uint8_t m_port1 = 0, m_port7 = 0;
uint16_t m_ex_ramaddr = 0, m_ex_ramaddr_upper = 0;
uint8_t* g_dpram = nullptr;                    // host-native words: byte a at [a ^ 3]

// Timing: the MCU gets 8 MHz worth of cycles per emulated second, converted from ticks.
constexpr uint64_t MCU_CYCLES_PER_FRAME = 134612;   // 8 MHz / 59.43 Hz
uint32_t g_ticks_per_frame = 50000;
uint64_t g_last_tick = 0, g_cycle_remainder = 0;
uint64_t g_int0_off_at = 0;
bool g_int0 = false;

uint8_t dp_read(uint16_t a) { return g_dpram[(a & 0x7ff) ^ 3]; }
void dp_write(uint16_t a, uint8_t v) { g_dpram[(a & 0x7ff) ^ 3] = v; }

void ioport7_w(uint8_t data) {
    m_ex_ramaddr_upper = (data & 0x0c) >> 2;
    if (!(data & 0x80) && (m_port7 & 0x80)) hw_raise_irq(0x00020000);   // MCU -> MIPS interrupt
    if (!(data & 0x01) && (m_port7 & 0x01)) m_ex_ramaddr = 0;            // address counter reset
    if (!(data & 0x02) && (m_port7 & 0x02)) m_ex_ramaddr = (m_ex_ramaddr + 1) & 0x1ff;  // clock
    m_port7 = data;
}

uint16_t dp_addr() { return (m_ex_ramaddr | (m_ex_ramaddr_upper << 9)) & 0x7ff; }

} // namespace

extern "C" void io_board_init(const char* rom_dir, uint32_t ticks_per_frame) {
    g_ticks_per_frame = ticks_per_frame;
    g_dpram = (uint8_t*)hw_phys_words(0x1F808000);
    std::string path = std::string(rom_dir) + "/hng64/tmp87ph40an.bin";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { fprintf(stderr, "[io-mcu] %s not found: controls disabled\n", path.c_str()); return; }
    std::vector<uint8_t> rom(0x8000);
    size_t n = fread(rom.data(), 1, rom.size(), f);
    fclose(f);
    if (n != rom.size()) { fprintf(stderr, "[io-mcu] %s is the wrong size: controls disabled\n", path.c_str()); return; }

    g_mcu.port_in[0] = [] { return dp_read(dp_addr()); };
    g_mcu.port_out[0] = [](uint8_t d) { dp_write(dp_addr(), d); };
    g_mcu.port_out[1] = [](uint8_t d) { m_port1 = d; };
    g_mcu.port_in[3] = [] { return input_current().port[(m_port1 & 0xe0) >> 5]; };
    g_mcu.port_out[3] = [](uint8_t) {};      // lamps / coin counters
    g_mcu.port_out[4] = [](uint8_t) {};      // unknown (serial-related)
    g_mcu.port_out[7] = [](uint8_t d) { ioport7_w(d); };
    g_mcu.load_rom(rom.data(), rom.size());
    g_mcu.reset();
    g_enabled = true;
    g_last_tick = g_ticks;
    fprintf(stderr, "[io-mcu] I/O board firmware running\n");
}

// Main CPU wrote the command trigger (0x1F7021C4): MAME pulses the MCU's INT0 for ~1000 cycles.
extern "C" void io_board_command_irq(void) {
    if (!g_enabled) return;
    g_mcu.set_input_line(0, true);
    g_int0 = true;
    g_int0_off_at = g_ticks + 32;
}

// Catch the MCU up to the current time. Called regularly from hw_events.
extern "C" void io_board_run(void) {
    if (!g_enabled) return;
    const uint64_t now = g_ticks;
    const uint64_t elapsed = now - g_last_tick;
    g_last_tick = now;
    const uint64_t num = elapsed * MCU_CYCLES_PER_FRAME + g_cycle_remainder;
    const int cycles = (int)(num / g_ticks_per_frame);
    g_cycle_remainder = num % g_ticks_per_frame;
    if (cycles > 0) g_mcu.run(cycles);
    if (g_int0 && now >= g_int0_off_at) { g_mcu.set_input_line(0, false); g_int0 = false; }
}
