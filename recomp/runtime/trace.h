// N64Recomp's trace mode inserts TRACE_ENTRY() at the start of every recompiled function
// and TRACE_RETURN() before each return. We use the entry hook as the runtime's safe point:
// it advances virtual time and delivers pending interrupts (see hw.cpp).
#pragma once
#include "hw.h"

#define TRACE_ENTRY() hw_on_function_entry(rdram, ctx, __func__);
#define TRACE_RETURN()

// Added after every branch label by tools/fix_generated.py, so loops without calls still
// advance time and can take interrupts.
#define HW_LABEL_TICK() hw_on_label(rdram, ctx);
