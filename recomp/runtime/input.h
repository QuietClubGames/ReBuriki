// Player controls: keyboard + XInput gamepads -> the 8 input ports the HNG64 I/O board reads.
#pragma once
#include <stdint.h>

// Bit layout of MAME's INPUT_PORTS(hng64_fight), all active low (0 = pressed):
//   IN4: P1 up 01, down 02, left 04, right 08, button 1-4 10/20/40/80
//   IN5: P2 up 02, down 04, left 08, right 10, button 1-3 20/40/80
//   IN6: P2 button 4 01
//   IN7: service 01, test 02, coin 1 04, coin 2 08, start 1 40, start 2 80
//   IN0-IN3: unused (0xFF)
struct InputFrame {
    uint8_t port[8];
};

// Sample the host keyboard/gamepads. Called once per emulated frame (at vblank), so the game
// sees one consistent input state per frame. This is also the hook for replays and netplay.
void input_sample_frame(void);
const InputFrame& input_current(void);
void input_print_help(void);

// Replace host input with a script: "frame=control,control;frame=..." (see input.cpp).
void input_set_script(const char* script);
