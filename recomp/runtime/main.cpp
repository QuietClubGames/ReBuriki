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
//     --roms DIR            folder with buriki/ and hng64/ (default: "roms" next to the exe, as
//                           installed, or one folder up, as in the source tree)
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
// Without a console (the installed game is built as a Windows program) everything printed goes to
// buriki.log next to the exe (the run before is kept as buriki.old.log), and a fatal error or crash
// shows a message box.
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
static bool g_log_to_file = false;     // no console: stderr goes to buriki.log

static std::string exe_dir() {
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, exe, sizeof exe);
    while (n > 0 && exe[n - 1] != '\\' && exe[n - 1] != '/') n--;
    return std::string(exe, n);
}

static void fatal_box(const char* message) {
    if (!g_log_to_file) return;
    std::string text = std::string("Buriki One has to stop:\n\n") + message +
                       "\n\nThe details are in buriki.log, in the game's folder.";
    MessageBoxA(nullptr, text.c_str(), "Buriki One", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS* e) {
    fprintf(stderr, "\n[CRASH] host exception %08lX at %p\n",
            e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    hw_dump_recent_functions();
    hw_report();
    fflush(stderr);
    char msg[96];
    snprintf(msg, sizeof msg, "the program crashed (error %08lX).", e->ExceptionRecord->ExceptionCode);
    fatal_box(msg);
    return EXCEPTION_EXECUTE_HANDLER;
}

// The installed game has no console. Send everything it prints to buriki.log next to the exe.
static void log_to_file_if_no_console() {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h != nullptr && h != INVALID_HANDLE_VALUE) return;      // a console, or redirected by a test
    const std::string log = exe_dir() + "buriki.log", old = exe_dir() + "buriki.old.log";
    MoveFileExA(log.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (freopen(log.c_str(), "w", stderr)) g_log_to_file = true;
    freopen("NUL", "w", stdout);
}

// "roms" next to the exe (installed), else one folder up (the source tree: recomp\buriki.exe, roms\).
static std::string default_rom_dir() {
    for (const char* rel : {"roms", "..\\roms"}) {
        const std::string d = exe_dir() + rel;
        const DWORD a = GetFileAttributesA(d.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return d;
    }
    return exe_dir() + "roms";
}
#else
static std::string default_rom_dir() { return "roms"; }
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    log_to_file_if_no_console();
    g_fatal_hook = fatal_box;
#endif
    HwConfig cfg = {};
    static const std::string rom_dir_default = default_rom_dir();
    cfg.rom_dir = rom_dir_default.c_str();
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
        else if (arg("--shot-every")) { fe.shot_every = (uint32_t)atoi(argv[++i]); fe.no_save = true; }
        else if (arg("--shot-dir")) fe.shot_dir = argv[++i];
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
    // Unlock Silber: the menu's setting with a window (applied by the front end when the window opens);
    // headless runs leave the game as the arcade has it unless asked with --set unlock_silber=1.
    for (const char* o : overrides)
        if (g_video_cfg.headless && !strcmp(o, "unlock_silber=1")) hw_set_unlock_silber(true);
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
