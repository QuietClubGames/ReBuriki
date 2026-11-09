// Hand-written replacements for functions that use CPU control instructions.
// N64Recomp skips these (see [patches] ignored in buriki.toml), so the runtime provides them
// (runtime/kernel.cpp). tools/find_kernel_funcs.py lists which functions need this.
#ifndef BURIKI_KERNEL_H
#define BURIKI_KERNEL_H

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

// Game code
void func_800C2420(uint8_t* rdram, recomp_context* ctx); // fatal exception screen
void func_800C5C90(uint8_t* rdram, recomp_context* ctx); // write a TLB entry
void func_800C5D30(uint8_t* rdram, recomp_context* ctx); // random number generator

// BIOS service routines
void func_9FC08100(uint8_t* rdram, recomp_context* ctx); // write a TLB entry
void func_9FC09300(uint8_t* rdram, recomp_context* ctx); // cache mode + TLB setup + fb control
void func_9FC09B04(uint8_t* rdram, recomp_context* ctx); // set cache mode
void func_9FC09B24(uint8_t* rdram, recomp_context* ctx); // flush caches + set cache mode

#ifdef __cplusplus
}
#endif

#endif
