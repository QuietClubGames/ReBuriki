// HNG64 sound board: NEC V53A (32 MHz) running the sound driver the game uploads, driving the
// L7A1045 sample chip. Wiring follows MAME src/mame/snk/hng64_a.cpp (LGPL-2.1+).
//
// Memory: the V53 sees 16 banks of 64 KB, each pointing into the 2 MB sound RAM that the main
// CPU fills at 0x60200000 (MAME m_soundram). The byte order works out so V53 byte address B is
// main-CPU byte 0x60200000 + B. Banks reset to entry 0x1F; I/O ports 0x200-0x21F change them.
//
// Timing: everything is derived from the deterministic tick clock. The V53 gets 32 MHz worth
// of cycles, and the sound chip's 44.1 kHz output is derived from the V53's own cycle count,
// so chip register writes happen at the right sample.
#include "hw.h"
#include "l7a1045.h"
#include "audio_out.h"
#include "audiocpu/v53a.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "savestate.h"
#include "netplay.h"
#include <chrono>
#include <vector>

namespace {

v53::V53A g_cpu(32000000);
L7A1045 g_dsp;
bool g_enabled = false;
bool g_running = false;        // sound CPU released from reset by the main CPU (0x55AA)

uint8_t* g_soundram = nullptr;  // host-native words, byte b at [b ^ 3]
uint8_t m_bank[16];

uint16_t main_latch[2] = {}, sound_latch[2] = {};

// Timing
constexpr uint64_t V53_CYCLES_PER_FRAME = 538449;      // 32 MHz / 59.43 Hz
constexpr double SAMPLES_PER_CYCLE = 44100.0 / 32000000.0;
uint32_t g_ticks_per_frame = 50000;
uint64_t g_last_tick = 0, g_cycle_remainder = 0;
// Sound-board time in V53 clocks, continuous across the CPU being held in reset:
// while running it's g_run_base_time + (cpu.total_cycles() - g_run_base_cpu).
uint64_t g_cpu_cycles = 0;          // time while held
uint64_t g_run_base_time = 0, g_run_base_cpu = 0;
uint64_t g_samples_done = 0;        // sound chip samples generated so far
// MAME: while the chip's DMA is started, a timer raises DRQ every 64 chip clocks
// (33.8688 MHz), i.e. every 60.47 V53 clocks. Tracked in 1/1000ths of a V53 clock.
constexpr uint64_t DRQ_PERIOD_MILLICYCLES = 60469;   // 64 * 32e6 / 33.8688e6 * 1000
uint64_t g_next_drq_millicycle = 0;
std::vector<int16_t> g_out;         // samples waiting to be pushed to audio_out (stereo)
uint64_t g_dma_words = 0, g_keyons = 0;

// Debug logging: set env BURIKI_LOGSND=1.
bool g_log = getenv("BURIKI_LOGSND") != nullptr;
int g_log_count = 0;
#define SLOG(...) do { if (g_log && g_log_count++ < 4000) { fprintf(stderr, "[snd %u] ", hw_frame_number()); fprintf(stderr, __VA_ARGS__); } } while (0)

inline uint32_t soundram_offset(uint32_t addr) {
    addr &= 0xFFFFF;
    return ((uint32_t)m_bank[addr >> 16] << 16) | (addr & 0xFFFF);
}
inline uint8_t sr8(uint32_t off) { return g_soundram[(off & 0x1FFFFF) ^ 3]; }
inline void sw8(uint32_t off, uint8_t v) { g_soundram[(off & 0x1FFFFF) ^ 3] = v; }

// Current time in V53 cycles (inside run() the core knows the exact cycle).
uint64_t now_cycles() { return g_running ? g_run_base_time + (g_cpu.total_cycles() - g_run_base_cpu) : g_cpu_cycles; }

// Diagnostics (env BURIKI_SNDPROF): where the sound board's time goes, printed every 600 frames.
struct SndProf {
    bool on = getenv("BURIKI_SNDPROF") != nullptr;
    uint64_t runs = 0, run_calls = 0, gen_calls = 0, samples = 0, cycles = 0, drq = 0;
    double cpu_s = 0, dsp_s = 0;
} g_prof;
struct ProfTimer {
    double* acc; std::chrono::steady_clock::time_point t0;
    explicit ProfTimer(double& a) : acc(g_prof.on ? &a : nullptr) { if (acc) t0 = std::chrono::steady_clock::now(); }
    ~ProfTimer() { if (acc) *acc += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
};

// Generate sound chip output up to the given V53 cycle count (MAME stream->update()).
void dsp_update_to(uint64_t cycles) {
    const uint64_t target = (uint64_t)((double)cycles * SAMPLES_PER_CYCLE);
    while (g_samples_done < target) {
        uint64_t chunk = std::min<uint64_t>(target - g_samples_done, 256);
        int32_t buf[512];
        g_prof.gen_calls++; g_prof.samples += chunk;
        ProfTimer pt(g_prof.dsp_s);
        g_dsp.generate(buf, (int)chunk);
        for (uint64_t i = 0; i < chunk * 2; i++) g_out.push_back((int16_t)std::clamp(buf[i], -32768, 32767));
        g_samples_done += chunk;
    }
}

bool g_command_taken = false;      // the V53 has acknowledged the command interrupt (sound_inject_command)
uint16_t g_replay_latch[2] = {};   // what the game has in the command latch during a netplay rollback

void sound_comms_w(uint32_t offset, uint16_t data) {
    SLOG("V53 writes comms %X = %04X\n", offset, data);
    switch (offset) {
    case 0x0: sound_latch[0] = data; break;
    case 0x2: sound_latch[1] = data; break;
    case 0xa: g_cpu.set_input_line(v53::V53A::LINE_IRQ5, false); g_command_taken = true; break;
    }
}
uint16_t sound_comms_r(uint32_t offset) {
    switch (offset) {
    case 0x0: return sound_latch[0];
    case 0x2: return sound_latch[1];
    case 0x4: return main_latch[0];
    case 0x6: return main_latch[1];
    }
    return 0;
}

void io_w16(uint16_t port, uint16_t data, uint16_t mask) {
    if (port < 0x10) {
        dsp_update_to(now_cycles());
        if ((port & 0xe) == 0x8) { if (data & 0x100) g_keyons++; SLOG("DSP control %04X\n", data); }
        g_dsp.io_write(port, data, mask);
    }
    else if (port == 0x80) SLOG("port 80 = %04X\n", data);
    else if (port >= 0x100 && port < 0x110) sound_comms_w(port & 0xe, data);
    else if (port >= 0x200 && port < 0x220) m_bank[(port >> 1) & 0xf] = data & 0x1f;   // MAME sound_bank_w
    // 0x80: unknown (MAME logs it)
}
uint16_t io_r16(uint16_t port) {
    if (port < 0x10) { dsp_update_to(now_cycles()); return g_dsp.io_read(port); }
    if (port >= 0x100 && port < 0x110) return sound_comms_r(port & 0xe);
    return 0;
}

void set_running(bool on) {
    if (on == g_running) return;
    if (on) {
        // MAME: HALT and RESET released together -> fresh start.
        g_cpu.set_halt_line(false);
        g_cpu.set_reset_line(false);
        g_run_base_time = g_cpu_cycles;
        g_run_base_cpu = g_cpu.total_cycles();
        g_running = true;
    } else {
        g_cpu_cycles = now_cycles();
        g_cpu.set_halt_line(true);
        g_cpu.set_reset_line(true);
        g_running = false;
    }
}

} // namespace

extern "C" void sound_init(const char* rom_dir, uint32_t ticks_per_frame, int play, const char* wav_path) {
    g_ticks_per_frame = ticks_per_frame;
    g_soundram = (uint8_t*)hw_phys_words(0x60200000);
    std::vector<uint8_t> rom(0x1000000);
    const char* sd[4] = {"007sd01a.77", "007sd02a.78", "007sd03a.79", "007sd04a.80"};
    for (int i = 0; i < 4; i++) {
        std::string path = std::string(rom_dir) + "/buriki/" + sd[i];
        FILE* f = fopen(path.c_str(), "rb");
        if (!f || fread(&rom[i * 0x400000], 1, 0x400000, f) != 0x400000) {
            if (f) fclose(f);
            fprintf(stderr, "[sound] can't read %s: sound disabled\n", path.c_str());
            return;
        }
        fclose(f);
    }
    g_dsp.init(std::move(rom));
    g_dsp.drq_handler = [](bool s) { if (!s) g_cpu.dreq(3, false); };

    auto& cb = g_cpu.cb;
    cb.mem_read8 = [](uint32_t a) { return sr8(soundram_offset(a)); };
    cb.mem_write8 = [](uint32_t a, uint8_t v) { sw8(soundram_offset(a), v); };
    cb.mem_read16 = [](uint32_t a) { uint32_t o = soundram_offset(a); return (uint16_t)(sr8(o) | sr8(o + 1) << 8); };
    cb.mem_write16 = [](uint32_t a, uint16_t v) { uint32_t o = soundram_offset(a); sw8(o, (uint8_t)v); sw8(o + 1, (uint8_t)(v >> 8)); };
    cb.io_read16 = [](uint16_t p) { return io_r16(p); };
    cb.io_write16 = [](uint16_t p, uint16_t v) { io_w16(p, v, 0xffff); };
    cb.io_read8 = [](uint16_t p) { uint16_t w = io_r16(p & ~1); return (uint8_t)((p & 1) ? w >> 8 : w); };
    cb.io_write8 = [](uint16_t p, uint8_t v) { io_w16(p & ~1, (p & 1) ? (uint16_t)(v << 8) : v, (p & 1) ? 0xff00 : 0x00ff); };
    // DMA. MAME binds memory reads (in_memr) and channel-3 16-bit I/O to the sound chip. We also
    // bind 16-bit memory access so 16-bit transfers move real data (MAME's unbound callback
    // would give 0).
    cb.dma_mem_read8 = [](uint32_t a) { return sr8(soundram_offset(a)); };
    cb.dma_mem_read16 = [](uint32_t off) { uint32_t o = soundram_offset(off << 1); return (uint16_t)(sr8(o) | sr8(o + 1) << 8); };
    cb.dma_mem_write16 = [](uint32_t off, uint16_t v) { uint32_t o = soundram_offset(off << 1); sw8(o, (uint8_t)v); sw8(o + 1, (uint8_t)(v >> 8)); };
    cb.dma_io_write16 = [](int ch, uint32_t, uint16_t v) { if (ch == 3) { g_dma_words++; g_dsp.dma_w16_cb(v); } };
    cb.dma_io_read16 = [](int ch, uint32_t) { return ch == 3 ? g_dsp.dma_r16_cb() : (uint16_t)0; };
    cb.dma_hreq = [](int) { g_cpu.hack_w(1); };
    cb.tout = [](int timer, int state) { if (timer == 2) g_cpu.set_input_line(v53::V53A::LINE_IRQ2, state != 0); };

    for (auto& b : m_bank) b = 0x1f;
    // The same mapping as the mem_* callbacks above, done inside the core (env BURIKI_V53_CALLBACKS=1 goes
    // back to the callbacks, to compare the two).
    if (!getenv("BURIKI_V53_CALLBACKS")) g_cpu.set_fast_memory(g_soundram, m_bank, 0x1FFFFF);
    g_cpu.reset();
    g_cpu.set_halt_line(true);
    g_cpu.set_reset_line(true);
    g_running = false;
    g_last_tick = g_ticks;
    audio_out_init(44100, play != 0, wav_path);
    g_enabled = true;
    fprintf(stderr, "[sound] sound board ready (CPU held in reset until the game starts it)\n");
}

// Catch the sound board up to the current time. Called regularly from hw_events.
// Experiments on how much the game depends on the sound board (see NOTES.md, "Sound board and rollback"):
//   BURIKI_SOUND_FREEZE=N   from frame N the board stops: no CPU, no chip, its replies never change again.
// And the save-state test's "nosound" option leaves the board out of the replays (savestate_skip_sound):
// it is not run, and what the game sends it is dropped (it was sent the first time those frames played).
static uint32_t sound_freeze_frame() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("BURIKI_SOUND_FREEZE"); v = e ? atoi(e) : 0; }
    return (uint32_t)v;
}
static bool sound_skipped() {
    return savestate_skip_sound() || (sound_freeze_frame() && hw_frame_number() >= sound_freeze_frame());
}
extern "C" void sound_run(void) {
    if (!g_enabled) return;
    if (sound_skipped()) { g_last_tick = g_ticks; return; }
    g_prof.runs++;
    const uint64_t now = g_ticks;
    const uint64_t elapsed = now - g_last_tick;
    g_last_tick = now;
    const uint64_t num = elapsed * V53_CYCLES_PER_FRAME + g_cycle_remainder;
    const uint64_t cycles = num / g_ticks_per_frame;
    g_cycle_remainder = num % g_ticks_per_frame;
    if (!cycles) return;
    if (g_running) {
        // run() overshoots by up to one instruction; the core carries that into the next call.
        const uint64_t target = now_cycles() + cycles;
        while (now_cycles() < target) {
            uint64_t step = std::min<uint64_t>(target - now_cycles(), 2048);
            if (g_dsp.dma_running()) {
                const uint64_t now_m = now_cycles() * 1000;
                if (g_next_drq_millicycle <= now_m) {
                    g_prof.drq++;
                    g_cpu.dreq(3, true);
                    g_next_drq_millicycle = std::max(g_next_drq_millicycle + DRQ_PERIOD_MILLICYCLES, now_m);
                }
                step = std::min<uint64_t>(step, (g_next_drq_millicycle - now_m + 999) / 1000);
                if (step == 0) step = 1;
            }
            g_prof.run_calls++; g_prof.cycles += step;
            { ProfTimer pt(g_prof.cpu_s); g_cpu.run((int)step); }
            dsp_update_to(now_cycles());
        }
    } else {
        g_cpu_cycles += cycles;
        dsp_update_to(g_cpu_cycles);
    }
}

