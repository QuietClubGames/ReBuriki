// Online play: rollback netcode over a direct UDP link (netplay.h; NOTES.md, "Online play").
//
// Frames and controls. hw_events calls netplay_frame() at every frame boundary, where the controls used to
// be read. "Frame F's controls" are the ones latched at the boundary where the frame counter becomes F.
// A player's controls read at boundary F are used for frame F + delay, and are sent at once, so with a
// connection faster than the delay the other side has them in time and nothing is ever guessed.
//
// States. At a boundary the machine can be saved (savestate.h) when the game is waiting in its main loop:
// not inside an interrupt, stack pointer at the main loop's level (WAIT_SP). That is nearly every frame.
// A state saved at boundary k holds the machine before frame k's controls are applied. It can be put back
// at a later boundary that ended at the same place in the code (same `site`), because then the host's own
// stack is the same as when it was saved.
//
// Guessing. Frame F may be played with a guess for the other player only while a state from a frame no
// later than the first unconfirmed one is being kept (so a wrong guess can be undone) and no more than
// MAX_PRED frames are unconfirmed. Otherwise the game waits for the real controls. Start-up and scene
// changes, where the game is not at its wait point, therefore run in step and fights run ahead.
//
// Sound. The sound board is outside all this: it runs forward only (NOTES.md, "Sound board and rollback").
// What the game tells it is noted frame by frame ("told"). When a rollback plays frames again, the commands
// the game sends are compared with what the board was told for those frames the first time: the ones it has
// already had are dropped, and new ones (the other player's move that had not arrived yet) are given to it
// when the rollback ends, a few frames late. A sound that played on a wrong guess cannot be taken back.
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "netplay.h"
#include "savestate.h"
#include "pagestate.h"
#include "hw.h"
#include "hw_mem.h"
#include "video.h"
#include "frontend/frontend.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

constexpr uint32_t MAGIC = 0x314B4252;      // "RBK1"
constexpr uint32_t PROTO = 1;
// Both players need the same emulation. Change this whenever the machine's behaviour changes.
constexpr char BUILD[24] = "ReBuriki netplay 1";
constexpr uint32_t WAIT_SP = 0x803FFFC0;    // the game's stack pointer in its main loop (func_800080B0)
constexpr int MAX_PRED = 8;                 // frames played on a guess, at most
constexpr int RING = 12;                    // saved states kept
constexpr uint32_t HIST = 1024;             // frames of controls remembered (a power of two)
constexpr uint32_t CHECK_EVERY = 60;        // frames between the two games comparing their memory
constexpr double FRAME_MS = 1000.0 / 59.43; // the arcade's frame
constexpr double TIMEOUT_MS = 10000;
constexpr uint32_t NVRAM_PHYS = 0x1F800000, NVRAM_SIZE = 0x4000;
constexpr int NV_CHUNK = 1024, NV_CHUNKS = NVRAM_SIZE / NV_CHUNK;

enum : uint8_t { T_HELLO = 1, T_CONFIG, T_NV, T_READY, T_START, T_INPUT, T_BYE };

#pragma pack(push, 1)
struct Hdr { uint32_t magic; uint8_t type; };
struct Hello { Hdr h; uint32_t proto; uint64_t content; char build[24]; };
struct Config { Hdr h; uint8_t ok, delay, unlock, chunks; int64_t clock; char reason[96]; };
struct NvChunk { Hdr h; uint16_t index, len; uint8_t data[NV_CHUNK]; };
struct Check { uint32_t frame; uint64_t hash; };
struct InputPkt {
    Hdr h;
    uint32_t frame;        // the sender's current frame
    uint32_t ack;          // the sender has the receiver's controls up to this frame
    uint32_t first;        // frame of in[0]
    uint32_t t_send;       // sender's clock (ms), echoed back for the ping
    uint32_t t_echo;       // the receiver's t_send most recently seen, and
    uint16_t hold;         //   how long ago that was (ms)
    uint8_t count, pad;
    Check checks[3];       // the sender's latest memory fingerprints
    uint16_t in[64];
};
#pragma pack(pop)

double now_ms() {
    static LARGE_INTEGER freq, start;
    LARGE_INTEGER t;
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start); }
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
}

