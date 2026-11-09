// Support routines the recompiled code calls (declared in recomp.h).
#include <algorithm>
#include <cstdio>
#include "hw.h"

struct FuncTableEntry { uint32_t vram; recomp_func_t* func; };
extern "C" const FuncTableEntry g_func_table[];
extern "C" const size_t g_func_table_size;

extern "C" {

void cop0_status_write(recomp_context* ctx, gpr value) {
    g_cop0_status = (uint32_t)value;
    ctx->status_reg = g_cop0_status;
    ctx->mips3_float_mode = (g_cop0_status >> 26) & 1;
    ctx->f_odd = ctx->mips3_float_mode ? &ctx->f1.u32l : &ctx->f0.u32h;   // see hw_boot()
    g_next_event = g_ticks + 1;          // interrupts may have just been enabled
}

gpr cop0_status_read(recomp_context* ctx) {
    (void)ctx;
    return (gpr)(int32_t)g_cop0_status;
}

void switch_error(const char* func, uint32_t vram, uint32_t jtbl) {
    hw_fatal("jump table at %08X in %s went out of range (table %08X)", vram, func, jtbl);
}

void do_break(uint32_t vram) {
    hw_fatal("MIPS 'break' instruction at %08X (usually a divide by zero check)", vram);
}

void pause_self(uint8_t* rdram) { (void)rdram; }

// Function-pointer calls (jalr) find their target here.
recomp_func_t* get_function(int32_t vram) {
    uint32_t v = (uint32_t)vram;
    // The BIOS service block (BIOS 0x8000-0xBFFF) is reachable at several addresses: its
    // RAM copy at 0x80004000 (KSEG0) / 0xA0004000 (KSEG1), and the ROM itself at
    // 0x9FC08000 / 0xBFC08000. It's recompiled once, at 0x9FC08000.
    if ((v & 0xDFFFC000u) == 0x80004000u || (v & 0xDFFFC000u) == 0x9FC08000u)
        v = 0x9FC08000u | (v & 0x3FFFu);
    const FuncTableEntry* end = g_func_table + g_func_table_size;
    const FuncTableEntry* it = std::lower_bound(g_func_table, end, v,
        [](const FuncTableEntry& e, uint32_t x) { return e.vram < x; });
    if (it != end && it->vram == v) return it->func;
    hw_fatal("indirect call to %08X, which isn't a known function start", v);
}

void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t instruction_vram) {
    (void)rdram; (void)ctx;
    hw_fatal("syscall instruction at %08X", (uint32_t)instruction_vram);
}

} // extern "C"
