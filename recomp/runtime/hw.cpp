// Hyper Neo Geo 64 hardware layer: memory map, registers, DMA, interrupts, timing.
// Reference: MAME src/mame/snk/hng64.cpp (main_map, sysregs, irqc, dmac, dualport, rtc).
#include "hw.h"
#include "input.h"
#include "funcs.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
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
    if (!data) data = (uint8_t*)calloc(1, size);
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
        if ((pa >> 2) & 1) { v = (((pa - 0x1F702100u) >> 3) & 0xF) == 0xD ? 0x10 : 0; }
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

uint32_t access_read(uint32_t va, int size) {
    uint32_t pa;
    if (!translate(va, pa)) hw_fatal("read from unmapped address %08X (no TLB entry) in %s", va, cur_func());
    if (pa - 0x1F808000u < 0x800) io_board_run();   // let the I/O MCU catch up before sharing RAM
    uint32_t w = phys_read32(pa & ~3u);
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

void hw_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "\n[FATAL] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    hw_dump_recent_functions();
    hw_report();
    fflush(stderr);
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
        { PerfTimer t(g_perf_video); video_frame(); }   // draw + present the frame the game just built
        input_sample_frame();        // controls for the next frame
        { PerfTimer t(g_perf_sound); sound_frame(); }   // this frame's audio to the speakers / WAV
        raise_irq(1u << 0);
        if (g_frame <= 5 || g_frame % 60 == 0) fprintf(stderr, "[frame] %u  (%llu calls)\n", g_frame, (unsigned long long)g_ticks);
        if (g_frame % 600 == 0) {   // speed report every 10 s of game time (wall clock, display only)
            static auto last = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            double s = std::chrono::duration<double>(now - last).count();
            last = now;
            fprintf(stderr, "[perf] frames %u-%u: %.0f fps | video %.0f%% sound %.0f%% io %.0f%% other (game) %.0f%%\n",
                    g_frame - 600, g_frame, 600.0 / s, 100 * g_perf_video / s, 100 * g_perf_sound / s, 100 * g_perf_io / s,
                    100 * (s - g_perf_video - g_perf_sound - g_perf_io) / s);
            g_perf_video = g_perf_sound = g_perf_io = 0;
        }
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
