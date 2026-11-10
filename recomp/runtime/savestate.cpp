// Save-state feasibility test for rollback netplay (see savestate.h).
//
//   BURIKI_SAVETEST=N,K[,R][,novideo][,nohash][,loop]
//
// At the end of frame N the machine is saved. The game plays on for K frames (pass 1) while a fingerprint of
// main RAM is noted every frame. Then the state is put back and the same K frames are played again (the
// replay) with the same controls (an --input-script is a function of the frame number), comparing
// fingerprints frame by frame and the whole machine at the end.
//   R        how many times (default 1). Without "loop" the same K frames are replayed R times.
//   loop     after each replay, save again where it ended and carry on: R save / replay cycles in a row,
//            each K frames long, the way a rollback would use it all through a fight. A cycle whose frames
//            don't end at the wait point in the game's main loop (scene changes) is skipped and counted.
//   novideo  the replays skip drawing, as a rollback would: the time per replayed frame is then the cost
//            that matters.
//   nohash   no per-frame fingerprints (they cost a few milliseconds each): only the whole machine at the
//            end of each replay is compared. For clean timings.
// It prints what it found and stops the program.
#include "savestate.h"
#include "hw.h"
#include "funcs.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

uint64_t fnv(const uint8_t* p, size_t n, uint64_t h = 1469598103934665603ull) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h ^= w; h *= 1099511628211ull; }
    for (; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
uint64_t ram_hash() { return fnv(g_ram, HW_RAM_SIZE); }

void machine(StateIO& io, void* ctx) {
    hw_state(io, ctx);
    sound_state(io);
    io_board_state(io);
    input_state(io);
}

struct Test {
    bool on = false, parsed = false, novideo = false, nohash = false, loop = false;
    uint32_t n = 0, k = 0, repeats = 1;
    int phase = 0;                 // 0 looking for a frame to save at, 1 first pass, 2 replaying
    std::vector<uint8_t> state;    // the saved machine
    std::vector<uint8_t> scratch;  // for whole-machine fingerprints
    std::vector<uint64_t> ram;     // pass 1: main RAM fingerprint after each frame
    uint64_t full = 0;             // pass 1: whole-machine fingerprint at the end
    const void* site = nullptr;    // the wait point: where the first saved frame ended
    uint32_t sp = 0;
    Clock::time_point t0;
    // totals
    uint32_t done = 0, identical = 0, different = 0, skipped = 0, first_bad = 0;
    uint64_t frames_replayed = 0;
    double save_ms = 0, load_ms = 0, pass1_ms = 0, replay_ms = 0, save_worst = 0, load_worst = 0, replay_worst = 0;
    uint32_t saves = 0, loads = 0;
    int mismatches = 0;
} g;

void parse() {
    g.parsed = true;
    const char* e = getenv("BURIKI_SAVETEST");
    if (!e) return;
    unsigned n = 0, k = 0, r = 1;
    const int got = sscanf(e, "%u,%u,%u", &n, &k, &r);
    if (got < 2 || k == 0) { fprintf(stderr, "[savetest] BURIKI_SAVETEST=N,K[,R][,novideo][,nohash][,loop]\n"); return; }
    const std::string s = e;
    g.on = true; g.n = n; g.k = k; g.repeats = got >= 3 && r ? r : 1;
    g.novideo = s.find("novideo") != std::string::npos;
    g.nohash = s.find("nohash") != std::string::npos;
    g.loop = s.find("loop") != std::string::npos;
    g.ram.resize(k + 1);
    g.state.reserve(32u << 20);
    g.scratch.reserve(32u << 20);
}

uint64_t full_hash(void* ctx) {
    g.scratch.clear();
    StateIO io{g.scratch, true};
    machine(io, ctx);
    return fnv(g.scratch.data(), g.scratch.size());
}

void save(void* ctx, uint32_t frame) {
    g.state.clear();
    auto t = Clock::now();
    StateIO io{g.state, true};
    machine(io, ctx);
    const double ms = ms_since(t);
    g.save_ms += ms; g.saves++;
    if (ms > g.save_worst) g.save_worst = ms;
    g.n = frame;
    if (!g.nohash) g.ram[0] = ram_hash();
    g.phase = 1;
    g.t0 = Clock::now();
}

void load(void* ctx) {
    auto t = Clock::now();
    StateIO io{g.state, false};
    machine(io, ctx);
    const double ms = ms_since(t);
    g.load_ms += ms; g.loads++;
    if (ms > g.load_worst) g.load_worst = ms;
    g.phase = 2;
    g.mismatches = 0;
    g.t0 = Clock::now();
}

void finish() {
    fprintf(stderr, "[savetest] RESULT: %u save / replay cycles of %u frames: %u identical, %u different%s, %u skipped (not at the wait point)\n",
            g.done, g.k, g.identical, g.different,
            g.different ? (" (first at frame " + std::to_string(g.first_bad) + ")").c_str() : "", g.skipped);
    if (g.saves && g.loads && g.frames_replayed)
        fprintf(stderr, "[savetest] TIMES: save %.2f ms (worst %.2f), load %.2f ms (worst %.2f), replay %.2f ms per frame%s (worst cycle %.2f), "
                        "first pass %.2f ms per frame, state %.1f MB\n",
                g.save_ms / g.saves, g.save_worst, g.load_ms / g.loads, g.load_worst, g.replay_ms / g.frames_replayed,
                g.novideo ? ", not drawing" : "", g.replay_worst, g.pass1_ms / g.frames_replayed, g.state.size() / 1048576.0);
    fflush(stderr);
    exit(g.different ? 4 : 0);
}

} // namespace

