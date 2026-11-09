// Memory access layer used by the recompiled code.
//
// Every load/store in the generated C goes through MEM_W/MEM_H/MEM_B/... (see recomp.h).
// Main RAM accessed through KSEG0/KSEG1 (0x80000000 / 0xA0000000) takes an inline fast
// path. Everything else (hardware registers, video RAM, ROM, TLB-mapped addresses) goes
// to hw_read*/hw_write* in hw.cpp.
//
// Storage layout: all memory is kept as host-native 32-bit words, the same convention
// N64Recomp uses. A byte at address A lives at base[A ^ 3], a halfword at base[A ^ 2].
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t* g_ram;                 // 16 MB main RAM
#define HW_RAM_SIZE 0x01000000u

uint32_t hw_read32(uint32_t vaddr);
uint16_t hw_read16(uint32_t vaddr);
uint8_t  hw_read8(uint32_t vaddr);
void hw_write32(uint32_t vaddr, uint32_t value);
void hw_write16(uint32_t vaddr, uint16_t value);
void hw_write8(uint32_t vaddr, uint8_t value);

#ifdef __cplusplus
}
#endif

// Offset into g_ram for a KSEG0/KSEG1 address that hits main RAM, or -1.
static inline int64_t hw_ram_offset(uint32_t a) {
    if ((a >> 30) == 2u) {                       // 0x80000000 - 0xBFFFFFFF
        uint32_t p = a & 0x1FFFFFFFu;
        if (p < HW_RAM_SIZE) return (int64_t)p;
    }
    return -1;
}

static inline uint32_t mem_r32(uint32_t a) {
    int64_t o = hw_ram_offset(a);
    return o >= 0 ? *(uint32_t*)(g_ram + o) : hw_read32(a);
}
static inline uint16_t mem_r16(uint32_t a) {
    int64_t o = hw_ram_offset(a);
    return o >= 0 ? *(uint16_t*)(g_ram + (o ^ 2)) : hw_read16(a);
}
static inline uint8_t mem_r8(uint32_t a) {
    int64_t o = hw_ram_offset(a);
    return o >= 0 ? g_ram[o ^ 3] : hw_read8(a);
}
static inline void mem_w32(uint32_t a, uint32_t v) {
    int64_t o = hw_ram_offset(a);
    if (o >= 0) *(uint32_t*)(g_ram + o) = v; else hw_write32(a, v);
}
static inline void mem_w16(uint32_t a, uint16_t v) {
    int64_t o = hw_ram_offset(a);
    if (o >= 0) *(uint16_t*)(g_ram + (o ^ 2)) = v; else hw_write16(a, v);
}
static inline void mem_w8(uint32_t a, uint8_t v) {
    int64_t o = hw_ram_offset(a);
    if (o >= 0) g_ram[o ^ 3] = v; else hw_write8(a, v);
}

#ifdef __cplusplus
// Proxy objects so that the generated "MEM_W(off, reg) = value;" statements and
// "x = MEM_W(off, reg);" reads both route through the functions above.
#define HW_MEM_PROXY(Name, ValueT, ReadExpr, WriteExpr)                              \
    struct Name {                                                                     \
        uint32_t a;                                                                   \
        operator ValueT() const { return (ValueT)(ReadExpr); }                        \
        template <class T> void operator=(T v) const { WriteExpr; }                   \
        void operator=(const Name& o) const { ValueT v = (ValueT)o; WriteExpr; }      \
    };

HW_MEM_PROXY(MemRef32,  int32_t,  mem_r32(a), mem_w32(a, (uint32_t)(uint64_t)v))
HW_MEM_PROXY(MemRef16,  int16_t,  mem_r16(a), mem_w16(a, (uint16_t)(uint64_t)v))
HW_MEM_PROXY(MemRef16U, uint16_t, mem_r16(a), mem_w16(a, (uint16_t)(uint64_t)v))
HW_MEM_PROXY(MemRef8,   int8_t,   mem_r8(a),  mem_w8(a, (uint8_t)(uint64_t)v))
HW_MEM_PROXY(MemRef8U,  uint8_t,  mem_r8(a),  mem_w8(a, (uint8_t)(uint64_t)v))
#undef HW_MEM_PROXY
#endif