// ---------------------------------------------------------------- the link
SOCKET g_sock = INVALID_SOCKET;
sockaddr_in g_peer = {};
bool g_have_peer = false;

// BURIKI_NET_SIM=delay_ms,jitter_ms,loss_percent[,seed]: a bad connection for tests, applied to what this
// copy sends.
struct Sim { bool on = false; double delay = 0, jitter = 0, loss = 0; uint64_t rng = 0x9E3779B97F4A7C15ull; } g_sim;
struct Delayed { double at; std::vector<uint8_t> data; };
std::vector<Delayed> g_delayed;
double sim_rand() {
    g_sim.rng ^= g_sim.rng << 13; g_sim.rng ^= g_sim.rng >> 7; g_sim.rng ^= g_sim.rng << 17;
    return (double)(g_sim.rng >> 11) / (double)(1ull << 53);
}
void raw_send(const void* p, int len) {
    if (g_have_peer) sendto(g_sock, (const char*)p, len, 0, (const sockaddr*)&g_peer, sizeof g_peer);
}
void send_pkt(const void* p, int len) {
    if (!g_sim.on) { raw_send(p, len); return; }
    if (sim_rand() * 100.0 < g_sim.loss) return;
    g_delayed.push_back({now_ms() + g_sim.delay + sim_rand() * g_sim.jitter,
                         std::vector<uint8_t>((const uint8_t*)p, (const uint8_t*)p + len)});
}
void flush_sim() {
    if (g_delayed.empty()) return;
    const double t = now_ms();
    for (size_t i = 0; i < g_delayed.size();) {
        if (g_delayed[i].at <= t) { raw_send(g_delayed[i].data.data(), (int)g_delayed[i].data.size()); g_delayed.erase(g_delayed.begin() + i); }
        else i++;
    }
}
int recv_pkt(uint8_t* buf, int cap, sockaddr_in* from) {
    int fl = sizeof *from;
    const int got = recvfrom(g_sock, (char*)buf, cap, 0, (sockaddr*)from, &fl);
    return got > 0 ? got : 0;     // nothing waiting, or an error such as "port unreachable" from an earlier send
}
bool same_addr(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}
template <class T> T make(uint8_t type) { T t; memset(&t, 0, sizeof t); t.h.magic = MAGIC; t.h.type = type; return t; }

// ---------------------------------------------------------------- the session
// A saved state: the large memory areas (pagestate.h: only the pages that changed are copied) and
// everything else (savestate.h).
struct Slot { uint32_t frame = 0; bool valid = false; const void* site = nullptr; PageCopy pages; std::vector<uint8_t> buf; };

struct Net {
    bool active = false, host = false, replaying = false, peer_left = false;
    int delay = 2;
    int64_t clock = 0;
    uint32_t max_frames = 0;
    // controls, by frame
    uint16_t local[HIST] = {}, remote[HIST] = {}, used[HIST] = {};
    uint32_t local_have = 0;       // our controls are known up to this frame
    uint32_t remote_have = 0;      // theirs have arrived up to this frame
    uint32_t peer_ack = 0;         // they have ours up to this frame
    uint32_t applied = 0;          // newest frame whose controls the game has been given (on this timeline)
    uint32_t wrong_from = 0;       // earliest frame played with a wrong guess (0 = none)
    uint32_t sampled = 0;          // newest boundary where our controls were read
    uint32_t replay_until = 0;
    uint16_t frame_in[2] = {};
    Slot ring[RING];
    // the other side's clock and the ping
    uint32_t peer_frame = 0; double peer_frame_at = 0;
    uint32_t echo_t = 0; double echo_at = 0;
    double rtt = -1, last_recv = 0, next_tick = 0;
    // memory fingerprints
    Check mine[16] = {}, theirs[16] = {};
    uint32_t mine_n = 0, theirs_n = 0, last_check = 0, compared = 0, checks_ok = 0, checks_bad = 0, first_bad = 0;
    // statistics
    uint32_t rollbacks = 0, replayed = 0, deepest = 0, waits = 0, postponed = 0, saves = 0;
    double wait_ms = 0, replay_ms = 0, replay_t0 = 0, save_ms = 0, replay_worst = 0;
    uint32_t final_frame = 0;
    uint64_t pages_saved = 0;
    // sound commands
    struct Told { uint32_t frame; uint8_t count; uint32_t cmd[6]; } told[HIST] = {};
    std::vector<uint32_t> pool;                              // told in the frames being played again, not matched yet
    std::vector<std::pair<uint32_t, uint32_t>> late;         // (frame, command) new in the frames played again
    uint32_t sounds = 0, sounds_late = 0, sounds_wrong = 0;
} n;