// Once per frame: hand the generated audio to the output.
extern "C" void sound_frame(void) {
    if (!g_enabled) return;
    sound_run();
    if (g_log && hw_frame_number() % 30 == 0)
        fprintf(stderr, "[snd %u] status: V53 %04X:%04X, key-ons %llu, DMA words %llu, dsp dma %d, latches main %04X %04X sound %04X %04X\n",
                hw_frame_number(), g_cpu.cs(), g_cpu.ip(), (unsigned long long)g_keyons, (unsigned long long)g_dma_words,
                (int)g_dsp.dma_running(), main_latch[0], main_latch[1], sound_latch[0], sound_latch[1]);
    if (!g_out.empty()) audio_out_push(g_out.data(), (int)(g_out.size() / 2));
    g_out.clear();
    if (g_prof.on && hw_frame_number() % 600 == 0) {
        const double f = 600.0;
        fprintf(stderr, "[sndprof] per frame: sound_run %.0f calls, cpu.run %.0f calls / %.0f clocks / %.3f ms, "
                        "dsp %.0f calls / %.0f samples / %.3f ms, DMA requests %.0f\n",
                g_prof.runs / f, g_prof.run_calls / f, g_prof.cycles / f, g_prof.cpu_s * 1000 / f,
                g_prof.gen_calls / f, g_prof.samples / f, g_prof.dsp_s * 1000 / f, g_prof.drq / f);
        fprintf(stderr, "[sndprof] per frame: %.0f instructions, %.0f slices, %.0f timer/DMA events, %.0f halted slices\n",
                g_cpu.stat_instr / f, g_cpu.stat_slices / f, g_cpu.stat_events / f, g_cpu.stat_halted / f);
        fprintf(stderr, "[sndprof] events per frame: interrupt controller %.0f, timers %.0f, DMA %.0f, serial clock %.0f\n",
                g_cpu.stat_kind[0] / f, g_cpu.stat_kind[1] / f, g_cpu.stat_kind[2] / f, g_cpu.stat_kind[3] / f);
        for (auto& k : g_cpu.stat_kind) k = 0;
        g_cpu.stat_instr = g_cpu.stat_slices = g_cpu.stat_events = g_cpu.stat_halted = 0;
        const bool on = g_prof.on;
        g_prof = SndProf();
        g_prof.on = on;
    }
}

