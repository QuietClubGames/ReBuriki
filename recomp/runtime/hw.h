// Hyper Neo Geo 64 hardware layer for the Buriki One recomp.
// Memory map and register behaviour follow MAME's hng64 driver (third_party/mame/src/mame/snk).
#pragma once
#include <stddef.h>
#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- timing --------------------------------------------------------------------------
// "Ticks" = recompiled function entries. Time advances only with game work, so a run is
// fully deterministic (needed later for rollback netplay).
extern uint64_t g_ticks;
extern uint64_t g_next_event;          // tick at which hw_events() must run next
void hw_events(uint8_t* rdram, recomp_context* ctx);

// ---- diagnostics ---------------------------------------------------------------------
#define HW_FUNC_RING 64
extern const char* g_func_ring[HW_FUNC_RING];
extern uint32_t g_func_ring_pos;
void hw_dump_recent_functions(void);
void hw_report(void);                  // prints the hardware access summary
// "Unlock Silber" (menu: Game): the hidden twelfth fighter is selectable whatever the save memory says.
// Takes effect the next time the player select screen starts. Off = the arcade's own rule.
void hw_set_unlock_silber(bool on);
// The arcade's own operator settings (Test menu: GAME OPTIONS and COIN/CREDIT SETTING), for the pause menu's
// Game and Rules pages. Values are the game's own numbers (hw.cpp has the table).
enum HwGameOption { GOPT_FREE_PLAY, GOPT_DIFFICULTY, GOPT_DEMO_SOUND, GOPT_COMMAND_DISPLAY, GOPT_ATTACK_CPU,
                    GOPT_ATTACK_VS, GOPT_TIME_CPU, GOPT_TIME_VS, GOPT_REPLAY, GOPT_CONTINUE, GOPT_BLOOD, GOPT_COUNT };
bool hw_game_options_ready(void);           // false until the game has set up / loaded its settings
int  hw_game_option_get(int option);        // -1 when not ready
void hw_game_option_set(int option, int value);   // saved (nvram) and applied to the running game
[[noreturn]] void hw_fatal(const char* fmt, ...);
// Called with the message just before hw_fatal exits (main.cpp shows it in a message box when the
// game runs without a console, as the installed game does).
extern void (*g_fatal_hook)(const char* message);
// Claims the one fatal-error report. false = another thread is already reporting one (and will exit): the
// caller should just wait. Stops a second message appearing while the first is still on screen.
bool hw_fatal_claim(void);
bool hw_fatal_busy(void);

// ---- setup ---------------------------------------------------------------------------
struct HwConfig {
    const char* rom_dir;               // folder holding buriki/ and hng64/ ROM folders
    uint32_t max_frames;               // stop after this many vblanks (0 = run forever)
    uint32_t ticks_per_frame;
    int io_log_limit;                  // how many distinct hardware addresses to print
    int trace_io;                      // print every hardware access
    int sound_play;                    // play audio through the speakers
    const char* wav_path;              // also record audio to this WAV file (or nullptr)
    const char* bios_file;             // BIOS file in hng64/ (nullptr = brom1.bin, Japan)
};
void hw_init(const struct HwConfig* cfg);
void hw_boot(recomp_context* ctx);     // does what the BIOS does, then returns (caller jumps to game)
uint32_t hw_cart_read32(uint32_t offset);

// Direct access to a physical memory area (host-native 32-bit words), for the renderer.
// Returns nullptr if nothing is mapped there.
uint32_t* hw_phys_words(uint32_t phys);
uint32_t hw_frame_number(void);

// Called once per vblank, before the vblank interrupt is raised (video.cpp).
void video_init(const char* rom_dir);
void video_frame(void);

// Raise main-CPU interrupt bits (MAME set_irq), e.g. 0x20000 = level 17, I/O board.
void hw_raise_irq(uint32_t bits);

// I/O board (io_board.cpp)
void io_board_init(const char* rom_dir, uint32_t ticks_per_frame);
void io_board_command_irq(void);
void io_board_run(void);

// Sound board (sound.cpp)
void sound_init(const char* rom_dir, uint32_t ticks_per_frame, int play, const char* wav_path);
void sound_run(void);
void sound_frame(void);
uint32_t sound_main_read(uint32_t pa);
void sound_main_write(uint32_t pa, uint32_t v, uint32_t mask);
void sound_report(void);

// 3D (video3d.cpp)
void video3d_init(const char* rom_dir);
void video3d_dl_upload(uint32_t data);
void video3d_clear(void);
uint64_t video3d_bad_geometry(void);

// ---- CPU-side state used by kernel.cpp / stubs.cpp -----------------------------------
extern uint32_t g_cop0_status;
void hw_tlb_write(uint32_t index, uint32_t pagemask, uint32_t entryhi, uint32_t phys0, uint32_t phys1);
uint32_t hw_cop0_count(void);

#ifdef __cplusplus
}
#endif

static inline void hw_on_function_entry(uint8_t* rdram, recomp_context* ctx, const char* name) {
    g_func_ring[g_func_ring_pos++ & (HW_FUNC_RING - 1)] = name;
    if (++g_ticks >= g_next_event) hw_events(rdram, ctx);
}

static inline void hw_on_label(uint8_t* rdram, recomp_context* ctx) {
    if (++g_ticks >= g_next_event) hw_events(rdram, ctx);
}