uint64_t ram_fingerprint(const uint8_t* p) {
    // four interleaved FNV-style lanes over 64-bit words (fast: this runs on 16 MB once a second)
    uint64_t h[4] = {1469598103934665603ull, 0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull};
    for (uint32_t i = 0; i + 32 <= HW_RAM_SIZE; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        for (int k = 0; k < 4; k++) h[k] = (h[k] ^ w[k]) * 1099511628211ull;
    }
    return h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7);
}

void bye() {
    if (!n.active || !g_have_peer) return;
    Hdr b = {MAGIC, T_BYE};
    for (int i = 0; i < 3; i++) raw_send(&b, sizeof b);
}

void report() {
    fprintf(stderr, "[netplay] %s, delay %d: %u rollbacks, %u frames replayed (deepest %u, %.2f ms a frame, longest rollback %.1f ms), "
                    "waited %u times (%.0f ms in all), ping %.0f ms, %u saves (%.2f ms and %.0f KB each)\n",
            n.host ? "host" : "joined", n.delay, n.rollbacks, n.replayed, n.deepest,
            n.replayed ? n.replay_ms / n.replayed : 0.0, n.replay_worst, n.waits, n.wait_ms, n.rtt < 0 ? 0.0 : n.rtt,
            n.saves, n.saves ? n.save_ms / n.saves : 0.0, n.saves ? n.pages_saved * 4.0 / n.saves : 0.0);
    fprintf(stderr, "[netplay] sound: %u commands, %u given late after a rollback, %u played on a wrong guess\n",
            n.sounds, n.sounds_late, n.sounds_wrong);
    fprintf(stderr, "[netplay] memory compared with the other player %u times: %u the same, %u different%s\n",
            n.checks_ok + n.checks_bad, n.checks_ok, n.checks_bad,
            n.checks_bad ? (" (first at frame " + std::to_string(n.first_bad) + ")").c_str() : "");
}

// At exit, however the program ends: the session's numbers in the log, and a goodbye to the other player.
void finish() {
    static bool done = false;
    if (done || !n.active) return;
    done = true;
    report();
    bye();
}

[[noreturn]] void stop(const char* why) {
    fprintf(stderr, "[netplay] %s (frame %u)\n", why, hw_frame_number());
    finish();
    hw_fatal("Online play ended: %s.", why);
}

void compare_checks() {
    for (uint32_t i = 0; i < 16; i++) {
        const Check& a = n.mine[i];
        if (!a.frame || a.frame <= n.compared) continue;
        for (uint32_t j = 0; j < 16; j++) {
            if (n.theirs[j].frame != a.frame) continue;
            if (n.theirs[j].hash == a.hash) n.checks_ok++;
            else {
                if (!n.checks_bad++) {
                    n.first_bad = a.frame;
                    fprintf(stderr, "[netplay] OUT OF STEP: the two games' memory differs at frame %u\n", a.frame);
                }
            }
            n.compared = std::max(n.compared, a.frame);
        }
    }
}

