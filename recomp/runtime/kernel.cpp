// Hand-written versions of the 3 game functions that use CPU control instructions
// N64Recomp can't translate (listed as "ignored" in buriki.toml). Each one mirrors the
// original MIPS code, quoted in the comments.
#include <cstdio>
#include "kernel.h"
#include "hw.h"

extern "C" {

// 800C2420: the game's fatal-exception screen. It saves every register, reads Cause/EPC,
// clears the video hardware and prints a register dump, then loops forever.
void func_800C2420(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    hw_fatal("the game called its crash handler (func_800C2420). ra=%08X sp=%08X",
             (uint32_t)ctx->r31, (uint32_t)ctx->r29);
}

// 800C5C90: write one TLB entry.
//   a0 = index, a1 = page mask, a2 = virtual address (EntryHi),
//   a3 = physical address of the even page, [sp+0x10] = physical address of the odd page.
void func_800C5C90(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    uint32_t phys1 = (uint32_t)MEM_W(0x10, ctx->r29);
    hw_tlb_write((uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6 & 0xFFFFE000u,
                 (uint32_t)ctx->r7, phys1);
}

// 800C5D30: random number generator.
//   mfc0 t3,Count ; sw t3,0x2c40(gp) ; lhu v0,0x2c44(gp) ; lw v1,0x2c40(gp)
//   v1 = (v0 + v1) & 0xff ; v1 = lbu table[0x800DCE00 + v1] ; v0 ^= v1 ; sh v0,0x2c44(gp)
void func_800C5D30(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    uint32_t count = hw_cop0_count();
    ctx->r11 = (gpr)(int32_t)count;
    MEM_W(0x2C40, ctx->r28) = count;
    ctx->r2 = MEM_HU(0x2C44, ctx->r28);
    ctx->r3 = MEM_W(0x2C40, ctx->r28);
    ctx->r3 = (gpr)(int32_t)(uint32_t)(ctx->r2 + ctx->r3) & 0xFF;
    ctx->r1 = (gpr)(int32_t)(0x800E0000u + (uint32_t)ctx->r3);
    ctx->r3 = MEM_BU(-0x3200, ctx->r1);
    ctx->r2 = ctx->r2 ^ ctx->r3;
    MEM_H(0x2C44, ctx->r28) = ctx->r2;
}

// ---- BIOS service routines (BIOS 0x8000+, called through the RAM copy at 0x80004000) ----

// 9FC08100: write a TLB entry. a0 = virtual address (EntryHi), a1 = physical address of the
// even page (odd page = a1 + 16 MB), a2 = index, a3 = page mask.
//   EntryLo0 = (a1 >> 6) | 0x17 ; EntryLo1 = EntryLo0 + 0x40000
void func_9FC08100(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    uint32_t phys0 = (uint32_t)ctx->r5 & ~0xFFFu;
    ctx->r8 = (gpr)(int32_t)(uint32_t)ctx->r4;      // leaves t0 = a0, as the original does
    ctx->r10 = 0x40000;                              // t2
    hw_tlb_write((uint32_t)ctx->r6, (uint32_t)ctx->r7, (uint32_t)ctx->r4, phys0, phys0 + 0x01000000u);
}

// 9FC09300: turn caches on (Config, no-op here), map 4 TLB pairs, then write the
// framebuffer control bytes at 0xD0000000 = 01, [2] = 00, [3] = 02. Returns via t1 = ra.
void func_9FC09300(uint8_t* rdram, recomp_context* ctx) {
    gpr ra = ctx->r31;
    ctx->r9 = ra;                                    // t1
    ctx->r23 = 8;                                    // s7
    struct { uint32_t va, pa, index; } maps[] = {
        {0xC0000000u, 0x20000000u, 0}, {0xD0000000u, 0x30000000u, 1},
        {0xE0000000u, 0x60000000u, 4}, {0x60000000u, 0xC0000000u, 7},
    };
    for (auto& m : maps) {
        ctx->r4 = (gpr)(int32_t)m.va; ctx->r5 = (gpr)(int32_t)m.pa;
        ctx->r6 = m.index;            ctx->r7 = 0x01FFE000;
        func_9FC08100(rdram, ctx);
    }
    ctx->r12 = 2; ctx->r10 = (gpr)(int32_t)0xD0000000u; ctx->r11 = 1;
    MEM_B(0, ctx->r10) = 1;
    MEM_B(2, ctx->r10) = 0;
    MEM_B(3, ctx->r10) = 2;
    ctx->r31 = ra;
}

// 9FC09B04: set the cache mode in Config. Caches don't exist in the recomp: nothing to do.
void func_9FC09B04(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }

// 9FC09B24: invalidate/write back both caches, set the cache mode. Nothing to do.
void func_9FC09B24(uint8_t* rdram, recomp_context* ctx) { (void)rdram; (void)ctx; }

} // extern "C"
