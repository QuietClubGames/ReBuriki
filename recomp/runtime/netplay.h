// Online play for two players: rollback netcode over a direct UDP link (netplay.cpp).
//
// Both copies run the whole game from power-on with the same save memory, options and clock, and feed it the
// same controls for every frame: each player's own, sent to the other and applied a few frames later
// (the input delay). While the other player's controls for a frame have not arrived yet the game carries on
// with a guess (the last ones seen); when the real ones turn out different, the machine is put back to its
// saved state from that frame and the frames since are played again, not drawn (a rollback).
// See NOTES.md, "Online play".
#pragma once
#include <stdint.h>

struct NetplayOptions {
    bool host = false;            // --netplay-host PORT
    const char* join = nullptr;   // --netplay-join ADDRESS[:PORT]
    int port = 7600;
    int delay = 2;                // --netplay-delay N: frames between pressing and the game seeing it (host decides)
    uint32_t max_frames = 0;      // --frames (tests): stop there, once every frame before it is settled
    bool unlock_silber = false;   // the host's setting; the joiner gets it from the host
};

// After hw_init (the ROMs are loaded) and after the host's save memory is loaded, before the game boots.
// Waits for the other player, checks both have the same game and build, and gives the joiner the host's
// save memory, options and clock. Returns false (with a message in *error) if that fails.
bool netplay_start(const NetplayOptions& opt, const char** error);

bool netplay_active(void);
bool netplay_replaying(void);     // inside a rollback: nothing is drawn, the sound board does not run
bool netplay_is_host(void);

// hw_events, once per frame, where the controls are read. May put the machine back to an earlier frame.
// `site` is where in the recompiled code the frame ended (see savestate.h).
void netplay_frame(void* recomp_ctx, const void* site, bool in_irq);
// input.cpp: the two players' controls for the frame that is starting (bit i = control i of input.cpp's list).
void netplay_frame_inputs(uint16_t bits[2]);
// The agreed clock: seconds since 1970 as the host's local time, advancing with the frame number.
int64_t netplay_clock(uint32_t frame);
// While this copy stands still on purpose (its pause menu is open): keeps telling the other copy it is
// there, so that one waits instead of deciding the connection is lost.
void netplay_idle(void);
// For the Online page: who this player is, the ping, the rollbacks ("" when not playing online); the ping
// in ms (-1 unknown); and this PC's address on its own network.
const char* netplay_status(void);
int netplay_ping(void);
const char* netplay_local_address(void);

// sound.cpp: the game has sent the sound board a command (the latch's contents when the command interrupt
// is raised). During a rollback the board is not given it; see "Sound" in netplay.cpp.
void netplay_sound_command(uint32_t latch);
// sound.cpp, for netplay: give the board a command now.
void sound_inject_command(uint32_t latch);

// input.cpp, for netplay: this player's own controls (the Player 1 keys / pad, or the input script's p1
// names) as bits.
uint16_t input_read_local(void);