void handle(const uint8_t* buf, int len) {
    Hdr h;
    if (len < (int)sizeof h) return;
    memcpy(&h, buf, sizeof h);
    if (h.magic != MAGIC) return;
    if (h.type == T_BYE) { n.peer_left = true; return; }
    if (h.type != T_INPUT) return;
    InputPkt p;
    const int fixed = (int)offsetof(InputPkt, in);
    if (len < fixed) return;
    memset(&p, 0, sizeof p);
    memcpy(&p, buf, std::min(len, (int)sizeof p));
    if (p.count > 64 || len < fixed + p.count * 2) return;
    const double t = now_ms();
    n.last_recv = t;
    if (p.frame >= n.peer_frame) { n.peer_frame = p.frame; n.peer_frame_at = t; }
    if (p.ack > n.peer_ack && p.ack <= n.local_have) n.peer_ack = p.ack;
    if (p.t_echo) {
        const double sample = (double)(uint32_t)((uint32_t)t - p.t_echo) - p.hold;
        if (sample >= 0 && sample < 5000) n.rtt = n.rtt < 0 ? sample : n.rtt * 0.9 + sample * 0.1;
    }
    n.echo_t = p.t_send; n.echo_at = t;
    for (int i = 0; i < p.count; i++) {
        const uint32_t f = p.first + (uint32_t)i;
        if (f != n.remote_have + 1) continue;          // already have it, or a gap (it will be sent again)
        n.remote[f % HIST] = p.in[i];
        n.remote_have = f;
        if (f <= n.applied && n.used[f % HIST] != p.in[i] && (!n.wrong_from || f < n.wrong_from)) n.wrong_from = f;
    }
    for (const Check& c : p.checks) {
        if (!c.frame) continue;
        bool known = false;
        for (const Check& k : n.theirs) known |= k.frame == c.frame;
        if (!known) n.theirs[n.theirs_n++ % 16] = c;
    }
    compare_checks();
}

void poll() {
    flush_sim();
    uint8_t buf[2048];
    sockaddr_in from;
    for (int got; (got = recv_pkt(buf, sizeof buf, &from)) > 0;)
        if (g_have_peer && same_addr(from, g_peer)) handle(buf, got);
}

void send_inputs(uint32_t frame) {
    InputPkt p = make<InputPkt>(T_INPUT);
    const double t = now_ms();
    p.frame = frame;
    p.ack = n.remote_have;
    p.first = n.peer_ack + 1;
    const uint32_t count = std::min<uint32_t>(64, n.local_have - n.peer_ack);
    p.count = (uint8_t)count;
    for (uint32_t i = 0; i < count; i++) p.in[i] = n.local[(p.first + i) % HIST];
    p.t_send = (uint32_t)t ? (uint32_t)t : 1;
    p.t_echo = n.echo_t;
    p.hold = (uint16_t)std::min(65535.0, std::max(0.0, t - n.echo_at));
    for (uint32_t i = 0; i < 3 && i < n.mine_n; i++) p.checks[i] = n.mine[(n.mine_n - 1 - i) % 16];
    send_pkt(&p, (int)offsetof(InputPkt, in) + (int)count * 2);
}

// The two players' controls for frame f: ours (always known), and theirs, or a guess: the last ones seen.
void set_inputs(uint32_t f) {
    const uint16_t mine = f <= n.local_have ? n.local[f % HIST] : 0;
    const uint16_t theirs = n.remote[(f <= n.remote_have ? f : n.remote_have) % HIST];
    n.used[f % HIST] = theirs;
    n.applied = f;
    n.frame_in[n.host ? 0 : 1] = mine;
    n.frame_in[n.host ? 1 : 0] = theirs;
}

// The state a wrong guess at or after frame `limit` would be undone from: the newest one not later than it.
Slot* newest_at_or_before(uint32_t limit, const void* site) {
    Slot* best = nullptr;
    for (Slot& s : n.ring)
        if (s.valid && s.frame <= limit && (!site || s.site == site) && (!best || s.frame > best->frame)) best = &s;
    return best;
}

void save_slot(uint32_t frame, const void* site, void* ctx) {
    // Replace an unused state or the oldest, but never the one a rollback would start from.
    const Slot* keep = newest_at_or_before(n.wrong_from ? n.wrong_from : n.remote_have + 1, nullptr);
    Slot* victim = nullptr;
    for (Slot& s : n.ring) {
        if (s.valid && s.frame == frame) { victim = &s; break; }
        if (&s == keep) continue;
        if (!victim || !s.valid || (victim->valid && s.frame < victim->frame)) victim = &s;
        if (!s.valid) break;
    }
    const double t = now_ms();
    pagestate_save(victim->pages);
    n.pages_saved += pagestate_last_pages();
    StateIO io{victim->buf, true};
    savestate_machine(io, ctx);
    victim->buf.resize(io.pos);
    victim->frame = frame; victim->site = site; victim->valid = true;
    n.saves++; n.save_ms += now_ms() - t;
    // Fingerprint of main RAM (the first thing in a state) every CHECK_EVERY frames, once the frame is
    // settled: see run_checks().
}

