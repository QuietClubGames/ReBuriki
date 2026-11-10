// Hyper Neo Geo 64 hardware layer: memory map, registers, DMA, interrupts, timing.
// Reference: MAME src/mame/snk/hng64.cpp (main_map, sysregs, irqc, dmac, dualport, rtc).
#include "hw.h"
#include "input.h"
#include "video.h"
#include "menu_art.h"
#include "savestate.h"
#include "netplay.h"
#include "pagestate.h"
#include "funcs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <vector>

extern "C" {
uint8_t* g_ram = nullptr;
uint64_t g_ticks = 0;
uint64_t g_next_event = 1;
const char* g_func_ring[HW_FUNC_RING] = {};
uint32_t g_func_ring_pos = 0;
uint32_t g_cop0_status = 0;
}

namespace {

// ---- physical memory regions -----------------------------------------------------------
struct Region {
    uint32_t base, size;
    uint8_t* data;          // host-native 32-bit words (byte A at data[A ^ 3])
    bool read_only;
    const char* name;
};
std::vector<Region> g_regions;

uint8_t* add_region(uint32_t base, uint32_t size, bool ro, const char* name, uint8_t* data = nullptr) {
    // The large areas the game writes to are watched for writes, so that online play's save states only
    // copy the pages that changed (pagestate.h). The sound board's memory is not part of those states.
    const bool big = !ro && (base == 0x00000000 || base == 0x20000000 || base == 0x20100000 || base == 0x30000000);
    if (!data) data = big ? pagestate_alloc(size) : (uint8_t*)calloc(1, size);
    g_regions.push_back({base, size, data, ro, name});
    return data;
}

Region* find_region(uint32_t pa) {
    for (auto& r : g_regions)
        if (pa - r.base < r.size) return &r;
    return nullptr;
}

// ---- device state ---------------------------------------------------------------------
struct Tlb { bool valid; uint32_t mask, hi, p0, p1; };
Tlb g_tlb[48];

HwConfig g_cfg;
uint32_t g_irq_pending = 0;
uint32_t g_irq_level = 0;
uint32_t g_dma_src = 0, g_dma_dst = 0, g_dma_len = 0;
uint32_t g_mcu_en = 0;
bool g_in_vblank = false;
uint32_t g_frame = 0;
uint64_t g_next_vblank = 0, g_vblank_end = 0, g_fifo_irq_at = UINT64_MAX;
uint64_t g_next_mcu = 0;
double g_perf_video = 0, g_perf_sound = 0, g_perf_io = 0;   // seconds spent, for [perf] lines
struct PerfTimer {
    double& acc; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit PerfTimer(double& a) : acc(a) {}
    ~PerfTimer() { acc += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
};
constexpr uint64_t MCU_SLICE_TICKS = 256;   // how often the I/O MCU is caught up
bool g_in_irq = false;
uint64_t g_irq_count[32] = {};

// ---- access logging --------------------------------------------------------------------
struct IoStat { uint64_t reads = 0, writes = 0; uint32_t last_write = 0; const char* first_func = ""; };
std::map<uint32_t, IoStat> g_io;
int g_io_printed = 0;

const char* cur_func() {
    const char* f = g_func_ring[(g_func_ring_pos - 1) & (HW_FUNC_RING - 1)];
    return f ? f : "?";
}

const char* region_name(uint32_t pa) {
    if (pa - 0x1F700000u < 0x1100) return "sysregs";
    if (pa - 0x1F701100u < 0x20) return "irq ctrl";
    if (pa - 0x1F701200u < 0x80) return "dma";
    if (pa - 0x1F702100u < 0x80) return "rtc";
    if (pa == 0x1F7021C4u) return "io-mcu irq";
    Region* r = find_region(pa);
    return r ? r->name : "UNMAPPED";
}

// Big RAM-like areas (video RAM, sound RAM, ROM...) are tracked per area, registers per address.
bool is_bulk(uint32_t pa, Region** out) {
    Region* r = find_region(pa);
    *out = r;
    return r && r->size >= 0x4000 && pa - 0x1F700000u >= 0x2200;
}

void log_io(uint32_t va, uint32_t pa, bool write, uint32_t value, int size) {
    Region* r;
    uint32_t key = is_bulk(pa, &r) ? r->base : pa;
    IoStat& s = g_io[key];
    bool first = (write ? s.writes : s.reads) == 0;
    if (write) { s.writes++; s.last_write = value; } else s.reads++;
    if (!*s.first_func) s.first_func = cur_func();
    if (g_cfg.trace_io || (first && g_io_printed < g_cfg.io_log_limit)) {
        if (!g_cfg.trace_io) g_io_printed++;
        fprintf(stderr, "[io] %s%d %08X (phys %08X, %s)%s%08X  in %s  frame %u\n",
                write ? "W" : "R", size * 8, va, pa, region_name(pa),
                write ? " = " : "", write ? value : 0, cur_func(), g_frame);
    }
}

// ---- interrupts -------------------------------------------------------------------------
void update_irq_level() {
    for (int i = 0; i < 31; i++)
        if (g_irq_pending & (1u << i)) { g_irq_level = (uint32_t)i; return; }
}

void raise_irq(uint32_t bits) {
    g_irq_pending |= bits;
    update_irq_level();
    g_next_event = g_ticks + 1;  // try to deliver at the next safe point
}

// ---- address translation ----------------------------------------------------------------
bool translate(uint32_t va, uint32_t& pa) {
    if ((va >> 30) == 2u) { pa = va & 0x1FFFFFFFu; return true; }   // KSEG0 / KSEG1
    for (const Tlb& t : g_tlb) {
        if (!t.valid) continue;
        uint32_t page = ((t.mask >> 13) + 1) * 0x1000u;   // size of one of the two pages
        uint32_t pair_mask = ~(page * 2 - 1);
        if ((va & pair_mask) == (t.hi & pair_mask)) {
            pa = ((va & page) ? t.p1 : t.p0) + (va & (page - 1));
            return true;
        }
    }
    return false;
}

// ---- register hooks ---------------------------------------------------------------------
void do_dma() {
    // MAME do_dma: copies (len + 1) words from start to dst, both physical.
    uint32_t words = g_dma_len + 1;
    Region* s = find_region(g_dma_src);
    Region* d = find_region(g_dma_dst);
    fprintf(stderr, "[dma] %08X -> %08X, %u words  (in %s)\n", g_dma_src, g_dma_dst, words, cur_func());
    if (s && d && g_dma_src - s->base + words * 4 <= s->size && g_dma_dst - d->base + words * 4 <= d->size) {
        memcpy(d->data + (g_dma_dst - d->base), s->data + (g_dma_src - s->base), (size_t)words * 4);
    } else {
        hw_fatal("DMA outside known memory: %08X -> %08X (%u words)", g_dma_src, g_dma_dst, words);
    }
    g_dma_src += words * 4;
    g_dma_dst += words * 4;
}

// The board's clock/calendar chip (Epson RTC62423, MSM6242-compatible): sixteen 4-bit registers.
// 0-B: seconds, minutes, hours, day, month, year as two decimal digits each (units first);
// C: day of the week (0 = Sunday); D-F: control (F bit 2 = 24-hour mode).
// With a window it shows the PC's clock, read once per frame so the digits belong together.
// Headless runs (and BURIKI_RTC=0) keep the old all-zero clock, so test runs don't depend on
// the time of day. The game only uses it for the operator's bookkeeping dates and the calendar
// screen; writes (the Test menu's CALENDAR SETTING) are ignored.
int rtc_read(int reg) {
    static const bool real = [] {
        const char* e = getenv("BURIKI_RTC");
        return e ? atoi(e) != 0 : !g_video_cfg.headless;
    }();
    if (!real) return 0;
    static uint32_t frame = ~0u;
    static struct tm now;
    if (frame != g_frame) {
        frame = g_frame;
        if (netplay_active()) {
            // online: both games must read the same clock, so it is the host's at the start plus game time
            const time_t t = (time_t)netplay_clock(g_frame);
            if (const struct tm* gt = gmtime(&t)) now = *gt;
        } else {
            const time_t t = time(nullptr);
            if (const struct tm* lt = localtime(&t)) now = *lt;
        }
    }
    const int year = now.tm_year % 100, month = now.tm_mon + 1;
    switch (reg) {
    case 0x0: return now.tm_sec % 10;   case 0x1: return now.tm_sec / 10;
    case 0x2: return now.tm_min % 10;   case 0x3: return now.tm_min / 10;
    case 0x4: return now.tm_hour % 10;  case 0x5: return now.tm_hour / 10;
    case 0x6: return now.tm_mday % 10;  case 0x7: return now.tm_mday / 10;
    case 0x8: return month % 10;        case 0x9: return month / 10;
    case 0xA: return year % 10;         case 0xB: return year / 10;
    case 0xC: return now.tm_wday;
    case 0xF: return 4;                 // 24-hour mode
    default: return 0;                  // D, E: not busy, no interrupt
    }
}

// Returns true and sets v if the register has special read behaviour.
bool read_hook(uint32_t pa, uint32_t& v) {
    switch (pa) {
    case 0x1F70001C: v = 0; return true;                   // bit 0x40 must be clear or games hang
    case 0x1F701084: v = 2; return true;                   // MCU -> MIPS latch
    case 0x1F701104: v = g_irq_level; return true;         // current irq level
    case 0x1F701254: v = 0; return true;                   // DMA status: idle
    case 0x20208048: v = g_in_vblank ? 1 : 0; return true; // tcram: vblank status
    case 0x20300218: v = 0; return true;                   // 3D display list status: ready
    case 0x68000000: case 0x68000004: case 0x68000008: case 0x6800000C:
        v = sound_main_read(pa); return true;              // main <-> sound CPU latches
    }
    if (pa - 0x1F702100u < 0x80) {                         // RTC: 1 nibble in every 8 bytes
        if ((pa >> 2) & 1) {
            const int reg = ((pa - 0x1F702100u) >> 3) & 0xF;
            // bit 4 on register D: MAME's "disables the system log reader" bit (not from the chip)
            v = (uint32_t)rtc_read(reg) | (reg == 0xD ? 0x10 : 0);
        }
        else v = 0xFFFFFFFF;
        return true;
    }
    if (pa == 0x1F808600 && g_mcu_en != 0x0C) {
        // MAME hack: the I/O board's "machine code" lives at dualport 0x600. 1 = fighting board.
        Region* r = find_region(pa);
        v = (*(uint32_t*)(r->data + (pa - r->base)) & 0x00FFFFFFu) | 0x01000000u;
        return true;
    }
    return false;
}

// MAME sprite_clear_even_w / sprite_clear_odd_w: writes here zero parts of two sprite entries.
void sprite_clear(uint32_t pa, uint32_t mask, bool odd) {
    uint32_t* spr = hw_phys_words(0x20000000);
    uint32_t base = odd ? 0x2000E400u : 0x2000D800u;
    uint32_t spr_offs = ((pa - base) / 4) * 0x40;
    auto clr = [&](uint32_t byte_off) { if (spr_offs + byte_off < 0xC000) spr[(spr_offs + byte_off) / 4] = 0; };
    if (!odd) {
        if (mask & 0xFFFF0000u) for (uint32_t o : {0x00u, 0x08u, 0x10u, 0x18u}) clr(o);
        if (mask & 0x0000FF00u) for (uint32_t o : {0x20u, 0x28u, 0x30u, 0x38u}) clr(o);
    } else {
        if (mask & 0xFFFF0000u) for (uint32_t o : {0x04u, 0x14u, 0x1Cu}) clr(o);
        if (mask & 0x0000FFFFu) for (uint32_t o : {0x24u, 0x34u, 0x3Cu}) clr(o);
    }
}

void write_hook(uint32_t pa, uint32_t v, uint32_t mask) {
    if (pa - 0x2000D800u < 0xC00) { sprite_clear(pa, mask, false); return; }
    if (pa - 0x2000E400u < 0xC00) { sprite_clear(pa, mask, true); return; }
    switch (pa) {
    case 0x1F70111C: g_irq_pending &= ~(v & mask); update_irq_level(); break;   // irq ack
    case 0x1F701204: g_dma_src = v; break;
    case 0x1F701214: g_dma_dst = v; break;
    case 0x1F701224: g_dma_len = v; do_dma(); break;
    case 0x1F701084: g_mcu_en = v & 0xFF; break;
    case 0x1F7021C4:   // MIPS -> I/O MCU command interrupt (MAME mips_to_iomcu_irq_w)
        if (mask & 0xFFFF0000u) { io_board_run(); io_board_command_irq(); }
        break;
    case 0x20300200: video3d_dl_upload(v); g_fifo_irq_at = g_ticks + 200; if (g_next_event > g_fifo_irq_at) g_next_event = g_fifo_irq_at; break;
    case 0x6F000000:
        if (mask & 0xFFFF0000u) fprintf(stderr, "[sound] cpu %s (%04X) in %s, frame %u\n",
                                        (v >> 16) == 0x55AA ? "ON" : (v >> 16) == 0xAA55 ? "OFF" : "?",
                                        v >> 16, cur_func(), g_frame);
        sound_main_write(pa, v, mask);
        break;
    case 0x68000000: case 0x68000008:
        sound_main_write(pa, v, mask);
        break;
    }
}

// ---- physical access ----------------------------------------------------------------------
uint32_t phys_read32(uint32_t pa) {
    uint32_t v;
    if (read_hook(pa, v)) return v;
    if (Region* r = find_region(pa)) return *(uint32_t*)(r->data + (pa - r->base));
    return 0xFFFFFFFFu;
}

void phys_write(uint32_t pa, uint32_t v, int size) {
    Region* r = find_region(pa);
    if (r && !r->read_only) {
        uint8_t* p = r->data + (pa - r->base);
        uint32_t off = pa - r->base;
        if (size == 4) *(uint32_t*)p = v;
        else if (size == 2) *(uint16_t*)(r->data + (off ^ 2)) = (uint16_t)v;
        else r->data[off ^ 3] = (uint8_t)v;
    }
    uint32_t aligned = pa & ~3u;
    uint32_t shift = size == 4 ? 0 : size == 2 ? (16 * (1 - ((pa >> 1) & 1))) : (8 * (3 - (pa & 3)));
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((size == 2 ? 0xFFFFu : 0xFFu) << shift);
    write_hook(aligned, v << shift, mask);
}

// ---- "Unlock Silber" ---------------------------------------------------------------------
// The game keeps a 16-bit word of flags in its save memory (nvram + 0x20F0): bits 0-10 = "finished
// with fighter n", bit 13 = Silber selectable. It sets bit 13 itself once bits 0-10 are all set,
// or after the operator menu's hidden code (NOTES.md, "Silber"). The player select screen asks
// for bit 13 each time it starts.
// With the option on, the game always reads bit 13 as set. What is stored stays truthful: when
// the game writes the word back, bit 13 is stored only if it was already there or the eleven
// fighter bits are complete. So turning the option off returns to exactly what was earned.
// The game also caches the word in RAM (gp + 0x27C0, with 0x4000 = "loaded"); switching the option
// updates that copy too. Addresses are for Buriki One rev. B.
constexpr uint32_t SILBER_FLAGS_PA = 0x1F8020F0, SILBER_CACHE_RAM = 0x000F3990;
constexpr uint32_t SILBER_BIT = 0x2000, SILBER_ALL_FIGHTERS = 0x07FF, SILBER_CACHE_LOADED = 0x4000;
bool g_unlock_silber = false;

uint32_t access_read(uint32_t va, int size) {
    uint32_t pa;
    if (!translate(va, pa)) hw_fatal("read from unmapped address %08X (no TLB entry) in %s", va, cur_func());
    if (pa - 0x1F808000u < 0x800) io_board_run();   // let the I/O MCU catch up before sharing RAM
    uint32_t w = phys_read32(pa & ~3u);
    if (g_unlock_silber && (pa & ~3u) == SILBER_FLAGS_PA) w |= SILBER_BIT << 16;   // (the word's upper half)
    uint32_t v = size == 4 ? w
               : size == 2 ? (w >> (16 * (1 - ((pa >> 1) & 1)))) & 0xFFFF
                           : (w >> (8 * (3 - (pa & 3)))) & 0xFF;
    if (pa >= HW_RAM_SIZE) log_io(va, pa, false, v, size);
    return v;
}

void access_write(uint32_t va, uint32_t v, int size) {
    uint32_t pa;
    if (!translate(va, pa)) hw_fatal("write %08X to unmapped address %08X (no TLB entry) in %s", v, va, cur_func());
    if (pa - 0x1F808000u < 0x800) io_board_run();
    if (pa >= HW_RAM_SIZE) log_io(va, pa, true, v, size);
    if (g_unlock_silber && pa == SILBER_FLAGS_PA && size == 2) {
        // store bit 13 only if it was earned (see above)
        const uint32_t stored = phys_read32(SILBER_FLAGS_PA) >> 16;
        const bool earned = (stored & SILBER_BIT) || (v & SILBER_ALL_FIGHTERS) == SILBER_ALL_FIGHTERS;
        v = (v & ~SILBER_BIT) | (earned ? SILBER_BIT : 0);
    }
    phys_write(pa, v, size);
}

std::vector<uint8_t> read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) hw_fatal("can't open %s", path.c_str());
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> d((size_t)n);
    if (fread(d.data(), 1, d.size(), f) != d.size()) hw_fatal("can't read %s", path.c_str());
    fclose(f);
    return d;
}

} // namespace

