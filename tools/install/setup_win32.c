/*
 * Setup: builds Buriki One for PC from the player's own ROMs and installs it.
 *
 * Adapted from ReLayer's Setup (tools/install/setup_win32.c), Copyright (c) 2026 The ReLayer project, MIT License
 * (licenses/ReLayer-MIT.txt). Changes: two ROM sets (the game and the Hyper Neo Geo 64 system ROMs), a dark
 * window drawn by Setup itself with an optional picture down its right-hand side, names from product.h (written
 * by make_package.py from product.py).
 *
 * Layout of the Setup folder (made by tools/install/make_package.py):
 *     Setup.exe
 *     files/python/      Python (standard library only)
 *     files/clang/       64-bit clang++ + linker + Windows and C++ headers/libraries
 *     files/n64recomp/   N64Recomp.exe (the MIPS-to-C translator) and its recomp.h
 *     files/source/      this project's source (no ROM data)
 *     files/setup_art.png (optional) the picture on the right of the window
 *
 * Setup runs files/source/tools/install/build_from_rom.py, shows its "@@progress" lines, writes everything
 * else to <install folder>/setup.log, then adds shortcuts and an "Apps & features" entry (per user, no admin).
 * Copied into the install folder as Uninstall.exe it removes the game again.
 *
 * The window: only the three text boxes and the buttons are Windows controls (the buttons and tick boxes are
 * owner-drawn); the labels, hints, progress bar and status line are painted, over a background made once
 * (make_background).
 *
 * Command line (for testing):  /rom=<zip> /bios=<zip> /dest=<folder> /silent /noshortcuts /noregister /shot=<bmp>
 * Uninstall:  /uninstall  (or just running the copy named Uninstall.exe), with /silent for no questions
 * (settings are kept) and /purge to delete the settings and saves as well.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "product.h"     /* P_PRODUCT, P_GAME, P_EXE, P_PUBLISHER, P_DISPLAY, P_VERSION: wide string literals */

#define APP_TITLE P_PRODUCT L" Setup"
#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" P_PRODUCT

/* the pause menu's colours */
#define C_BG      RGB(0x12, 0x13, 0x1A)
#define C_FIELD   RGB(0x1C, 0x1E, 0x29)
#define C_BORDER  RGB(0x36, 0x39, 0x49)
#define C_BUTTON  RGB(0x27, 0x2A, 0x38)
#define C_PRESSED RGB(0x1F, 0x21, 0x2D)
#define C_TEXT    RGB(0xEC, 0xED, 0xF1)
#define C_DIM     RGB(0x8E, 0x91, 0x9E)
#define C_OFF     RGB(0x5C, 0x5F, 0x6C)
#define C_GOLD    RGB(0xE8, 0xB6, 0x4C)
#define C_GOLD_LO RGB(0xC2, 0x96, 0x38)
#define C_ERROR   RGB(0xF0, 0x70, 0x68)
#define C_ON_GOLD RGB(0x15, 0x16, 0x1B)

enum { ID_ROM = 100, ID_ROM_BROWSE, ID_DEST, ID_DEST_BROWSE, ID_DESKTOP, ID_STARTMENU, ID_GO, ID_CANCEL, ID_BIOS,
       ID_BIOS_BROWSE };
enum { WM_APP_PROGRESS = WM_APP + 1, WM_APP_DONE };

/* layout, in 96-dpi pixels */
#define COL_X 28                /* the left column: labels, text boxes, buttons */
#define COL_W 540
#define EDIT_W 434
#define BROWSE_W 98
#define ROW1_Y 142
#define ROW2_Y 232
#define ROW3_Y 322
#define CHECK_Y 392
#define PROG_Y 436
#define STATUS_Y 454
#define BUTTON_Y 522
#define WIN_H 580
#define ART_OVERLAP 44          /* the picture starts this far inside the column's right margin, faded out */

static HINSTANCE g_inst;
static HWND g_wnd, g_rom, g_dest, g_desktop, g_startmenu, g_go, g_cancel, g_rom_browse, g_dest_browse, g_bios,
    g_bios_browse;
static HFONT g_font, g_font_bold, g_font_small, g_font_title, g_font_sub;
static HBRUSH g_br_bg, g_br_field;
static HBITMAP g_bg;            /* the whole client area's background */
static int g_dpi = 96, g_win_w = COL_X + COL_W + COL_X, g_art_w;   /* g_art_w: picture width, 96-dpi pixels */
static wchar_t g_setup_dir[MAX_PATH], g_files[MAX_PATH];
static HANDLE g_job, g_child;
static volatile LONG g_running;
static wchar_t g_error[1024], g_done_exe[MAX_PATH], g_log_path[MAX_PATH];
static wchar_t g_status[1200] = L"Setup makes the game from your ROMs on this PC. It takes a minute or two (longer on an "
                                L"older PC) and needs about 500 MB of free space.";
static int g_pct;
static int g_want_desktop = 1, g_want_startmenu = 1;
static int g_state;     /* 0 = ready, 1 = installing, 2 = finished, 3 = failed */
static int g_silent, g_no_shortcuts, g_no_register, g_exit_code;
static wchar_t g_shot[MAX_PATH];

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void path_join(wchar_t *out, const wchar_t *a, const wchar_t *b) {
    swprintf(out, MAX_PATH, L"%ls\\%ls", a, b);
}

static int file_exists(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ------------------------------------------------------------------ the picture (files/setup_art.png) */
static const GUID k_wic_clsid = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
static const GUID k_wic_iid = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
static const GUID k_wic_bgra = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0e}};