// A state at a check frame holds the true game once every frame before it has the other player's real
// controls and none of them was guessed wrong (a wrong guess makes later states invalid at the rollback).
void run_checks() {
    for (Slot& s : n.ring) {
        if (!s.valid || s.frame % CHECK_EVERY || s.frame <= n.last_check) continue;
        if (s.frame > n.remote_have + 1 || (n.wrong_from && n.wrong_from < s.frame)) continue;
        n.last_check = s.frame;
        n.mine[n.mine_n++ % 16] = {s.frame, ram_fingerprint(s.pages.data.empty() ? s.buf.data() : s.pages.data.data())};   // main RAM comes first
        compare_checks();
    }
}

void tell(uint32_t frame, uint32_t cmd) {
    Net::Told& t = n.told[frame % HIST];
    if (t.frame != frame) { t.frame = frame; t.count = 0; }
    if (t.count < 6) t.cmd[t.count++] = cmd;
}

void rollback(Slot* s, void* ctx, uint32_t now_frame) {
    n.replay_t0 = now_ms();
    n.pool.clear();
    n.late.clear();
    for (uint32_t f = s->frame; f < now_frame; f++) {
        const Net::Told& t = n.told[f % HIST];
        if (t.frame == f) n.pool.insert(n.pool.end(), t.cmd, t.cmd + t.count);
    }
    pagestate_load(s->pages);
    StateIO io{s->buf, false};
    savestate_machine(io, ctx);                 // the frame counter is s->frame again
    for (Slot& o : n.ring) if (o.valid && o.frame > s->frame) o.valid = false;
    n.rollbacks++;
    n.replayed += now_frame - s->frame;
    n.deepest = std::max(n.deepest, now_frame - s->frame);
    n.replaying = true;
    n.replay_until = now_frame;
    n.wrong_from = 0;
    set_inputs(s->frame);
}

// Keep to the arcade's frame rate when there is no window doing it, and let the other side catch up when
// this copy is ahead of it (each side works that out for itself from the frame numbers in the packets).
void pace(uint32_t frame) {
    if (!g_video_cfg.throttle) return;
    const double t = now_ms();
    if (g_video_cfg.headless) {
        if (n.next_tick < t - 100) n.next_tick = t;
        while (now_ms() < n.next_tick) { poll(); Sleep(1); }
        n.next_tick += FRAME_MS;
    }
    if (n.rtt >= 0 && n.peer_frame) {
        const double theirs = n.peer_frame + (now_ms() - n.peer_frame_at + n.rtt / 2) / FRAME_MS;
        const double ahead = (double)frame - theirs;
        if (ahead > 0.5) Sleep((DWORD)std::min(8.0, ahead * 3.0));
    }
}

uint64_t content_hash() {
    uint64_t h = 1469598103934665603ull;
    const struct { uint32_t base, size; } parts[] = {{0x04000000, 0x02000000}, {0x1FC00000, 0x80000}};
    for (auto& p : parts) {
        const uint32_t* w = hw_phys_words(p.base);
        if (!w) continue;
        for (uint32_t i = 0; i < p.size / 4; i += 2) { h ^= (uint64_t)w[i] | (uint64_t)w[i + 1] << 32; h *= 1099511628211ull; }
    }
    return h;
}

const char* g_error = "";
bool fail(const char* msg) { g_error = msg; return false; }

} // namespace

bool netplay_active(void) { return n.active; }
bool netplay_replaying(void) { return n.replaying; }
bool netplay_is_host(void) { return n.host; }
int64_t netplay_clock(uint32_t frame) { return n.clock + (int64_t)(frame / 60); }
void netplay_frame_inputs(uint16_t bits[2]) { bits[0] = n.frame_in[0]; bits[1] = n.frame_in[1]; }

