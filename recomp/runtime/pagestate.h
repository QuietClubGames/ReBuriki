// Cheap copies of the machine's large memory areas, for the save states online play makes every frame
// (pagestate.cpp). Windows keeps a list of the 4 KB pages a program has written to ("write watch"); with
// it, bringing a copy up to date, or putting one back, only touches the pages that changed in between:
// about 1 MB in a fight instead of 20 MB.
#pragma once
#include <stdint.h>
#include <vector>

// For hw.cpp: zeroed memory for a large writable area, watched for writes. Areas come back in the order
// they were asked for (main RAM first), and are laid out in a PageCopy in that order.
uint8_t* pagestate_alloc(uint32_t size);
bool pagestate_owns(const uint8_t* data);

// One saved copy of all the watched areas.
struct PageCopy {
    std::vector<uint8_t> data;
    uint32_t epoch = 0;        // when it was last brought up to date (0 = never)
};
void pagestate_save(PageCopy& copy);         // make `copy` equal to the live memory
void pagestate_load(const PageCopy& copy);   // make the live memory equal to `copy`
uint32_t pagestate_last_pages(void);         // how many pages the last save or load copied (statistics)