/* Open the picture. With pixels == NULL only its size is read; otherwise it is scaled to w x h BGRA pixels. */
static int load_art(UINT *iw, UINT *ih, uint32_t *pixels, int w, int h) {
    wchar_t art[MAX_PATH];
    path_join(art, g_files, L"setup_art.png");
    if (!file_exists(art)) return 0;
    IWICImagingFactory *f = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICBitmapScaler *sc = NULL;
    IWICFormatConverter *cv = NULL;
    int ok = 0;
    if (SUCCEEDED(CoCreateInstance(&k_wic_clsid, NULL, CLSCTX_INPROC_SERVER, &k_wic_iid, (void **)&f)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, art, NULL, GENERIC_READ,
                                                               WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) &&
        SUCCEEDED(IWICBitmapFrameDecode_GetSize(fr, iw, ih)) && *iw > 0 && *ih > 0) {
        if (!pixels) ok = 1;
        else if (SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(f, &sc)) &&
                 SUCCEEDED(IWICBitmapScaler_Initialize(sc, (IWICBitmapSource *)fr, (UINT)w, (UINT)h,
                                                       WICBitmapInterpolationModeFant)) &&
                 SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &cv)) &&
                 SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)sc, &k_wic_bgra,
                                                          WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)) &&
                 SUCCEEDED(IWICFormatConverter_CopyPixels(cv, NULL, (UINT)w * 4, (UINT)(w * h * 4), (BYTE *)pixels)))
            ok = 1;
    }
    if (cv) IWICFormatConverter_Release(cv);
    if (sc) IWICBitmapScaler_Release(sc);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (f) IWICImagingFactory_Release(f);
    return ok;
}

/* How wide the picture is shown: as tall as the window, in its own shape, within sensible limits. */
static void measure_art(void) {
    UINT iw, ih;
    if (!load_art(&iw, &ih, NULL, 0, 0)) return;
    g_art_w = (int)((double)WIN_H * iw / ih + 0.5);
    if (g_art_w < 200) g_art_w = 200;
    if (g_art_w > 520) g_art_w = 520;
    g_win_w = COL_X + COL_W + COL_X - ART_OVERLAP + g_art_w;
}

/* The background: the menu's dark panel; with a picture, it runs down the right-hand side under a dark overlay
 * that is solid at the picture's left edge (so it grows out of the panel) and lightest at the window's edge. */
static HBITMAP make_background(int w, int h) {
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
    void *bits;
    HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) return NULL;
    uint32_t *px = bits;
    const int br = GetRValue(C_BG), bg = GetGValue(C_BG), bb = GetBValue(C_BG);
    for (int i = 0; i < w * h; i++) px[i] = (uint32_t)(br << 16 | bg << 8 | bb);

    int aw = S(g_art_w), ok = 0;
    uint32_t *art = NULL;
    if (aw > 0 && aw <= w) {
        UINT iw, ih;
        if (load_art(&iw, &ih, NULL, 0, 0)) {
            /* cover aw x h: scale to the height, or to the width if that leaves a gap, and centre the crop */
            int sw = (int)((double)h * iw / ih + 0.5), sh = h;
            if (sw < aw) { sw = aw; sh = (int)((double)aw * ih / iw + 0.5); }
            art = malloc((size_t)sw * sh * 4);
            if (art && load_art(&iw, &ih, art, sw, sh)) {
                int ox = (sw - aw) / 2, oy = (sh - h) / 4, x0 = w - aw;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < aw; x++) {
                        /* how much of the picture shows: 0 at its left edge, rising smoothly, a little less
                         * towards the bottom where the buttons' row ends */
                        float t = (float)x / (float)aw, k = t < 0.5f ? t / 0.5f : 1.0f;
                        k = k * k * (3 - 2 * k) * 0.80f * (1.0f - 0.30f * (float)y / (float)h);
                        uint32_t c = art[(y + oy) * sw + x + ox];
                        int r = (int)(((c >> 16) & 255) * k + br * (1 - k)), g = (int)(((c >> 8) & 255) * k + bg * (1 - k)),
                            b = (int)((c & 255) * k + bb * (1 - k));
                        px[y * w + x0 + x] = (uint32_t)(r << 16 | g << 8 | b);
                    }
                ok = 1;
            }
        }
    }
    free(art);
    if (!ok) {
        /* no picture: a faint gold light from the right, as on the menu's plain panel */
        for (int y = 0; y < h; y++)
            for (int x = w / 2; x < w; x++) {
                float t = (float)(x - w / 2) / (float)(w / 2), k = t * t * 0.10f * (1.0f - 0.6f * (float)y / (float)h);
                int r = (int)(0xE8 * k + br * (1 - k)), g = (int)(0xB6 * k + bg * (1 - k)), b = (int)(0x4C * k + bb * (1 - k));
                px[y * w + x] = (uint32_t)(r << 16 | g << 8 | b);
            }
    }
    return bmp;
}

/* ------------------------------------------------------------------ logging + the build process */
static void log_line(const char *s) {
    HANDLE h = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n;
    WriteFile(h, s, (DWORD)strlen(s), &n, NULL);
    WriteFile(h, "\r\n", 2, &n, NULL);
    CloseHandle(h);
}

static wchar_t *utf8_to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    else w[0] = 0;
    return w;
}

static void handle_line(char *line) {
    log_line(line);
    if (!strncmp(line, "@@progress ", 11)) {
        char *end;
        long pct = strtol(line + 11, &end, 10);
        while (*end == ' ') end++;
        PostMessageW(g_wnd, WM_APP_PROGRESS, (WPARAM)pct, (LPARAM)utf8_to_wide(end));
    } else if (!strncmp(line, "@@error ", 8)) {
        wchar_t *w = utf8_to_wide(line + 8);
        wcsncpy(g_error, w, 1023);
        free(w);
    } else if (!strncmp(line, "@@done ", 7)) {
        wchar_t *w = utf8_to_wide(line + 7);
        wcsncpy(g_done_exe, w, MAX_PATH - 1);
        free(w);
    }
}

