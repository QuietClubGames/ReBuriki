// Buriki One recomp: entry point.
//   buriki.exe [options]
//     --frames N            stop after N frames (default: run until the window is closed)
//     --headless            no window, run as fast as possible
//     --no-throttle         window, but don't limit to 60 fps
//     --scale N             window N x 448 lines tall for this run (default: settings.ini)
//     --dump-dir DIR        save frames as .bmp into DIR, chosen with:
//     --dump-frames A,B,C   ...these frame numbers
//     --dump-every N        ...every Nth frame
//     --input-script S      scripted controls instead of keyboard/pad, e.g.
//                           "600=coin1;604=;700=p1start;704=" (see input.cpp)
//     --wav FILE            also record the audio to a WAV file
//     --no-sound            don't play audio
//     --ticks-per-frame N   virtual time per frame (default 50000)
//     --io-log N            print the first N distinct hardware accesses (default 300)
//     --trace-io            print every hardware access
//     --roms DIR            folder with buriki/ and hng64/ (default D:/Buriki/roms)
//     --bios FILE           BIOS in hng64/ (default: the menu's region, USA; headless: brom1.bin, Japan)
//     --nvram FILE          keep the arcade's save memory in FILE (default: nvram.bin next to the exe
//                           with a window; off when headless)
//     --no-nvram            don't load or save the save memory
//     --set KEY=VALUE       change a settings.ini value for this run only (not saved)
//     --menu-test PAGE      tests: at frame --menu-test-at (default 600) open the menu on PAGE (display,
//                           audio, controls, game, system, capture; add ":pad" for controller hints; or
//                           "none" / "toast"), save the window's picture to --menu-shot FILE.bmp, quit.
//                           "pause": keep the menu open 4 s, save the picture, close it and carry on.
//     --test-keys "N:key,..."  tests: press keys (esc tab enter up down left right q e f9 f11) at displayed
//                           frame N (frames keep counting while the menu is open). Doesn't save settings.
//                           Doesn't write settings, controls or save memory.
//
// Window keys: Tab / Esc menu (or Back + Start on a pad), Alt+Enter / F11 full screen, F2 Test, 9 Service.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#include "hw.h"
#include "video.h"
#include "input.h"
#include "funcs.h"
#include "frontend/frontend.h"

static recomp_context g_ctx;

#ifdef _WIN32
static LONG WINAPI on_crash(EXCEPTION_POINTERS* e) {
    fprintf(stderr, "\n[CRASH] host exception %08lX at %p\n",
            e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    hw_dump_recent_functions();
    hw_report();
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char** argv) {
    HwConfig cfg = {};
    cfg.rom_dir = "D:/Buriki/roms";
    cfg.max_frames = 0;
    cfg.ticks_per_frame = 50000;
    cfg.io_log_limit = 300;
    bool no_sound = false;
    FeOptions fe;
    const char* nvram = nullptr;
    bool no_nvram = false, bios_given = false;
    std::vector<const char*> overrides;
    for (int i = 1; i < argc; i++) {
        auto arg = [&](const char* name) { return !strcmp(argv[i], name) && i + 1 < argc; };
        if (arg("--frames")) cfg.max_frames = (uint32_t)atoi(argv[++i]);
        else if (arg("--ticks-per-frame")) cfg.ticks_per_frame = (uint32_t)atoi(argv[++i]);
        else if (arg("--io-log")) cfg.io_log_limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace-io")) cfg.trace_io = 1;
        else if (arg("--roms")) cfg.rom_dir = argv[++i];
        else if (!strcmp(argv[i], "--headless")) g_video_cfg.headless = true;
        else if (!strcmp(argv[i], "--no-throttle")) g_video_cfg.throttle = false;
        else if (arg("--scale")) g_video_cfg.scale = atoi(argv[++i]);
        else if (arg("--dump-dir")) g_video_cfg.dump_dir = argv[++i];
        else if (arg("--dump-every")) g_video_cfg.dump_every = (uint32_t)atoi(argv[++i]);
        else if (arg("--input-script")) input_set_script(argv[++i]);
        else if (arg("--wav")) cfg.wav_path = argv[++i];
        else if (arg("--bios")) { cfg.bios_file = argv[++i]; bios_given = true; }
        else if (arg("--nvram")) nvram = argv[++i];
        else if (!strcmp(argv[i], "--no-nvram")) no_nvram = true;
        else if (arg("--set")) overrides.push_back(argv[++i]);
        else if (arg("--menu-test")) fe.menu_test = argv[++i];
        else if (arg("--menu-test-at")) fe.menu_test_at = (uint32_t)atoi(argv[++i]);
        else if (arg("--menu-shot")) fe.menu_shot = argv[++i];
        else if (arg("--test-keys")) { fe.test_keys = argv[++i]; fe.no_save = true; }
        else if (!strcmp(argv[i], "--no-sound")) no_sound = true;
        else if (arg("--dump-frames")) {
            for (char* t = strtok(argv[++i], ","); t; t = strtok(nullptr, ",")) g_video_cfg.dump_frames.push_back((uint32_t)atoi(t));
        }
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 1; }
    }
    cfg.sound_play = !no_sound && !g_video_cfg.headless;
    setvbuf(stderr, nullptr, _IOLBF, 1 << 16);
#ifdef _WIN32
    SetUnhandledExceptionFilter(on_crash);
#endif

    // Front end: settings.ini / controls.ini next to the exe. Headless runs keep their old defaults (Japanese
    // BIOS, no save memory) so automated runs don't depend on what a player last chose.
    fe.headless = g_video_cfg.headless;
    fe.throttle = g_video_cfg.throttle;
    fe.scale = g_video_cfg.scale;
    fe_init(fe);
    for (const char* o : overrides)
        if (!fe_override_setting(o)) fprintf(stderr, "[frontend] --set %s: unknown setting\n", o);
    if (!bios_given && !g_video_cfg.headless) {
        static std::string bios;
        bios = fe_bios_file();
        std::string path = std::string(cfg.rom_dir) + "/hng64/" + bios;
        if (FILE* f = fopen(path.c_str(), "rb")) { fclose(f); cfg.bios_file = bios.c_str(); }
        else fprintf(stderr, "[frontend] %s not found, booting with brom1.bin (Japan)\n", path.c_str());
    }
    fe_note_running_bios(cfg.bios_file);
    fprintf(stderr, "[boot] BIOS %s\n", cfg.bios_file ? cfg.bios_file : "brom1.bin");

    hw_init(&cfg);
    // The arcade's save memory (operator settings, high scores), before the game boots and reads it.
    static std::string nvram_default;
    if (!nvram && !g_video_cfg.headless && !fe.menu_test) { nvram_default = fe_default_nvram_path(); nvram = nvram_default.c_str(); }
    if (!no_nvram) fe_nvram_init(nvram);
    hw_boot(&g_ctx);

    // Watchdog: if no recompiled function or loop label is reached for 3 s, the game is
    // stuck somewhere time can't advance. Report where.
    std::thread([] {
        uint64_t last = ~0ull, last_fe = 0;
        int still = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uint64_t now = g_ticks, fe_now = fe_activity();   // the pause menu stops game time on purpose
            still = (now == last && fe_now == last_fe) ? still + 1 : 0;
            last = now;
            last_fe = fe_now;
            if (still >= 3) hw_fatal("game stopped making progress for 3 s (stuck)");
        }
    }).detach();

    func_80008000(g_ram, &g_ctx);
    hw_fatal("game entry point returned");
}