// ---- public API -----------------------------------------------------------------------------
extern "C" {

uint32_t hw_read32(uint32_t a) { return access_read(a, 4); }
uint16_t hw_read16(uint32_t a) { return (uint16_t)access_read(a, 2); }
uint8_t  hw_read8(uint32_t a)  { return (uint8_t)access_read(a, 1); }
void hw_write32(uint32_t a, uint32_t v) { access_write(a, v, 4); }
void hw_write16(uint32_t a, uint16_t v) { access_write(a, v, 2); }
void hw_write8(uint32_t a, uint8_t v)   { access_write(a, v, 1); }

bool hw_get_unlock_silber(void) { return g_unlock_silber; }
void hw_set_unlock_silber(bool on) {
    g_unlock_silber = on;
    if (!g_ram) return;
    // the game's cached copy of the flags, if it has loaded them already
    uint16_t* cache = (uint16_t*)(g_ram + (SILBER_CACHE_RAM ^ 2));
    if (!(*cache & SILBER_CACHE_LOADED)) return;
    const bool earned = (hw_phys_words(SILBER_FLAGS_PA)[0] >> 16) & SILBER_BIT;
    if (on || earned) *cache |= SILBER_BIT;
    else *cache &= (uint16_t)~SILBER_BIT;
}

// ---- the arcade's operator settings ---------------------------------------------------------
// Found by changing each option in the Test menu and comparing the save memory (NOTES.md,
// "Arcade options"). Each is one byte in the nvram; at power on the game copies them into RAM
// and from then on reads only the copies, so a change is written to both: the nvram byte keeps
// it for next time and the RAM byte makes it apply now (the game looks most of them up when a
// fight or screen starts). There is no checksum. Addresses are for Buriki One rev. B.
//   coin settings: nvram 0x2030 + n  <->  RAM 0x801446E0 + n     (free play = +2)
//   game options:  nvram 0x2050 + n  <->  RAM 0x801446F0 + n     (in the Test menu's order)
// LANGUAGE (0x2059) is left to the Test menu on purpose: changing it while the game runs sends
// the game to resources it hasn't loaded (it stops with a bad read).
namespace {
struct GameOption { uint16_t nv; uint32_t ram; uint8_t lo, hi; };
const GameOption k_game_options[GOPT_COUNT] = {
    {0x2032, 0x1446E2, 0, 1},   // FREE PLAY         0 off, 1 on
    {0x2050, 0x1446F0, 1, 4},   // DIFFICULTY        1 easy, 2 normal, 3 hard, 4 hardest
    {0x2051, 0x1446F1, 0, 1},   // DEMO SOUND        0 off, 1 on
    {0x2052, 0x1446F2, 0, 1},   // COMMAND DISP.     0 without, 1 with
    {0x2053, 0x1446F3, 1, 3},   // ATTACK POWER CPU  1 light, 2 normal, 3 heavy
    {0x2054, 0x1446F4, 1, 3},   // ATTACK POWER VS.
    {0x2055, 0x1446F5, 1, 3},   // PLAY TIME CPU     1 slow, 2 normal, 3 quick
    {0x2056, 0x1446F6, 1, 3},   // PLAY TIME VS.
    {0x2057, 0x1446F7, 0, 1},   // REPLAY            0 off, 1 on
    {0x2058, 0x1446F8, 0, 1},   // CONTINUE          0 off, 1 on
    {0x205D, 0x1446FD, 0, 1},   // BLOOD DISP.       0 off, 1 on
};
uint8_t* nvram_byte(uint32_t off) { return (uint8_t*)hw_phys_words(0x1F800000 + (off & ~3u)) + ((off & 3) ^ 3); }
}

// Ready = the saved difficulty is a real value and the game's RAM copy agrees with it (so the
// game has set up or loaded its settings, and isn't in the middle of initialising them).
bool hw_game_options_ready(void) {
    if (!g_ram) return false;
    const GameOption& d = k_game_options[GOPT_DIFFICULTY];
    const uint8_t v = *nvram_byte(d.nv);
    return v >= d.lo && v <= d.hi && g_ram[d.ram ^ 3] == v;
}

int hw_game_option_get(int option) {
    if (option < 0 || option >= GOPT_COUNT || !hw_game_options_ready()) return -1;
    return *nvram_byte(k_game_options[option].nv);
}

void hw_game_option_set(int option, int value) {
    if (option < 0 || option >= GOPT_COUNT || !hw_game_options_ready()) return;
    const GameOption& o = k_game_options[option];
    if (value < o.lo) value = o.lo;
    if (value > o.hi) value = o.hi;
    *nvram_byte(o.nv) = (uint8_t)value;
    g_ram[o.ram ^ 3] = (uint8_t)value;
    fprintf(stderr, "[options] arcade option %d = %d\n", option, value);
}

// Save state (savestate.h): every writable memory region, the CPU's registers, and this file's
// own interrupt and timing state. ROM regions and counters kept only for diagnostics are left out.
extern "C++" void hw_state(StateIO& io, void* recomp_ctx) {
    for (Region& r : g_regions) {
        if (r.read_only) continue;
        if ((r.base == 0x60000000 || r.base == 0x68000000) && savestate_without_sound()) continue;   // the sound board's
        if (savestate_pages_separate() && pagestate_owns(r.data)) continue;                          // copied by pagestate
        io.raw(r.data, r.size);
    }
    io.raw(recomp_ctx, sizeof(recomp_context));
    io.pod(g_ticks); io.pod(g_next_event); io.pod(g_cop0_status);
    io.pod(g_tlb);
    io.pod(g_irq_pending); io.pod(g_irq_level);
    io.pod(g_dma_src); io.pod(g_dma_dst); io.pod(g_dma_len);
    io.pod(g_mcu_en);
    io.pod(g_in_vblank); io.pod(g_frame);
    io.pod(g_next_vblank); io.pod(g_vblank_end); io.pod(g_fifo_irq_at); io.pod(g_next_mcu);
    io.pod(g_in_irq);
}

void hw_tlb_write(uint32_t index, uint32_t pagemask, uint32_t entryhi, uint32_t phys0, uint32_t phys1) {
    if (index >= 48) hw_fatal("TLB index %u out of range", index);
    g_tlb[index] = {true, pagemask, entryhi & 0xFFFFE000u, phys0, phys1};
    uint32_t page = ((pagemask >> 13) + 1) * 0x1000u;
    fprintf(stderr, "[tlb] #%u  %08X-%08X -> %08X, %08X-%08X -> %08X\n", index,
            entryhi, entryhi + page - 1, phys0, entryhi + page, entryhi + 2 * page - 1, phys1);
}

uint32_t hw_cop0_count(void) {
    // VR4300 Count runs at half the CPU clock. With no cycle counting, scale ticks.
    return (uint32_t)(g_ticks * 48u);
}

uint32_t hw_cart_read32(uint32_t offset) { return phys_read32(0x04000000u + offset); }

uint32_t* hw_phys_words(uint32_t phys) {
    Region* r = find_region(phys);
    return r ? (uint32_t*)(r->data + ((phys - r->base) & ~3u)) : nullptr;
}

uint32_t hw_frame_number(void) { return g_frame; }

void hw_raise_irq(uint32_t bits) { raise_irq(bits); }

void hw_dump_recent_functions(void) {
    fprintf(stderr, "Most recently entered functions (oldest first):\n");
    for (uint32_t i = 0; i < HW_FUNC_RING; i++) {
        const char* f = g_func_ring[(g_func_ring_pos + i) & (HW_FUNC_RING - 1)];
        if (f) fprintf(stderr, "  %s\n", f);
    }
}

void hw_report(void) {
    sound_report();
    fprintf(stderr, "\n==== hardware access summary (frame %u, %llu function calls) ====\n",
            g_frame, (unsigned long long)g_ticks);
    for (int i = 0; i < 32; i++)
        if (g_irq_count[i]) fprintf(stderr, "  irq level %2d delivered %llu times\n", i, (unsigned long long)g_irq_count[i]);
    fprintf(stderr, "  %-10s %-12s %10s %10s  %-10s %s\n", "phys", "region", "reads", "writes", "last write", "first seen in");
    for (auto& [pa, s] : g_io) {
        Region* r;
        if (is_bulk(pa, &r))
            fprintf(stderr, "  (area)     %-12s %10llu %10llu  %-10s %s\n", r->name,
                    (unsigned long long)s.reads, (unsigned long long)s.writes, "", s.first_func);
        else
            fprintf(stderr, "  %08X   %-12s %10llu %10llu  %08X   %s\n", pa, region_name(pa),
                    (unsigned long long)s.reads, (unsigned long long)s.writes, s.last_write, s.first_func);
    }
}

void hw_note_nan(const char* func, void* vctx) {
    static uint64_t count = 0;
    if (++count <= 10 || (count & (count - 1)) == 0)
        fprintf(stderr, "[nan] float NaN operand in %s, frame %u (occurrence %llu)\n",
                func, g_frame, (unsigned long long)count);
    if (count == 1) {
        // First one: dump enough state to trace where the NaN came from.
        recomp_context* ctx = (recomp_context*)vctx;
        gpr* r = &ctx->r0;
        for (int i = 0; i < 32; i++)
            fprintf(stderr, "  r%-2d %08X%s", i, (uint32_t)r[i], (i % 4 == 3) ? "\n" : "");
        fpr* f = &ctx->f0;
        for (int i = 0; i < 32; i++)
            fprintf(stderr, "  f%-2d %08X:%08X%s", i, f[i].u32h, f[i].u32l, (i % 4 == 3) ? "\n" : "");
        for (int reg : {4, 5, 6, 7}) {
            uint32_t a = (uint32_t)r[reg] & ~3u;
            if (hw_ram_offset(a) < 0) continue;
            fprintf(stderr, "  memory at r%d (%08X):", reg, a);
            for (int k = 0; k < 16; k++) fprintf(stderr, "%s%08X", k % 8 ? " " : "\n    ", mem_r32(a + k * 4));
            fprintf(stderr, "\n");
        }
        hw_dump_recent_functions();
    }
}

// Diagnostics for finding and testing game variables, run at the end of each frame:
//   BURIKI_RAMDUMP=dir:f1,f2,...   save main RAM as dir/ram_NNNNN.bin on those frames, big-endian
//                                  (the byte at RAM address A is at file offset A)
//   BURIKI_POKE=f0-f1:ADDR=VV;...  on frames f0..f1 write the hex byte VV to RAM address ADDR (hex,
//                                  with or without the 0x80000000; 1F80xxxx = the save memory). More than two hex digits: 16 or
//                                  32 bits, big-endian. Changes the game: for experiments only.
static void hw_debug_frame() {
    struct Poke { uint32_t f0, f1, addr, val; int bytes; };
    static std::vector<uint32_t> dump_frames;
    static std::string dump_dir;
    static std::vector<Poke> pokes;
    static bool init = false;
    if (!init) {
        init = true;
        if (const char* e = getenv("BURIKI_RAMDUMP")) {
            std::string t = e;
            size_t c = t.rfind(':');
            if (c != std::string::npos) {
                dump_dir = t.substr(0, c);
                for (char* q = strtok(&t[c + 1], ","); q; q = strtok(nullptr, ",")) dump_frames.push_back((uint32_t)atoi(q));
            }
        }
        if (const char* e = getenv("BURIKI_POKE")) {
            std::string t = e;
            for (char* q = strtok(&t[0], ";"); q; q = strtok(nullptr, ";")) {
                Poke k{};
                char val[32] = {0};
                if (sscanf(q, "%u-%u:%x=%31s", &k.f0, &k.f1, &k.addr, val) == 4) {
                    k.val = (uint32_t)strtoul(val, nullptr, 16);
                    k.bytes = strlen(val) <= 2 ? 1 : strlen(val) <= 4 ? 2 : 4;
                    if ((k.addr >> 16) != 0x1F80) k.addr &= HW_RAM_SIZE - 1;   // 1F80xxxx: the save memory
                    pokes.push_back(k);
                    fprintf(stderr, "[poke] frames %u-%u: %08X = %0*X\n", k.f0, k.f1, 0x80000000u | k.addr, k.bytes * 2, k.val);
                }
            }
        }
    }
    for (const Poke& k : pokes)
        if (g_frame >= k.f0 && g_frame <= k.f1)
            for (int i = 0; i < k.bytes; i++) {
                const uint8_t b = (uint8_t)(k.val >> (8 * (k.bytes - 1 - i)));
                if ((k.addr >> 16) == 0x1F80) ((uint8_t*)hw_phys_words((k.addr + i) & ~3u))[((k.addr + i) & 3) ^ 3] = b;
                else g_ram[(k.addr + i) ^ 3] = b;
            }
    if (std::find(dump_frames.begin(), dump_frames.end(), g_frame) != dump_frames.end()) {
        char name[64];
        snprintf(name, sizeof name, "/ram_%05u.bin", g_frame);
        if (FILE* f = fopen((dump_dir + name).c_str(), "wb")) {
            std::vector<uint8_t> be(HW_RAM_SIZE);
            for (uint32_t i = 0; i < HW_RAM_SIZE; i++) be[i] = g_ram[i ^ 3];
            fwrite(be.data(), 1, be.size(), f);
            fclose(f);
        }
    }
}

void (*g_fatal_hook)(const char* message) = nullptr;

static std::atomic<bool> g_fatal_claimed{false};
bool hw_fatal_claim(void) { return !g_fatal_claimed.exchange(true); }
bool hw_fatal_busy(void) { return g_fatal_claimed.load(); }

void hw_fatal(const char* fmt, ...) {
    // Only the first fatal error is reported. A second one (the watchdog noticing that the game has
    // stopped, because the first error's message box is open) waits here until the program exits.
    if (!hw_fatal_claim()) for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n[FATAL] %s\n", msg);
    hw_dump_recent_functions();
    hw_report();
    fflush(stderr);
    if (g_fatal_hook) g_fatal_hook(msg);
    exit(3);
}

void hw_init(const HwConfig* cfg) {
    g_cfg = *cfg;
    std::string dir = cfg->rom_dir;

    g_ram = add_region(0x00000000, HW_RAM_SIZE, false, "main ram");

    // Cartridge program ROM, 32 MB at physical 0x04000000. Each pair of chips is
    // interleaved 16 bits at a time (MAME: ROM_LOAD32_WORD_SWAP, even chip in the high half).
    uint8_t* cart = add_region(0x04000000, 0x02000000, true, "cart rom");
    const char* pairs[3][2] = {{"007pr01b.81", "007pr02b.82"}, {"007pr03a.83", "007pr04a.84"}, {"007pr05a.85", "007pr06a.86"}};
    for (int p = 0; p < 3; p++) {
        auto lo = read_file(dir + "/buriki/" + pairs[p][0]);
        auto hi = read_file(dir + "/buriki/" + pairs[p][1]);
        uint32_t* out = (uint32_t*)(cart + p * 0x800000);
        for (size_t i = 0; i < lo.size() / 2; i++) {
            uint32_t h = (uint32_t)hi[i * 2 + 1] << 8 | hi[i * 2];
            uint32_t l = (uint32_t)lo[i * 2 + 1] << 8 | lo[i * 2];
            out[i] = h << 16 | l;
        }
    }

    // BIOS: big-endian, 512 KB at 0x1FC00000.
    // BIOS region decides the game's language: brom1 = Japan, bios_us = USA,
    // bios_export = Export/World, bios_korea = Korea (MAME hng64 set).
    auto bios = read_file(dir + "/hng64/" + (cfg->bios_file ? cfg->bios_file : "brom1.bin"));
    uint8_t* b = add_region(0x1FC00000, 0x80000, true, "bios");
    for (size_t i = 0; i < bios.size() / 4; i++)
        ((uint32_t*)b)[i] = (uint32_t)bios[i * 4] << 24 | bios[i * 4 + 1] << 16 | bios[i * 4 + 2] << 8 | bios[i * 4 + 3];

    add_region(0x1F700000, 0x2200, false, "sysregs");
    add_region(0x1F800000, 0x4000, false, "nvram");
    add_region(0x1F808000, 0x800, false, "dualport");
    add_region(0x20000000, 0x20000, false, "sprite ram");
    add_region(0x20100000, 0x80000, false, "tilemap ram");
    add_region(0x20190000, 0x100, false, "video regs");
    add_region(0x20200000, 0x4000, false, "palette");
    add_region(0x20208000, 0x100, false, "tcram");
    add_region(0x20300000, 0x400, false, "3d list");
    add_region(0x30000000, 0x300000, false, "framebuffer");
    add_region(0x60000000, 0x400000, false, "sound ram");
    add_region(0x68000000, 0x10, false, "sound comms");
    add_region(0x6F000000, 0x4, false, "sound enable");
    add_region(0xC0000000, 0x2000, false, "comm dpram");

    video_init(cfg->rom_dir);
    menu_art_build();               // the pause menu's portraits, from the sprite ROMs
    io_board_init(cfg->rom_dir, cfg->ticks_per_frame);
    sound_init(cfg->rom_dir, cfg->ticks_per_frame, cfg->sound_play, cfg->wav_path);
    input_print_help();

    g_next_vblank = cfg->ticks_per_frame;
    g_next_event = g_next_vblank;
}

void hw_boot(recomp_context* ctx) {
    // What the BIOS does on a normal boot (BIOS 0xBFC06444 onwards), without its self-tests:
    //  1. DMA the game from the cart into RAM, using the cart header fields at 0x50/0x54/0x58.
    //  2. DMA BIOS 0x1FC08000 (16 KB of service routines) to RAM 0x4000.
    //  3. Jump to the entry point in the header at 0x5C (the caller does this).
    g_dma_src = hw_cart_read32(0x50); g_dma_dst = hw_cart_read32(0x54); g_dma_len = hw_cart_read32(0x58) - 1;
    do_dma();
    g_dma_src = 0x1FC08000; g_dma_dst = 0x00004000; g_dma_len = 0x1000 - 1;
    do_dma();

    // CPU state the BIOS leaves behind: Status = CU1|CU0|FR (0x34000000), FPU control 0x01000000.
    g_cop0_status = 0x34000000;
    ctx->status_reg = g_cop0_status;
    ctx->mips3_float_mode = 1;           // Status.FR = 1: 32 independent 64-bit float registers
    // Generated code reads odd float registers as f_odd[(n - 1) * 2]. With FR = 1 that must
    // land on f(n).u32l, so f_odd points at f1. (With FR = 0 it would be &f0.u32h, the
    // high half of the even/odd pair, which is the N64 convention.)
    ctx->f_odd = &ctx->f1.u32l;
    ctx->r29 = (gpr)(int32_t)0x80004000;
    fprintf(stderr, "[boot] entry point %08X\n", hw_cart_read32(0x5C));
}

void hw_events(uint8_t* rdram, recomp_context* ctx) {
    // Vblank start/end.
    if (g_ticks >= g_next_vblank) {
        g_next_vblank += g_cfg.ticks_per_frame;
        g_vblank_end = g_ticks + g_cfg.ticks_per_frame / 12;
        g_in_vblank = true;
        g_frame++;
        if (!savestate_skip_video()) { PerfTimer t(g_perf_video); video_frame(); }   // draw + present the frame the game just built
        // Online play: exchange controls with the other player; may put the machine back to an earlier
        // frame (a rollback), after which the frames up to this one play again without being drawn.
        netplay_frame(ctx, __builtin_return_address(0), g_in_irq);
        input_sample_frame();        // controls for the next frame
        { PerfTimer t(g_perf_sound); sound_frame(); }   // this frame's audio to the speakers / WAV
        raise_irq(1u << 0);
        if (g_frame <= 5 || g_frame % 60 == 0) fprintf(stderr, "[frame] %u  (%llu calls)\n", g_frame, (unsigned long long)g_ticks);
        {
            // Diagnostics (env BURIKI_LOGVBL): which function the game is in when each vblank
            // arrives. If it's always the idle wait loop, the game finishes its work in time.
            static bool on = getenv("BURIKI_LOGVBL") != nullptr;
            static std::map<std::string, int> hist;
            if (on) {
                // (with the game's stack pointer: the same function at the same stack depth every frame means
                // the game is at the same place in the same chain of calls, which matters for save states)
                char key[96];
                snprintf(key, sizeof key, "%s sp=%08X", cur_func(), (uint32_t)ctx->r29);
                hist[key]++;
                if (g_frame % 600 == 0) {
                    std::vector<std::pair<int, std::string>> v;
                    for (auto& [k, n] : hist) v.push_back({n, k});
                    std::sort(v.rbegin(), v.rend());
                    fprintf(stderr, "[vbl] frames %u-%u:", g_frame - 600, g_frame);
                    for (size_t i = 0; i < v.size() && i < 4; i++) fprintf(stderr, " %s x%d", v[i].second.c_str(), v[i].first);
                    fprintf(stderr, " (%zu functions)\n", v.size());
                    hist.clear();
                }
            }
        }
        if (g_frame % 600 == 0) {   // speed report every 10 s of game time (wall clock, display only)
            static auto last = std::chrono::steady_clock::now();
            static bool first = true;       // the first call only starts the clock (no time has passed yet)
            auto now = std::chrono::steady_clock::now();
            double s = std::chrono::duration<double>(now - last).count();
            last = now;
            if (first || s <= 0) { first = false; g_perf_video = g_perf_sound = g_perf_io = 0; }
            else fprintf(stderr, "[perf] frames %u-%u: %.0f fps | video %.0f%% sound %.0f%% io %.0f%% other (game) %.0f%%\n",
                    g_frame - 600, g_frame, 600.0 / s, 100 * g_perf_video / s, 100 * g_perf_sound / s, 100 * g_perf_io / s,
                    100 * (s - g_perf_video - g_perf_sound - g_perf_io) / s);
            g_perf_video = g_perf_sound = g_perf_io = 0;
        }
        hw_debug_frame();
        // Save-state test (env BURIKI_SAVETEST). The place in the recompiled code this frame ended at is
        // where hw_events was called from.
        savestate_frame(ctx, __builtin_return_address(0), g_in_irq);
        if (g_cfg.max_frames && g_frame >= g_cfg.max_frames) {
            // Fingerprint of main RAM: two runs with the same inputs must print the same value.
            uint64_t h = 1469598103934665603ull;
            for (uint32_t i = 0; i < HW_RAM_SIZE; i += 8) { h ^= *(uint64_t*)(g_ram + i); h *= 1099511628211ull; }
            fprintf(stderr, "[stop] reached %u frames, RAM fingerprint %016llX\n", g_frame, (unsigned long long)h);
            hw_report();
            fflush(stderr);
            exit(0);
        }
    }
    if (g_in_vblank && g_ticks >= g_vblank_end) g_in_vblank = false;
    if (g_ticks >= g_next_mcu) {
        { PerfTimer t(g_perf_io); io_board_run(); }
        { PerfTimer t(g_perf_sound); sound_run(); }
        g_next_mcu = g_ticks + MCU_SLICE_TICKS;
    }
    if (g_ticks >= g_fifo_irq_at) { g_fifo_irq_at = UINT64_MAX; raise_irq(1u << 3); }

    // Deliver an interrupt if one is pending and the CPU accepts it:
    // Status.IE (bit 0), IM2 (bit 10, the HNG64's interrupt line), not already in an exception.
    bool can_take = (g_cop0_status & 0x401u) == 0x401u && !(g_cop0_status & 0x6u) && !g_in_irq;
    if (g_irq_pending && can_take) {
        uint32_t level = g_irq_level;
        g_irq_count[level & 31]++;
        // Same as the exception vector at 0x80000180: it pushes 0x48 bytes, then 0x18 more
        // before calling the dispatcher 0x800C2B20. Everything is restored afterwards.
        recomp_context saved = *ctx;
        uint32_t saved_status = g_cop0_status;
        g_in_irq = true;
        g_cop0_status |= 0x2u;           // EXL
        ctx->r29 = saved.r29 - 0x48 - 0x18;
        func_800C2B20(rdram, ctx);
        *ctx = saved;
        g_cop0_status = saved_status;
        g_in_irq = false;
    }

    uint64_t next = g_next_vblank;
    if (g_next_mcu < next) next = g_next_mcu;
    if (g_in_vblank && g_vblank_end < next) next = g_vblank_end;
    if (g_fifo_irq_at < next) next = g_fifo_irq_at;
    if (g_irq_pending && g_ticks + 64 < next) next = g_ticks + 64;   // re-check masked irqs
    g_next_event = next;
}

} // extern "C"
