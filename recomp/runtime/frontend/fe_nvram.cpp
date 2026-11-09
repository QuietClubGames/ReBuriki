// The arcade's save memory: the 16 KB nvram at physical 0x1F800000 (operator settings, high scores,
// bookkeeping) is kept in nvram.bin next to the exe. Loaded once after hw_init, before the game boots; saved
// when it has changed (checked once a second) and at exit. The file holds the bytes in the board's own
// (big-endian) order.
#include "frontend.h"
#include "fe_internal.h"
#include "../hw.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static constexpr uint32_t NVRAM_PHYS = 0x1F800000, NVRAM_SIZE = 0x4000;
static std::string g_path;
static bool g_on;
static uint8_t g_saved[NVRAM_SIZE];
static ULONGLONG g_last_check;

static void snapshot(uint8_t* out) {
    const uint32_t* w = hw_phys_words(NVRAM_PHYS);
    for (uint32_t i = 0; i < NVRAM_SIZE / 4; i++) {
        out[i * 4] = (uint8_t)(w[i] >> 24); out[i * 4 + 1] = (uint8_t)(w[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(w[i] >> 8); out[i * 4 + 3] = (uint8_t)w[i];
    }
}

static bool write_file(const uint8_t* data) {
    std::string tmp = g_path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, NVRAM_SIZE, f) == NVRAM_SIZE;
    ok = fclose(f) == 0 && ok;
    // replace in one step, so a crash mid-write never leaves a half-written file
    return ok && MoveFileExA(tmp.c_str(), g_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static void at_exit() { fe_nvram_save(); }

void fe_nvram_init(const char* path) {
    if (!path || !hw_phys_words(NVRAM_PHYS)) return;
    g_path = path;
    g_on = true;
    uint32_t* w = hw_phys_words(NVRAM_PHYS);
    FILE* f = fopen(path, "rb");
    if (f) {
        uint8_t b[NVRAM_SIZE];
        size_t n = fread(b, 1, NVRAM_SIZE, f);
        fclose(f);
        if (n == NVRAM_SIZE) {
            for (uint32_t i = 0; i < NVRAM_SIZE / 4; i++)
                w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 | (uint32_t)b[i * 4 + 2] << 8 | b[i * 4 + 3];
            fe_log("[nvram] loaded %s", path);
        } else {
            fe_log("[nvram] %s is %zu bytes, not %u; starting with blank save memory", path, n, NVRAM_SIZE);
        }
    } else {
        fe_log("[nvram] no %s yet; the game will set up its save memory", path);
    }
    snapshot(g_saved);
    g_last_check = GetTickCount64();
    atexit(at_exit);
}

void fe_nvram_save(void) {
    if (!g_on) return;
    uint8_t now[NVRAM_SIZE];
    snapshot(now);
    if (!memcmp(now, g_saved, NVRAM_SIZE)) return;
    if (write_file(now)) memcpy(g_saved, now, NVRAM_SIZE);
    else fe_log("[nvram] couldn't write %s", g_path.c_str());
}

void fe_nvram_tick(void) {
    if (!g_on) return;
    ULONGLONG t = GetTickCount64();
    if (t - g_last_check < 1000) return;
    g_last_check = t;
    fe_nvram_save();
}

const char* fe_nvram_path(void) { return g_on ? g_path.c_str() : nullptr; }

const char* fe_default_nvram_path(void) {
    static std::string p = fe_path("nvram.bin");
    return p.c_str();
}