static DWORD WINAPI reader(LPVOID pipe) {
    char buf[4096], line[8192];
    size_t len = 0;
    DWORD n;
    while (ReadFile(pipe, buf, sizeof buf, &n, NULL) && n) {
        for (DWORD i = 0; i < n; i++) {
            if (buf[i] == '\n' || len == sizeof line - 1) {
                while (len && line[len - 1] == '\r') len--;
                line[len] = 0;
                handle_line(line);
                len = 0;
            } else line[len++] = buf[i];
        }
    }
    if (len) { line[len] = 0; handle_line(line); }
    CloseHandle(pipe);
    WaitForSingleObject(g_child, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(g_child, &code);
    PostMessageW(g_wnd, WM_APP_DONE, code, 0);
    return 0;
}

/* Append  name "value"  to a command line, quoted the way CommandLineToArgvW and the C runtime read it:
 * backslashes just before a quote (or before the closing quote) are doubled, quotes are escaped. */
static void add_arg(wchar_t *cmd, size_t cap, const wchar_t *name, const wchar_t *value) {
    size_t n = wcslen(cmd);
    if (name) { swprintf(cmd + n, cap - n, L" %ls", name); n = wcslen(cmd); }
    if (n + 2 >= cap) return;
    cmd[n++] = L' ';
    cmd[n++] = L'"';
    for (const wchar_t *p = value; *p && n + 4 < cap; p++) {
        size_t slashes = 0;
        while (*p == L'\\') { slashes++; p++; }
        if (*p == 0 || *p == L'"') slashes *= 2;            /* they would otherwise escape the quote */
        while (slashes-- && n + 4 < cap) cmd[n++] = L'\\';
        if (*p == 0) break;
        if (*p == L'"') cmd[n++] = L'\\';
        cmd[n++] = *p;
    }
    cmd[n++] = L'"';
    cmd[n] = 0;
}

static int start_build(const wchar_t *rom, const wchar_t *bios, const wchar_t *dest) {
    wchar_t py[MAX_PATH], script[MAX_PATH], clang[MAX_PATH], recomp[MAX_PATH], cmd[8 * MAX_PATH];
    path_join(py, g_files, L"python\\python.exe");
    path_join(script, g_files, L"source\\tools\\install\\build_from_rom.py");
    path_join(clang, g_files, L"clang\\bin");
    path_join(recomp, g_files, L"n64recomp");
    if (!file_exists(py) || !file_exists(script)) {
        wcscpy(g_error, L"Setup's files folder is missing or incomplete. Unzip the whole Setup download and run Setup.exe from there.");
        return 0;
    }
    /* the folder must exist (or be creatable) and be writable: the log goes there first */
    int made = SHCreateDirectoryExW(NULL, dest, NULL);
    path_join(g_log_path, dest, L"setup.log");
    DeleteFileW(g_log_path);
    HANDLE probe = CreateFileW(g_log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (probe == INVALID_HANDLE_VALUE) {
        (void)made;
        swprintf(g_error, 1024, L"Setup can't write to %ls. Choose a folder of your own, for example one in Documents or "
                                L"on another drive (folders like Program Files need administrator rights).", dest);
        return 0;
    }
    CloseHandle(probe);
    const size_t cap = sizeof cmd / sizeof cmd[0];
    swprintf(cmd, cap, L"\"%ls\" -u -X utf8", py);
    add_arg(cmd, cap, NULL, script);
    add_arg(cmd, cap, L"--rom", rom);
    add_arg(cmd, cap, L"--dest", dest);
    add_arg(cmd, cap, L"--clang", clang);
    add_arg(cmd, cap, L"--n64recomp", recomp);
    if (bios[0]) add_arg(cmd, cap, L"--bios", bios);

    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    /* a job object so Cancel (or closing Setup) stops the whole build, compilers included */
    g_job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, dest, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        swprintf(g_error, 1024, L"Could not start the build (error %lu).", GetLastError());
        return 0;
    }
    AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(wr);
    g_child = pi.hProcess;
    InterlockedExchange(&g_running, 1);
    CreateThread(NULL, 0, reader, rd, 0, NULL);
    return 1;
}

/* ------------------------------------------------------------------ shortcuts, Apps & features, uninstall */
static void make_shortcut(int csidl, const wchar_t *exe, const wchar_t *dir) {
    wchar_t folder[MAX_PATH], lnk[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, csidl, NULL, 0, folder))) return;
    path_join(lnk, folder, P_PRODUCT L".lnk");
    IShellLinkW *sl;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return;
    IShellLinkW_SetPath(sl, exe);
    IShellLinkW_SetWorkingDirectory(sl, dir);
    IShellLinkW_SetDescription(sl, P_PRODUCT L": " P_GAME L" on PC");
    IPersistFile *pf;
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        IPersistFile_Save(pf, lnk, TRUE);
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
}

/* Removes the shortcut only if it starts the game in `dir` (another install's shortcut is left alone). */
static void remove_shortcut(int csidl, const wchar_t *dir) {
    wchar_t folder[MAX_PATH], lnk[MAX_PATH], target[MAX_PATH] = L"", mine[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, csidl, NULL, 0, folder))) return;
    path_join(lnk, folder, P_PRODUCT L".lnk");
    if (!file_exists(lnk)) return;
    IShellLinkW *sl;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return;
    IPersistFile *pf;
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        if (SUCCEEDED(IPersistFile_Load(pf, lnk, STGM_READ))) IShellLinkW_GetPath(sl, target, MAX_PATH, NULL, SLGP_RAWPATH);
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
    path_join(mine, dir, P_EXE);
    if (!_wcsicmp(target, mine)) DeleteFileW(lnk);
}

static void reg_str(HKEY k, const wchar_t *name, const wchar_t *v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
}

static void register_uninstall(const wchar_t *dest, const wchar_t *exe) {
    wchar_t self[MAX_PATH], un[MAX_PATH], cmd[MAX_PATH + 32];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    path_join(un, dest, L"Uninstall.exe");
    CopyFileW(self, un, FALSE);
    swprintf(cmd, MAX_PATH + 32, L"\"%ls\" /uninstall", un);
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)) return;
    reg_str(k, L"DisplayName", P_DISPLAY);
    reg_str(k, L"DisplayIcon", exe);
    reg_str(k, L"DisplayVersion", P_VERSION);
    reg_str(k, L"InstallLocation", dest);
    reg_str(k, L"UninstallString", cmd);
    reg_str(k, L"Publisher", P_PUBLISHER);
    DWORD one = 1, kb = 180 * 1024;                 /* the game and its unpacked ROM data */
    RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE *)&one, sizeof one);
    RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE *)&one, sizeof one);
    RegSetValueExW(k, L"EstimatedSize", 0, REG_DWORD, (const BYTE *)&kb, sizeof kb);
    RegCloseKey(k);
}