// Main CPU side, 0x68000000-0x6800000F (MAME main_sound_comms_r/w) and 0x6F000000.
extern "C" uint32_t sound_main_read(uint32_t pa) {
    sound_run();
    if (pa == 0x68000000) return (uint32_t)main_latch[0] << 16 | main_latch[1];
    if (pa == 0x68000004) return (uint32_t)sound_latch[0] << 16 | sound_latch[1];
    return 0;
}

extern "C" void sound_main_write(uint32_t pa, uint32_t v, uint32_t mask) {
    if (!g_enabled) return;
    sound_run();
    if (savestate_skip_sound()) {
        // A rollback is playing frames again and the board is not part of it. Online play still needs to
        // know which commands the game sends this time round (netplay.cpp decides what to do with them).
        if (netplay_active()) {
            if (pa == 0x68000000) {
                if (mask & 0xffff0000u) g_replay_latch[0] = (uint16_t)(v >> 16);
                if (mask & 0x0000ffffu) g_replay_latch[1] = (uint16_t)v;
            } else if (pa == 0x68000008 && (mask & 0xffff0000u) && ((v >> 16) & 1)) {
                netplay_sound_command((uint32_t)g_replay_latch[0] << 16 | g_replay_latch[1]);
            }
        }
        return;
    }
    {   // BURIKI_LOGCMD=N: what the game sends the board, from frame N (600 lines)
        static int from = -1, left = 600;
        if (from < 0) { const char* e = getenv("BURIKI_LOGCMD"); from = e ? atoi(e) : 0; }
        if (from && (int)hw_frame_number() >= from && left > 0) {
            left--;
            fprintf(stderr, "[cmd %u] %08X = %08X (mask %08X)\n", hw_frame_number(), pa, v, mask);
        }
    }
    SLOG("main writes %08X = %08X (mask %08X)\n", pa, v, mask);
    if (pa == 0x68000000) {
        if (mask & 0xffff0000u) main_latch[0] = (uint16_t)(v >> 16);
        if (mask & 0x0000ffffu) main_latch[1] = (uint16_t)v;
        g_replay_latch[0] = main_latch[0]; g_replay_latch[1] = main_latch[1];
    } else if (pa == 0x68000008) {
        if ((mask & 0xffff0000u) && ((v >> 16) & 1)) {
            if (g_running) g_cpu.set_input_line(v53::V53A::LINE_IRQ5, true);
            if (netplay_active()) netplay_sound_command((uint32_t)main_latch[0] << 16 | main_latch[1]);
        }
    } else if (pa == 0x6F000000 && (mask & 0xffff0000u)) {
        const uint16_t cmd = (uint16_t)(v >> 16);
        if (cmd == 0x55AA) set_running(true);
        else if (cmd == 0xAA55) set_running(false);
    }
}