void netplay_sound_command(uint32_t latch) {
    if (!n.active) return;
    const uint32_t frame = hw_frame_number();
    if (!n.replaying) { n.sounds++; tell(frame, latch); return; }
    auto it = std::find(n.pool.begin(), n.pool.end(), latch);
    if (it != n.pool.end()) n.pool.erase(it);          // the board had this one the first time
    else n.late.push_back({frame, latch});
}

void netplay_idle(void) {
    if (!n.active || n.replaying) return;
    static double last = 0;
    poll();
    const double t = now_ms();
    if (t - last < 50) return;
    last = t;
    send_inputs(n.sampled);
}

const char* netplay_status(void) {
    static char s[96];
    if (!n.active) return "";
    snprintf(s, sizeof s, "ping %.0f ms, %u rollbacks", n.rtt < 0 ? 0.0 : n.rtt, n.rollbacks);
    return s;
}

bool netplay_start(const NetplayOptions& opt, const char** error) {
    static char reason[160];
    *error = "";
    auto failed = [&](bool) { *error = g_error; if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; } return false; };

    if (const char* e = getenv("BURIKI_NET_SIM")) {
        unsigned long long seed = 0;
        if (sscanf(e, "%lf,%lf,%lf,%llu", &g_sim.delay, &g_sim.jitter, &g_sim.loss, &seed) >= 1) {
            g_sim.on = true;
            g_sim.rng ^= seed * 0x2545F4914F6CDD1Dull + GetCurrentProcessId();
            fprintf(stderr, "[netplay] simulated connection: %.0f ms + up to %.0f ms, %.0f%% of packets lost\n", g_sim.delay, g_sim.jitter, g_sim.loss);
        }
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return failed(fail("Windows networking could not be started"));
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return failed(fail("could not open a network socket"));
    u_long nonblocking = 1;
    ioctlsocket(g_sock, FIONBIO, &nonblocking);

    const uint64_t content = content_hash();
    uint32_t* nv = hw_phys_words(NVRAM_PHYS);
    uint8_t buf[2048];
    sockaddr_in from;
    const double t0 = now_ms();

    if (opt.host) {
        sockaddr_in me = {};
        me.sin_family = AF_INET; me.sin_addr.s_addr = htonl(INADDR_ANY); me.sin_port = htons((u_short)opt.port);
        if (bind(g_sock, (sockaddr*)&me, sizeof me)) {
            snprintf(reason, sizeof reason, "port %d is in use by another program", opt.port);
            return failed(fail(reason));
        }
        fprintf(stderr, "[netplay] hosting on UDP port %d, waiting for the other player\n", opt.port);
        Config cfg = make<Config>(T_CONFIG);
        cfg.ok = 1; cfg.delay = (uint8_t)std::clamp(opt.delay, 0, 15); cfg.unlock = opt.unlock_silber; cfg.chunks = NV_CHUNKS;
        const time_t now = time(nullptr);
        struct tm lt = {};
        if (const struct tm* p = localtime(&now)) lt = *p;
        cfg.clock = (int64_t)_mkgmtime(&lt);         // the host's local time, as if it were UTC
        double last = -1e9;
        for (bool ready = false; !ready;) {
            for (int got; (got = recv_pkt(buf, sizeof buf, &from)) > 0;) {
                Hdr h;
                if (got < (int)sizeof h) continue;
                memcpy(&h, buf, sizeof h);
                if (h.magic != MAGIC) continue;
                if (h.type == T_HELLO && got >= (int)sizeof(Hello) && !g_have_peer) {
                    Hello he;
                    memcpy(&he, buf, sizeof he);
                    Config no = make<Config>(T_CONFIG);
                    if (he.proto != PROTO || memcmp(he.build, BUILD, sizeof BUILD))
                        snprintf(no.reason, sizeof no.reason, "the two players have different versions of ReBuriki");
                    else if (he.content != content)
                        snprintf(no.reason, sizeof no.reason, "the two players have different game files or regions");
                    if (no.reason[0]) {
                        fprintf(stderr, "[netplay] turned a player away: %s\n", no.reason);
                        sendto(g_sock, (const char*)&no, sizeof no, 0, (sockaddr*)&from, sizeof from);
                        continue;
                    }
                    g_peer = from; g_have_peer = true;
                    char ip[64] = "";
                    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                    fprintf(stderr, "[netplay] a player is joining from %s\n", ip);
                } else if (h.type == T_READY && g_have_peer && same_addr(from, g_peer)) ready = true;
            }
            if (g_have_peer && now_ms() - last > 200) {
                last = now_ms();
                raw_send(&cfg, sizeof cfg);
                for (int i = 0; i < NV_CHUNKS; i++) {
                    NvChunk c = make<NvChunk>(T_NV);
                    c.index = (uint16_t)i; c.len = NV_CHUNK;
                    memcpy(c.data, (const uint8_t*)nv + i * NV_CHUNK, NV_CHUNK);
                    raw_send(&c, sizeof c);
                }
            }
            if (g_video_cfg.headless && now_ms() - t0 > 60000) return failed(fail("nobody joined"));
            fe_wait_pump();
            Sleep(2);
        }
        Hdr start = {MAGIC, T_START};
        for (int i = 0; i < 3; i++) raw_send(&start, sizeof start);
        n.delay = cfg.delay; n.clock = cfg.clock;
    } else {
        std::string addr = opt.join ? opt.join : "";
        int port = opt.port;
        const size_t colon = addr.rfind(':');
        if (colon != std::string::npos) { port = atoi(addr.c_str() + colon + 1); addr.resize(colon); }
        addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(addr.c_str(), std::to_string(port).c_str(), &hints, &res) || !res) {
            snprintf(reason, sizeof reason, "the address \"%s\" was not found", addr.c_str());
            return failed(fail(reason));
        }
        memcpy(&g_peer, res->ai_addr, sizeof g_peer);
        freeaddrinfo(res);
        g_have_peer = true;
        fprintf(stderr, "[netplay] joining %s, port %d\n", addr.c_str(), port);
        Hello he = make<Hello>(T_HELLO);
        he.proto = PROTO; he.content = content; memcpy(he.build, BUILD, sizeof BUILD);
        Config cfg = {};
        bool have_cfg = false, have_chunk[NV_CHUNKS] = {}, started = false;
        static uint8_t nvbuf[NVRAM_SIZE];
        double last = -1e9;
        while (!started) {
            for (int got; (got = recv_pkt(buf, sizeof buf, &from)) > 0;) {
                Hdr h;
                if (got < (int)sizeof h || !same_addr(from, g_peer)) continue;
                memcpy(&h, buf, sizeof h);
                if (h.magic != MAGIC) continue;
                if (h.type == T_CONFIG && got >= (int)sizeof(Config)) {
                    memcpy(&cfg, buf, sizeof cfg);
                    cfg.reason[sizeof cfg.reason - 1] = 0;
                    if (!cfg.ok) { snprintf(reason, sizeof reason, "%s", cfg.reason); return failed(fail(reason)); }
                    have_cfg = true;
                } else if (h.type == T_NV && got >= (int)sizeof(NvChunk)) {
                    NvChunk c;
                    memcpy(&c, buf, sizeof c);
                    if (c.index < NV_CHUNKS && c.len == NV_CHUNK) { memcpy(nvbuf + c.index * NV_CHUNK, c.data, NV_CHUNK); have_chunk[c.index] = true; }
                } else if ((h.type == T_START || h.type == T_INPUT) && have_cfg) started = true;
            }
            bool all = have_cfg;
            for (bool b : have_chunk) all &= b;
            if (now_ms() - last > (all ? 100 : 250)) {
                last = now_ms();
                if (all) { Hdr r = {MAGIC, T_READY}; raw_send(&r, sizeof r); }
                else raw_send(&he, sizeof he);
            }
            if (!all) started = false;
            if (now_ms() - t0 > 30000) return failed(fail("no answer from the other player (check the address, and that their port is open)"));
            fe_wait_pump();
            Sleep(2);
        }
        if (nv) memcpy(nv, nvbuf, NVRAM_SIZE);
        hw_set_unlock_silber(cfg.unlock != 0);
        n.delay = cfg.delay; n.clock = cfg.clock;
    }

    n.host = opt.host;
    n.max_frames = opt.max_frames;
    n.local_have = n.remote_have = n.peer_ack = (uint32_t)n.delay;   // frames 1..delay: nothing pressed, by agreement
    n.last_recv = now_ms();
    n.active = true;
    savestate_set_without_sound(true);
    savestate_set_pages_separate(true);
    atexit(finish);
    fprintf(stderr, "[netplay] connected: %s, input delay %d frames\n", n.host ? "hosting (player 1)" : "joined (player 2)", n.delay);
    return true;
}

