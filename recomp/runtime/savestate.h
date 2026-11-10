// Save states: the whole emulated machine copied out to a buffer and back (savestate.cpp).
//
// What this is for: rollback netplay needs to return the machine to an earlier frame and replay from there.
// This is the first step, a feasibility test (env BURIKI_SAVETEST, see savestate.cpp), not a player feature.
//
// The recompiled game is native code on the host's own stack, so a state can only be taken and put back
// where the host stack is the same both times: at the end of a frame that arrives while the game waits in
// its main loop (func_800080B0, label L_800080F0), which is every frame of a fight. See NOTES.md.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

struct StateIO {
    std::vector<uint8_t>& buf;
    bool saving;
    size_t pos = 0;
    // Saving writes at `pos` too (the buffer is reused from one save to the next and grows only once);
    // the caller trims it to `pos` afterwards.
    void raw(void* p, size_t n) {
        if (saving) {
            if (buf.size() < pos + n) buf.resize(pos + n);
            memcpy(buf.data() + pos, p, n);
        } else memcpy(p, buf.data() + pos, n);
        pos += n;
    }
    template <class T> void pod(T& v) { raw(&v, sizeof v); }
};

// Each module saves or loads what belongs to it (same order both ways).
struct recomp_context_tag;
void hw_state(StateIO& io, void* recomp_ctx);   // memory regions, CPU registers, interrupts, timing
void sound_state(StateIO& io);                  // sound CPU, sound chip, their timing
void io_board_state(StateIO& io);               // I/O board microcontroller
void input_state(StateIO& io);                  // control latches and coin pulses

// Called by hw_events at the end of each frame (after the frame's own work, before the vblank interrupt is
// delivered). `site` is where in the recompiled code the frame ended.
void savestate_frame(void* recomp_ctx, const void* site, bool in_irq);
// The replay half of the test doesn't draw (as a rollback wouldn't): hw_events asks before video_frame().
bool savestate_skip_video(void);
// The test's "nosound" option: the sound board is left out of the saved state (savestate_without_sound) and
// is not run during the replays (savestate_skip_sound).
bool savestate_skip_sound(void);
bool savestate_without_sound(void);