// Online play: a command the game turned out to send in a frame that a rollback played again, and which the
// board was never given. It is handed over now (late by the depth of the rollback): latch, interrupt, and
// the CPU runs until it has taken it, so that several can be given one after another.
void sound_inject_command(uint32_t latch) {
    if (!g_enabled || !g_running) return;
    main_latch[0] = (uint16_t)(latch >> 16); main_latch[1] = (uint16_t)latch;
    g_command_taken = false;
    g_cpu.set_input_line(v53::V53A::LINE_IRQ5, true);
    for (int i = 0; i < 400 && !g_command_taken; i++) {
        g_cpu.run(64);
        dsp_update_to(now_cycles());
    }
}

extern "C" void sound_report(void) {
    if (!g_enabled) return;
    fprintf(stderr, "[sound] V53 at %04X:%04X, %llu cycles, %llu samples, %llu key-ons, %llu DMA words, %llu log events%s%s\n",
            g_cpu.cs(), g_cpu.ip(), (unsigned long long)g_cpu.total_cycles(), (unsigned long long)g_samples_done,
            (unsigned long long)g_keyons, (unsigned long long)g_dma_words, (unsigned long long)g_cpu.log_count(),
            g_cpu.log_count() ? ", last: " : "", g_cpu.log_count() ? g_cpu.last_log().c_str() : "");
    audio_out_close();
}

// Save state (savestate.h). The two chip objects are copied as raw bytes: their callbacks and buffer
// pointers don't change after start-up, so the bytes put back are the ones that were there. The chip's
// sample RAM lives outside the object and is copied separately. Sound waiting to be played is dropped
// on a load. A proper netplay build should save these field by field instead.
void sound_state(StateIO& io) {
    g_cpu.state_prepare();
    io.raw(&g_cpu, sizeof g_cpu);
    io.raw(&g_dsp, sizeof g_dsp);
    io.raw(g_dsp.state_ram().data(), g_dsp.state_ram().size());
    io.pod(g_enabled); io.pod(g_running); io.pod(m_bank); io.pod(main_latch); io.pod(sound_latch);
    io.pod(g_last_tick); io.pod(g_cycle_remainder); io.pod(g_cpu_cycles);
    io.pod(g_run_base_time); io.pod(g_run_base_cpu); io.pod(g_samples_done); io.pod(g_next_drq_millicycle);
    io.pod(g_dma_words); io.pod(g_keyons);
    if (!io.saving) g_out.clear();
}