void netplay_frame(void* ctx, const void* site, bool in_irq) {
    if (!n.active) return;
    const uint32_t frame = hw_frame_number();
    const bool saveable = !in_irq && (uint32_t)((recomp_context*)ctx)->r29 == WAIT_SP;

    if (n.replaying) {
        if (frame < n.replay_until) {
            // Only the states a later rollback (or a memory check) could need.
            if (saveable && (frame > n.remote_have || frame % CHECK_EVERY == 0)) save_slot(frame, site, ctx);
            set_inputs(frame);
            return;
        }
        n.replaying = false;                           // back at the present
        // Sound: what the game sent this time that the board has not had, it gets now.
        for (auto& [f, cmd] : n.late) { sound_inject_command(cmd); tell(f, cmd); }
        n.sounds_late += (uint32_t)n.late.size();
        n.sounds_wrong += (uint32_t)n.pool.size();
        n.late.clear(); n.pool.clear();
        const double took = now_ms() - n.replay_t0;
        n.replay_ms += took;
        n.replay_worst = std::max(n.replay_worst, took);
    }

    if (n.sampled < frame) {                           // first time at this boundary: our controls, for frame + delay
        n.sampled = frame;
        pace(frame);
        const uint16_t bits = input_read_local();
        for (uint32_t f = n.local_have + 1; f <= frame + (uint32_t)n.delay; f++) n.local[f % HIST] = bits;
        n.local_have = frame + (uint32_t)n.delay;
    }
    send_inputs(frame);

    const bool last = n.max_frames && frame >= n.max_frames;
    const double t0 = now_ms();
    double resent = t0, settled_at = 0;
    bool waited = false, told = false;
    for (;;) {
        poll();
        run_checks();
        // A wrong guess: put the machine back and play the frames again. Only where this boundary and the
        // saved one ended at the same place in the game's code; otherwise carry on and try at the next.
        if (n.wrong_from && n.wrong_from < frame) {
            Slot* s = saveable ? newest_at_or_before(n.wrong_from, site) : nullptr;
            if (s) { rollback(s, ctx, frame); break; }
            if (!told) { told = true; n.postponed++; }
        }
        if (last) {
            // Tests (--frames): finish only when every frame before this one is settled on both sides.
            const bool settled = n.remote_have + 1 >= frame && (!n.wrong_from || n.wrong_from >= frame);
            if (settled && !settled_at) settled_at = now_ms();
            if (settled && (n.peer_ack + 1 >= frame || n.peer_left || now_ms() - settled_at > 3000)) break;
            if (n.wrong_from && n.wrong_from < frame && !saveable && now_ms() - t0 > 3000) {
                fprintf(stderr, "[netplay] the last frame is not at a wait point: the result below is not settled\n");
                break;
            }
        } else {
            if (n.peer_left) stop("the other player left");
            if (frame <= n.remote_have) break;                                   // the real controls are here
            const bool anchor = (saveable && frame <= n.remote_have + 1) || newest_at_or_before(n.remote_have + 1, nullptr);
            if (anchor && frame - n.remote_have <= (uint32_t)MAX_PRED) break;     // play on with a guess
        }
        waited = true;
        const double t = now_ms();
        if (t - resent > 8) { send_inputs(frame); resent = t; }
        if (t - n.last_recv > TIMEOUT_MS) stop("the connection was lost");
        fe_wait_pump();
        Sleep(1);
    }
    if (waited) { n.waits++; n.wait_ms += now_ms() - t0; }
    if (n.replaying) return;                           // a rollback has just started (its first frame is set up)

    if (last && !n.final_frame) {
        n.final_frame = frame;
        send_inputs(frame);
    }
    if (saveable) save_slot(frame, site, ctx);
    set_inputs(frame);
}