static void delete_in(const wchar_t *dir, const wchar_t *name) {
    wchar_t p[MAX_PATH];
    path_join(p, dir, name);
    DeleteFileW(p);
}

static void delete_tree(const wchar_t *dir, const wchar_t *name) {
    wchar_t d[MAX_PATH + 2] = {0};
    path_join(d, dir, name);
    d[wcslen(d) + 1] = 0;                           /* SHFileOperation wants a double-NUL-terminated list */
    if (GetFileAttributesW(d) == INVALID_FILE_ATTRIBUTES) return;
    SHFILEOPSTRUCTW op = {0};
    op.wFunc = FO_DELETE;
    op.pFrom = d;
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationW(&op);
}

/* The scratch folder of a build: deleted only if it carries Setup's own marker file. */
static void delete_scratch(const wchar_t *dir) {
    wchar_t mark[MAX_PATH];
    swprintf(mark, MAX_PATH, L"%ls\\_build\\.setup-scratch", dir);
    if (file_exists(mark)) delete_tree(dir, L"_build");
}

/* quiet: no questions and no message (settings are kept unless purge). */
static int uninstall(const wchar_t *dir, int quiet, int purge) {
    wchar_t game[MAX_PATH];
    path_join(game, dir, P_EXE);
    if (!file_exists(game)) {          /* not an install folder: never delete things from anywhere else */
        if (!quiet) MessageBoxW(NULL, P_PRODUCT L" isn't installed in this folder.", P_PRODUCT, MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    if (!quiet && MessageBoxW(NULL, L"Remove " P_PRODUCT L" from this PC?", P_PRODUCT, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 1;
    int saves = quiet ? purge :
                MessageBoxW(NULL, L"Also delete your settings, controls and the game's saved settings and scores?\n\n"
                                  L"Choose No to keep them for next time.",
                            P_PRODUCT, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    static const wchar_t *files[] = {P_EXE, L"setup.log", L"buriki.log", L"buriki.old.log", L"nvram.bin.tmp",
                                     L"menu_art.png", L"menu_art.jpg", L"menu_art.jpeg"};
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) delete_in(dir, files[i]);
    /* folders: the ROM data unpacked from the player's sets (just the two folders Setup made; "roms" itself only
     * if that leaves it empty), a build that was interrupted, and the log folder the NVIDIA driver leaves in the
     * folder a game runs from */
    wchar_t roms[MAX_PATH];
    path_join(roms, dir, L"roms");
    delete_tree(roms, L"buriki");
    delete_tree(roms, L"hng64");
    RemoveDirectoryW(roms);
    delete_scratch(dir);
    delete_tree(dir, L"NVIDIA Corporation");
    if (saves) {
        delete_in(dir, L"settings.ini");
        delete_in(dir, L"controls.ini");
        delete_in(dir, L"nvram.bin");
    }
    remove_shortcut(CSIDL_DESKTOPDIRECTORY, dir);
    remove_shortcut(CSIDL_PROGRAMS, dir);
    /* the Apps entry: only if it is this folder's (an older install elsewhere mustn't remove a newer one's) */
    HKEY k;
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, KEY_READ, &k)) {
        wchar_t loc[MAX_PATH] = L"";
        DWORD size = sizeof loc;
        RegQueryValueExW(k, L"InstallLocation", NULL, NULL, (BYTE *)loc, &size);
        RegCloseKey(k);
        if (!_wcsicmp(loc, dir)) RegDeleteKeyW(HKEY_CURRENT_USER, UNINSTALL_KEY);
    }
    if (!quiet)
        MessageBoxW(NULL, saves ? P_PRODUCT L" has been removed." :
                                  P_PRODUCT L" has been removed. Your settings were kept in its folder.",
                    P_PRODUCT, MB_OK | MB_ICONINFORMATION);
    /* this program is <dir>\Uninstall.exe and can't delete itself while running: a hidden cmd does it after */
    wchar_t cmd[3 * MAX_PATH], sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    swprintf(cmd, 3 * MAX_PATH, L"\"%ls\\cmd.exe\" /c ping 127.0.0.1 -n 3 > nul & del \"%ls\\Uninstall.exe\" & rmdir \"%ls\"",
             sysdir, dir, dir);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, sysdir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return 0;
}

/* ------------------------------------------------------------------ the window */
static void set_status(const wchar_t *text) {
    wcsncpy(g_status, text, sizeof g_status / sizeof g_status[0] - 1);
    RECT r = {S(COL_X), S(PROG_Y - 4), S(COL_X + COL_W), S(BUTTON_Y - 4)};
    InvalidateRect(g_wnd, &r, FALSE);
}

static void set_state(int st) {
    g_state = st;
    int idle = st == 0 || st == 3;
    EnableWindow(g_rom, idle);
    EnableWindow(g_dest, idle);
    EnableWindow(g_rom_browse, idle);
    EnableWindow(g_bios, idle);
    EnableWindow(g_bios_browse, idle);
    EnableWindow(g_dest_browse, idle);
    EnableWindow(g_desktop, idle);
    EnableWindow(g_startmenu, idle);
    SetWindowTextW(g_go, st == 2 ? L"Play" : st == 3 ? L"Try again" : L"Install");
    EnableWindow(g_go, st != 1);
    SetWindowTextW(g_cancel, st == 2 ? L"Close" : L"Cancel");
    InvalidateRect(g_wnd, NULL, FALSE);
}

static void browse_zip(HWND edit, const wchar_t *title) {
    wchar_t file[MAX_PATH] = L"";
    GetWindowTextW(edit, file, MAX_PATH);
    OPENFILENAMEW of = {sizeof of};
    of.hwndOwner = g_wnd;
    of.lpstrFilter = L"MAME ROM set (*.zip)\0*.zip\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = title;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&of)) SetWindowTextW(edit, file);
}

static void browse_dest(void) {
    IFileOpenDialog *d;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return;
    DWORD opt;
    IFileOpenDialog_GetOptions(d, &opt);
    IFileOpenDialog_SetOptions(d, opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IFileOpenDialog_SetTitle(d, L"Install " P_PRODUCT L" to");
    if (SUCCEEDED(IFileOpenDialog_Show(d, g_wnd))) {
        IShellItem *it;
        if (SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
            wchar_t *p;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
                /* picking "D:\Games" installs into "D:\Games\<product>" */
                wchar_t full[MAX_PATH];
                const wchar_t *base = wcsrchr(p, L'\\');
                if (base && !_wcsicmp(base + 1, P_PRODUCT)) { wcsncpy(full, p, MAX_PATH - 1); full[MAX_PATH - 1] = 0; }
                else swprintf(full, MAX_PATH, L"%ls%ls%ls", p, p[wcslen(p) - 1] == L'\\' ? L"" : L"\\", P_PRODUCT);
                SetWindowTextW(g_dest, full);
                CoTaskMemFree(p);
            }
            IShellItem_Release(it);
        }
    }
    IFileOpenDialog_Release(d);
}

/* Does the folder hold anything that isn't Setup's? (No game exe, and more than a stopped install's leftovers.)
 * Setup replaces roms\buriki, roms\hng64 and _build inside its install folder, so it only installs into a folder
 * that is new, empty, or already this game's. build_from_rom.py checks the same. */
static int has_other_files(const wchar_t *dir) {
    wchar_t p[MAX_PATH];
    path_join(p, dir, P_EXE);
    if (file_exists(p)) return 0;
    swprintf(p, MAX_PATH, L"%ls\\_build\\.setup-scratch", dir);
    int scratch_ok = file_exists(p);
    path_join(p, dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int other = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || !_wcsicmp(fd.cFileName, L"setup.log")) continue;
        if (scratch_ok && !_wcsicmp(fd.cFileName, L"_build")) continue;
        other = 1;
    } while (!other && FindNextFileW(h, &fd));
    FindClose(h);
    return other;
}

static void go(void) {
    if (g_state == 1) return;
    if (g_state == 2) {
        wchar_t dir[MAX_PATH];
        GetWindowTextW(g_dest, dir, MAX_PATH);
        ShellExecuteW(NULL, L"open", g_done_exe, NULL, dir, SW_SHOWNORMAL);
        DestroyWindow(g_wnd);
        return;
    }
    wchar_t rom[MAX_PATH], bios[MAX_PATH], dest[MAX_PATH];
    GetWindowTextW(g_rom, rom, MAX_PATH);
    GetWindowTextW(g_bios, bios, MAX_PATH);
    GetWindowTextW(g_dest, dest, MAX_PATH);
    if (!file_exists(rom)) {
        if (!g_silent) MessageBoxW(g_wnd, L"Choose your " P_GAME L" ROM first: the MAME set buriki.zip.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
        else g_exit_code = 2, DestroyWindow(g_wnd);
        return;
    }
    if (bios[0] && !file_exists(bios)) {
        if (!g_silent) MessageBoxW(g_wnd, L"The system ROM file was not found. Choose the MAME set hng64.zip again.",
                                   APP_TITLE, MB_OK | MB_ICONINFORMATION);
        else g_exit_code = 2, DestroyWindow(g_wnd);
        return;
    }
    /* the install folder: trimmed, made absolute, without a trailing backslash (except a drive's root) */
    {
        wchar_t *b = dest;
        while (*b == L' ' || *b == L'"') b++;
        size_t n = wcslen(b);
        while (n && (b[n - 1] == L' ' || b[n - 1] == L'"')) b[--n] = 0;
        if (!n) return;
        wchar_t full[MAX_PATH];
        DWORD got = GetFullPathNameW(b, MAX_PATH, full, NULL);
        if (!got || got >= MAX_PATH - 40) {     /* room for the game's own files below it */
            if (!g_silent) MessageBoxW(g_wnd, L"That folder's path is too long. Choose a shorter one.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
            else g_exit_code = 2, DestroyWindow(g_wnd);
            return;
        }
        n = wcslen(full);
        while (n > 3 && full[n - 1] == L'\\') full[--n] = 0;
        wcscpy(dest, full);
        SetWindowTextW(g_dest, dest);
        if (has_other_files(dest)) {
            /* offer a folder of its own inside the one chosen, and let the player look before going on */
            wchar_t sub[MAX_PATH], msg[2 * MAX_PATH + 200];
            path_join(sub, dest, P_PRODUCT);
            swprintf(msg, sizeof msg / sizeof msg[0], L"%ls already has other files in it.\n\nSetup will install into its own "
                     L"folder there instead:\n%ls\n\nPress Install again to go ahead.", dest, sub);
            if (g_silent) { g_exit_code = 2; DestroyWindow(g_wnd); return; }
            SetWindowTextW(g_dest, sub);
            MessageBoxW(g_wnd, msg, APP_TITLE, MB_OK | MB_ICONINFORMATION);
            return;
        }
    }
    g_error[0] = 0;
    g_done_exe[0] = 0;
    g_pct = 0;
    set_status(L"Starting");
    set_state(1);
    if (!start_build(rom, bios, dest)) {
        set_status(g_error);
        set_state(3);
        if (g_silent) g_exit_code = 1, DestroyWindow(g_wnd);
    }
}

static void finished(DWORD code) {
    InterlockedExchange(&g_running, 0);
    if (g_job) { CloseHandle(g_job); g_job = NULL; }
    if (g_child) { CloseHandle(g_child); g_child = NULL; }
    if (code == 0 && g_done_exe[0]) {
        wchar_t dest[MAX_PATH];
        GetWindowTextW(g_dest, dest, MAX_PATH);
        if (!g_no_shortcuts) {
            if (g_want_desktop) make_shortcut(CSIDL_DESKTOPDIRECTORY, g_done_exe, dest);
            if (g_want_startmenu) make_shortcut(CSIDL_PROGRAMS, g_done_exe, dest);
        }
        if (!g_no_register) register_uninstall(dest, g_done_exe);
        g_pct = 100;
        set_status(L"Installed. Press Play to start (Back + Start or Tab opens the menu in game).");
        set_state(2);
        g_exit_code = 0;
    } else {
        set_status(g_error[0] ? g_error : L"Setup stopped (see setup.log in the install folder).");
        set_state(3);
        g_exit_code = 1;
    }
    if (g_silent) DestroyWindow(g_wnd);
}

static HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, HFONT font) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

/* A text box: the borderless edit control sits inside a rounded field that paint_dc draws. */
#define FIELD_H 32
static HWND add_field(int y, int id) {
    return add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, COL_X + 10, y + 26 + 6, EDIT_W - 20, FIELD_H - 12, id, g_font);
}

static void create_controls(void) {
    g_font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_bold = CreateFontW(-S(15), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_small = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_title = CreateFontW(-S(38), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_sub = CreateFontW(-S(15), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_br_bg = CreateSolidBrush(C_BG);
    g_br_field = CreateSolidBrush(C_FIELD);
    RECT cr;
    GetClientRect(g_wnd, &cr);
    g_bg = make_background(cr.right, cr.bottom);

    const int bx = COL_X + EDIT_W + 8, bo = 26;     /* Browse buttons; the fields sit 26 below their labels */
    g_rom = add_field(ROW1_Y, ID_ROM);
    g_rom_browse = add(L"BUTTON", L"Browse...", BS_OWNERDRAW | WS_TABSTOP, bx, ROW1_Y + bo, BROWSE_W, FIELD_H, ID_ROM_BROWSE, g_font);
    g_bios = add_field(ROW2_Y, ID_BIOS);
    g_bios_browse = add(L"BUTTON", L"Browse...", BS_OWNERDRAW | WS_TABSTOP, bx, ROW2_Y + bo, BROWSE_W, FIELD_H, ID_BIOS_BROWSE, g_font);
    g_dest = add_field(ROW3_Y, ID_DEST);
    g_dest_browse = add(L"BUTTON", L"Browse...", BS_OWNERDRAW | WS_TABSTOP, bx, ROW3_Y + bo, BROWSE_W, FIELD_H, ID_DEST_BROWSE, g_font);
    g_desktop = add(L"BUTTON", L"Desktop shortcut", BS_OWNERDRAW | WS_TABSTOP, COL_X, CHECK_Y, 180, 24, ID_DESKTOP, g_font);
    g_startmenu = add(L"BUTTON", L"Start menu shortcut", BS_OWNERDRAW | WS_TABSTOP, COL_X + 190, CHECK_Y, 200, 24, ID_STARTMENU, g_font);
    g_go = add(L"BUTTON", L"Install", BS_OWNERDRAW | WS_TABSTOP, COL_X + COL_W - 104 - 8 - 104, BUTTON_Y, 104, 34, ID_GO, g_font_bold);
    g_cancel = add(L"BUTTON", L"Cancel", BS_OWNERDRAW | WS_TABSTOP, COL_X + COL_W - 104, BUTTON_Y, 104, 34, ID_CANCEL, g_font);
}

static void fill_round(HDC dc, int x0, int y0, int x1, int y1, int radius, COLORREF fill, COLORREF edge) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, x0, y0, x1, y1, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void text_at(HDC dc, HFONT font, COLORREF colour, int x, int y, const wchar_t *s) {
    SelectObject(dc, font);
    SetTextColor(dc, colour);
    TextOutW(dc, S(x), S(y), s, (int)wcslen(s));
}

/* Buttons and tick boxes (all owner-drawn). */
static void draw_item(const DRAWITEMSTRUCT *d) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    int id = (int)d->CtlID, off = (d->itemState & ODS_DISABLED) != 0, down = (d->itemState & ODS_SELECTED) != 0,
        focus = (d->itemState & ODS_FOCUS) != 0 && !(d->itemState & ODS_NOFOCUSRECT);
    wchar_t text[64];
    GetWindowTextW(d->hwndItem, text, 64);
    FillRect(dc, &r, g_br_bg);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, (HFONT)SendMessageW(d->hwndItem, WM_GETFONT, 0, 0));
    if (id == ID_DESKTOP || id == ID_STARTMENU) {
        int on = id == ID_DESKTOP ? g_want_desktop : g_want_startmenu;
        int b = S(18), top = (r.bottom - b) / 2;
        fill_round(dc, 0, top, b, top + b, S(5), on ? (off ? C_GOLD_LO : C_GOLD) : C_FIELD, on ? C_GOLD_LO : (focus ? C_GOLD : C_BORDER));
        if (on) {                                   /* the tick */
            HPEN p = CreatePen(PS_SOLID, S(2), C_ON_GOLD);
            HGDIOBJ op = SelectObject(dc, p);
            MoveToEx(dc, S(4), top + S(9), NULL);
            LineTo(dc, S(8), top + S(13));
            LineTo(dc, S(14), top + S(5));
            SelectObject(dc, op);
            DeleteObject(p);
        }
        RECT tr = {b + S(9), 0, r.right, r.bottom};
        SetTextColor(dc, off ? C_OFF : C_TEXT);
        DrawTextW(dc, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        if (focus) {
            RECT fr = {b + S(6), r.bottom - S(3), b + S(9) + S(40), r.bottom - S(2)};
            HBRUSH g = CreateSolidBrush(C_GOLD);
            FillRect(dc, &fr, g);
            DeleteObject(g);
        }
        return;
    }
    int primary = id == ID_GO;
    COLORREF fill = primary ? (off ? C_BUTTON : down ? C_GOLD_LO : C_GOLD) : (down ? C_PRESSED : C_BUTTON);
    COLORREF edge = primary && !off ? C_GOLD_LO : focus ? C_GOLD : C_BORDER;
    fill_round(dc, 0, 0, r.right, r.bottom, S(8), fill, edge);
    SetTextColor(dc, off ? C_OFF : primary ? C_ON_GOLD : C_TEXT);
    DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    if (primary && focus && !off) {                 /* a second, inner ring for keyboard focus */
        HPEN p = CreatePen(PS_SOLID, 1, C_ON_GOLD);
        HGDIOBJ op = SelectObject(dc, p), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, S(3), S(3), r.right - S(3), r.bottom - S(3), S(6), S(6));
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(p);
    }
}

static void row(HDC dc, int y, const wchar_t *label, const wchar_t *hint, HWND edit) {
    text_at(dc, g_font_bold, C_TEXT, COL_X, y, label);
    fill_round(dc, S(COL_X), S(y + 26), S(COL_X + EDIT_W), S(y + 26 + FIELD_H), S(8), C_FIELD,
               GetFocus() == edit ? C_GOLD : C_BORDER);
    if (hint) text_at(dc, g_font_small, C_DIM, COL_X, y + 26 + FIELD_H + 6, hint);
}

static void paint_dc(HWND h, HDC out) {
    RECT cr;
    GetClientRect(h, &cr);
    /* draw into a buffer, then show it in one go (no flicker) */
    HDC dc = CreateCompatibleDC(out);
    HBITMAP buf = CreateCompatibleBitmap(out, cr.right, cr.bottom);
    HGDIOBJ old_buf = SelectObject(dc, buf);
    if (g_bg) {
        HDC m = CreateCompatibleDC(out);
        HGDIOBJ old = SelectObject(m, g_bg);
        BitBlt(dc, 0, 0, cr.right, cr.bottom, m, 0, 0, SRCCOPY);
        SelectObject(m, old);
        DeleteDC(m);
    } else FillRect(dc, &cr, g_br_bg);
    SetBkMode(dc, TRANSPARENT);

    /* title */
    text_at(dc, g_font_title, C_TEXT, COL_X - 2, 24, P_PRODUCT);
    HBRUSH gold = CreateSolidBrush(C_GOLD);
    RECT bar = {S(COL_X), S(78), S(COL_X + 64), S(78 + 4)};
    FillRect(dc, &bar, gold);
    text_at(dc, g_font_sub, C_DIM, COL_X, 94, P_GAME L" on PC: setup");

    row(dc, ROW1_Y, L"Your " P_GAME L" ROM",
        L"The MAME set buriki.zip (" P_GAME L", rev. B). It is checked before anything is installed.", g_rom);
    row(dc, ROW2_Y, L"Your Hyper Neo Geo 64 system ROM",
        L"The MAME set hng64.zip: the arcade board's own startup and control programs.", g_bios);
    row(dc, ROW3_Y, L"Install to", NULL, g_dest);

    /* progress bar */
    int x0 = S(COL_X), x1 = S(COL_X + COL_W), y0 = S(PROG_Y), y1 = S(PROG_Y + 8);
    fill_round(dc, x0, y0, x1, y1, S(8), C_FIELD, C_FIELD);
    if (g_pct > 0) {
        int xe = x0 + (x1 - x0) * (g_pct > 100 ? 100 : g_pct) / 100;
        if (xe < x0 + S(8)) xe = x0 + S(8);
        fill_round(dc, x0, y0, xe, y1, S(8), C_GOLD, C_GOLD);
    }
    /* status (wraps over up to three lines) */
    RECT sr = {S(COL_X), S(STATUS_Y), S(COL_X + COL_W), S(BUTTON_Y - 6)};
    SelectObject(dc, g_font_small);
    SetTextColor(dc, g_state == 3 ? C_ERROR : g_state == 2 ? C_TEXT : C_DIM);
    DrawTextW(dc, g_status, -1, &sr, DT_WORDBREAK | DT_NOPREFIX);
    /* footer, left of the buttons */
    text_at(dc, g_font_small, C_OFF, COL_X, BUTTON_Y + 9, L"No game data comes with Setup.");
    DeleteObject(gold);

    BitBlt(out, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_buf);
    DeleteObject(buf);
    DeleteDC(dc);
}

static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    paint_dc(h, dc);
    EndPaint(h, &ps);
}

static void save_shot(void) {
    RECT r;
    GetWindowRect(g_wnd, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC sdc = GetDC(NULL), m = CreateCompatibleDC(sdc);
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, h, 1, 24, BI_RGB}};
    void *bits;
    HBITMAP b = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(m, b);
    SendMessageW(g_wnd, WM_PRINT, (WPARAM)m, PRF_NONCLIENT | PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    int stride = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh = {0x4D42, (DWORD)(sizeof fh + sizeof bi.bmiHeader + stride * h), 0, 0, sizeof fh + sizeof bi.bmiHeader};
    FILE *f = _wfopen(g_shot, L"wb");
    if (f) {
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
        fwrite(bits, (size_t)stride * h, 1, f);
        fclose(f);
    }
    DeleteObject(b);
    DeleteDC(m);
    ReleaseDC(NULL, sdc);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: paint(h); return 0;
    case WM_PRINTCLIENT: paint_dc(h, (HDC)wp); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {       /* (a disabled text box asks with WM_CTLCOLORSTATIC) */
        HDC dc = (HDC)wp;
        SetBkColor(dc, C_FIELD);
        SetTextColor(dc, C_TEXT);
        return (LRESULT)g_br_field;
    }
    case WM_DRAWITEM: draw_item((const DRAWITEMSTRUCT *)lp); return TRUE;
    case DM_GETDEFID: return MAKELONG(ID_GO, DC_HASDEFID);      /* Enter presses Install / Play */
    case WM_COMMAND:
        if ((HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) && lp) {   /* the field's gold outline */
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        switch (LOWORD(wp)) {
        case ID_ROM_BROWSE: browse_zip(g_rom, L"Choose your " P_GAME L" ROM (buriki.zip)"); break;
        case ID_BIOS_BROWSE: browse_zip(g_bios, L"Choose your Hyper Neo Geo 64 system ROM (hng64.zip)"); break;
        case ID_DEST_BROWSE: browse_dest(); break;
        case ID_DESKTOP: g_want_desktop = !g_want_desktop; InvalidateRect(g_desktop, NULL, FALSE); break;
        case ID_STARTMENU: g_want_startmenu = !g_want_startmenu; InvalidateRect(g_startmenu, NULL, FALSE); break;
        case ID_GO:
        case IDOK: go(); break;
        case ID_CANCEL:
        case IDCANCEL:
            if (g_state == 1 && MessageBoxW(h, L"Stop installing?", APP_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) break;
            DestroyWindow(h);
            break;
        }
        return 0;
    case WM_APP_PROGRESS:
        g_pct = (int)wp;
        set_status((wchar_t *)lp);
        free((void *)lp);
        return 0;
    case WM_APP_DONE: finished((DWORD)wp); return 0;
    case WM_CLOSE:
        if (g_state == 1 && MessageBoxW(h, L"Stop installing?", APP_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (g_job) {                        /* a build is still running: stop it and remove what it was making */
            HANDLE child = g_child;
            CloseHandle(g_job);             /* kills Python and the compilers */
            g_job = NULL;
            if (child) WaitForSingleObject(child, 5000);
            wchar_t dest[MAX_PATH];
            GetWindowTextW(g_dest, dest, MAX_PATH);
            Sleep(200);                     /* the compilers' files are released a moment after they stop */
            delete_scratch(dest);
            wchar_t game[MAX_PATH];
            path_join(game, dest, P_EXE);
            if (!file_exists(game)) {       /* nothing was installed there before: leave no empty folder behind */
                delete_in(dest, L"setup.log");
                RemoveDirectoryW(dest);
            }
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* A dark title bar to match (Windows 10 2004 and later; older versions keep the normal one). */
static void dark_title_bar(HWND h) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT(WINAPI * SetAttr)(HWND, DWORD, LPCVOID, DWORD);
    SetAttr set = (SetAttr)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
    BOOL on = TRUE;
    if (set && FAILED(set(h, 20, &on, sizeof on))) set(h, 19, &on, sizeof on);   /* DWMWA_USE_IMMERSIVE_DARK_MODE */
}

/* a ROM set, if it's lying next to Setup, one folder up, or in Downloads */
static void guess_rom(wchar_t *out, const wchar_t *name) {
    wchar_t p[MAX_PATH], dl[MAX_PATH];
    path_join(p, g_setup_dir, name);
    if (file_exists(p)) { wcscpy(out, p); return; }
    swprintf(p, MAX_PATH, L"%ls\\..\\%ls", g_setup_dir, name);
    if (file_exists(p)) { GetFullPathNameW(p, MAX_PATH, out, NULL); return; }
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, dl))) {
        swprintf(p, MAX_PATH, L"%ls\\Downloads\\%ls", dl, name);
        if (file_exists(p)) { wcscpy(out, p); return; }
    }
    out[0] = 0;
}

static const wchar_t *arg_value(const wchar_t *arg, const wchar_t *name) {
    size_t n = wcslen(name);
    return !_wcsnicmp(arg, name, n) ? arg + n : NULL;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show) {
    (void)prev; (void)cmdline;
    g_inst = inst;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GetModuleFileNameW(NULL, g_setup_dir, MAX_PATH);
    *wcsrchr(g_setup_dir, L'\\') = 0;
    path_join(g_files, g_setup_dir, L"files");

    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t rom[MAX_PATH] = L"", bios[MAX_PATH] = L"", dest[MAX_PATH] = L"";
    int bios_given = 0, want_uninstall = 0, purge = 0;
    {   /* the copy Setup leaves in the install folder is named Uninstall.exe: run on its own, it uninstalls */
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(NULL, self, MAX_PATH);
        const wchar_t *base = wcsrchr(self, L'\\');
        if (base && !_wcsicmp(base + 1, L"Uninstall.exe")) want_uninstall = 1;
    }
    for (int i = 1; i < argc; i++) {
        const wchar_t *v;
        if (!_wcsicmp(argv[i], L"/uninstall")) want_uninstall = 1;
        else if (!_wcsicmp(argv[i], L"/purge")) purge = 1;
        else if ((v = arg_value(argv[i], L"/rom="))) wcsncpy(rom, v, MAX_PATH - 1);
        else if ((v = arg_value(argv[i], L"/bios="))) wcsncpy(bios, v, MAX_PATH - 1), bios_given = 1;
        else if ((v = arg_value(argv[i], L"/dest="))) wcsncpy(dest, v, MAX_PATH - 1);
        else if ((v = arg_value(argv[i], L"/shot="))) wcsncpy(g_shot, v, MAX_PATH - 1);
        else if (!_wcsicmp(argv[i], L"/silent")) g_silent = 1;
        else if (!_wcsicmp(argv[i], L"/noshortcuts")) g_no_shortcuts = 1;
        else if (!_wcsicmp(argv[i], L"/noregister")) g_no_register = 1;
    }
    if (want_uninstall) return uninstall(g_setup_dir, g_silent, purge);
    if (!rom[0]) guess_rom(rom, L"buriki.zip");
    if (!bios_given) guess_rom(bios, L"hng64.zip");
    if (!dest[0]) {
        wchar_t la[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, la)))
            swprintf(dest, MAX_PATH, L"%ls\\Programs\\%ls", la, P_PRODUCT);
    }

    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HDC sdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);
    measure_art();
    {   /* on a small or heavily scaled desktop, draw everything a little smaller so the whole window fits */
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int fit_w = MulDiv(wa.right - wa.left - 16, 96, g_win_w), fit_h = MulDiv(wa.bottom - wa.top - 48, 96, WIN_H);
        if (fit_w < g_dpi) g_dpi = fit_w;
        if (fit_h < g_dpi) g_dpi = fit_h;
        if (g_dpi < 60) g_dpi = 60;
    }

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"BurikiSetup";
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = {0, 0, S(g_win_w), S(WIN_H)};
    AdjustWindowRect(&r, style, FALSE);
    g_wnd = CreateWindowW(L"BurikiSetup", APP_TITLE, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                          r.bottom - r.top, NULL, NULL, inst, NULL);
    dark_title_bar(g_wnd);
    create_controls();
    SetWindowTextW(g_rom, rom);
    SetWindowTextW(g_bios, bios);
    SetWindowTextW(g_dest, dest);
    /* centre on the screen */
    RECT wr, wa;
    GetWindowRect(g_wnd, &wr);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    SetWindowPos(g_wnd, NULL, wa.left + (wa.right - wa.left - (wr.right - wr.left)) / 2,
                 wa.top + (wa.bottom - wa.top - (wr.bottom - wr.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(g_wnd, g_silent ? SW_SHOWMINNOACTIVE : show);
    UpdateWindow(g_wnd);
    if (g_shot[0]) {
        MSG m;
        for (int i = 0; i < 20; i++) {
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
            Sleep(15);
        }
        save_shot();
        return 0;
    }
    if (g_silent) go();
    else SetFocus(g_go);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    CoUninitialize();
    return g_exit_code;
}