bool savestate_skip_video(void) { return g.on && g.novideo && g.phase == 2; }

void savestate_frame(void* ctx, const void* site, bool in_irq) {
    if (!g.parsed) parse();
    if (!g.on) return;
    const uint32_t frame = hw_frame_number();
    const uint32_t sp = (uint32_t)((recomp_context*)ctx)->r29;
    const bool at_wait = !in_irq && (!g.site || (site == g.site && sp == g.sp));

    if (g.phase == 0) {
        if (frame < g.n) return;
        if (!at_wait) return;                       // look for the next frame that ends at the wait point
        if (!g.site) {
            g.site = site; g.sp = sp;
            fprintf(stderr, "[savetest] first save at frame %u (code site %p, stack %08X)\n", frame, site, sp);
        }
        save(ctx, frame);
        return;
    }

    const uint32_t i = frame - g.n;                 // frames since the save
    if (g.phase == 1) {
        if (!g.nohash && i <= g.k) g.ram[i] = ram_hash();
        if (i < g.k) return;
        g.pass1_ms += ms_since(g.t0);
        if (!at_wait) {                             // can't put the state back here: give this cycle up
            g.skipped++;
            g.done++;
            g.phase = 0;
            if (!g.loop) { fprintf(stderr, "[savetest] frame %u ended somewhere else (site %p, stack %08X): can't load here\n", frame, site, sp); g.different++; g.first_bad = frame; finish(); }
            if (g.done >= g.repeats) finish();
            return;
        }
        g.full = full_hash(ctx);
        load(ctx);                                  // back to frame n; the same frames now play again
        return;
    }

    // replaying
    if (!g.nohash && i <= g.k && ram_hash() != g.ram[i]) { if (!g.mismatches++ && !g.first_bad) g.first_bad = frame; }
    if (i < g.k) return;
    const double ms = ms_since(g.t0);
    g.replay_ms += ms; g.frames_replayed += g.k;
    if (ms / g.k > g.replay_worst) g.replay_worst = ms / g.k;
    const bool same = g.mismatches == 0 && full_hash(ctx) == g.full;
    if (same) g.identical++;
    else { g.different++; if (!g.first_bad) g.first_bad = frame; }
    g.done++;
    if (!g.loop)
        fprintf(stderr, "[savetest] replay %u: %s (main RAM differed on %d of %u frames)\n", g.done, same ? "IDENTICAL" : "DIFFERENT", g.mismatches, g.k);
    if (g.done >= g.repeats) finish();
    if (g.loop) save(ctx, frame);                         // carry on from here: the next cycle starts at this frame
    else load(ctx);                                       // the same frames again
}
