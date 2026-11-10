// Cheap copies of the machine's large memory areas (pagestate.h).
//
// Every page has a stamp: the "epoch" in which it was last seen to have changed. The epoch goes up by one
// after every save and every load. A copy remembers the epoch it was made in, so the pages where the live
// memory can differ from a copy are exactly the ones with a newer stamp:
//   - the game wrote to the page since (Windows' write watch reports it, note_written() stamps it), or
//   - a load put other contents into it since (pagestate_load stamps what it copies).
#define NOMINMAX
#include <windows.h>

#include "pagestate.h"

#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t PAGE = 4096;
struct Area { uint8_t* data; uint32_t size, first_page; };
std::vector<Area> g_areas;
std::vector<uint32_t> g_stamp;        // per page, all areas one after another
std::vector<void*> g_written;         // scratch for GetWriteWatch
uint32_t g_epoch = 2;                 // stamps start at 1, so a copy that was never made (epoch 0) takes every page
uint32_t g_total = 0;                 // bytes in all areas
uint32_t g_last_pages = 0;

// Stamp the pages written since the last call (and start the watch again).
void note_written(bool stamp) {
    for (const Area& a : g_areas) {
        ULONG_PTR count = a.size / PAGE;
        DWORD granularity = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, a.data, a.size, g_written.data(), &count, &granularity) != 0) {
            // can't happen for memory from pagestate_alloc; be safe: treat everything as written
            if (stamp) for (uint32_t i = 0; i < a.size / PAGE; i++) g_stamp[a.first_page + i] = g_epoch;
            continue;
        }
        if (stamp)
            for (ULONG_PTR i = 0; i < count; i++)
                g_stamp[a.first_page + (uint32_t)(((uint8_t*)g_written[i] - a.data) / PAGE)] = g_epoch;
    }
}

} // namespace

uint8_t* pagestate_alloc(uint32_t size) {
    size = (size + PAGE - 1) & ~(PAGE - 1);
    // BURIKI_NOWATCH=1: plain memory, in case the write watch ever misbehaves (states then copy everything)
    if (getenv("BURIKI_NOWATCH")) return (uint8_t*)calloc(1, size);
    uint8_t* p = (uint8_t*)VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (!p) return (uint8_t*)calloc(1, size);          // not watched: hw_state copies it whole, as before
    g_areas.push_back({p, size, (uint32_t)g_stamp.size()});
    g_stamp.resize(g_stamp.size() + size / PAGE, 1);
    if (g_written.size() < size / PAGE) g_written.resize(size / PAGE);
    g_total += size;
    return p;
}

bool pagestate_owns(const uint8_t* data) {
    for (const Area& a : g_areas) if (a.data == data) return true;
    return false;
}

void pagestate_save(PageCopy& copy) {
    note_written(true);
    if (copy.data.size() != g_total) { copy.data.resize(g_total); copy.epoch = 0; }
    uint32_t done = 0;
    uint8_t* out = copy.data.data();
    for (const Area& a : g_areas) {
        for (uint32_t i = 0; i < a.size / PAGE; i++)
            if (g_stamp[a.first_page + i] > copy.epoch) { memcpy(out + i * PAGE, a.data + i * PAGE, PAGE); done++; }
        out += a.size;
    }
    copy.epoch = g_epoch++;
    g_last_pages = done;
}

void pagestate_load(const PageCopy& copy) {
    if (copy.data.size() != g_total || !copy.epoch) return;
    note_written(true);
    uint32_t done = 0;
    const uint8_t* in = copy.data.data();
    for (const Area& a : g_areas) {
        for (uint32_t i = 0; i < a.size / PAGE; i++)
            if (g_stamp[a.first_page + i] > copy.epoch) {
                memcpy(a.data + i * PAGE, in + i * PAGE, PAGE);
                g_stamp[a.first_page + i] = g_epoch;       // changed now, as far as every other copy is concerned
                done++;
            }
        in += a.size;
    }
    note_written(false);     // those were our own writes, already stamped
    g_epoch++;
    g_last_pages = done;
}

uint32_t pagestate_last_pages(void) { return g_last_pages; }
