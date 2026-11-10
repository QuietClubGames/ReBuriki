# Developing ReBuriki

ReBuriki is a static recompilation: the game's MIPS code is translated to C by
[N64Recomp](https://github.com/N64Recomp/N64Recomp) and compiled with a hand-written runtime that stands in
for the Hyper Neo Geo 64 hardware. `NOTES.md` is the project diary: what was found, how, and what is still
open. This page is the short version of how to build and test.

Nothing made from the ROMs is ever committed or shipped. The repository holds no game code or data; builds
are made from your own ROM sets.

## Layout

| Path | What |
|---|---|
| `recomp/buriki.toml` | N64Recomp configuration |
| `recomp/runtime/` | The runtime: memory map and interrupts (`hw.cpp`), 2D and 3D video (`video.cpp`, `video3d.cpp`), the Direct3D 11 renderer (`hd3d.cpp`), sound board (`sound.cpp`, `l7a1045.cpp`, `audiocpu/`), I/O board (`io_board.cpp`, `iomcu/`), front end (`frontend/`) |
| `tools/data/` | Function address lists for the game code and BIOS (addresses only) |
| `tools/*.py` | Build steps: symbols, function table, header, fixes to the generated C |
| `tools/install/` | The installer: `setup_win32.c`, `build_from_rom.py`, `make_package.py`, names in `product.py` |
| `licenses/`, `LICENSE.md` | Licences |

## What you need

- Windows 10 or 11, 64-bit.
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) on PATH (`clang++`, `x86_64-w64-mingw32-clang++`).
- Python 3.11 or newer (Pillow as well, for packaging).
- N64Recomp built in `third_party/N64Recomp/build/N64Recomp.exe`.
- Your own `buriki.zip` and `hng64.zip`.

## Building

The quickest way is the same script Setup runs. It checks the ROM sets, translates, compiles and installs
into a folder of your choice:

    python tools/install/build_from_rom.py --rom buriki.zip --bios hng64.zip --dest dist/dev --keep-work

`tools/build_recomp.ps1` is the older developer build (unpacked ROMs in `roms/`, output `recomp/buriki.exe`,
a console program). Its paths assume the repository is at `D:\Buriki`.

## Running and testing

The exe takes options for automated runs (the full list is at the top of `recomp/runtime/main.cpp`):

    ReBuriki.exe --headless --no-sound --frames 3000 --input-script "1300=coin1;1306=;1400=p1start;1406="
    ReBuriki.exe --frames 2500 --no-throttle --shot-every 300 --shot-dir shots --set picture_shape=3

- `--headless` runs without a window as fast as possible, with the Japanese BIOS, a zero clock and no save
  memory, so two runs with the same input script give the same result. At the end it prints a fingerprint of
  main RAM.
- `--dump-dir` / `--dump-every` save the 512x448 picture; `--shot-every` saves the window.
- `--set key=value` changes a `settings.ini` value for one run. `--test-keys` presses menu keys.
- Online play (in development, not in the menu yet): `--netplay-host PORT` on one copy and
  `--netplay-join ADDRESS[:PORT]` on the other; `--netplay-delay N` on the host sets the input delay.
  `python tools\netplay_test.py` runs two linked copies through a simulated bad connection and checks
  they end up identical to an ordinary run; `tools\netplay_demo.bat` shows two linked windows on one PC.
  See "Online play" in `NOTES.md`.
- Environment variables for diagnostics are listed in `NOTES.md` (`BURIKI_LOG3D`, `BURIKI_SPRITES`,
  `BURIKI_RAMDUMP`, `BURIKI_POKE`, `BURIKI_LOGFX` and others).

The rule for changes to the picture or timing: the 512x448 software picture of a headless run must stay
byte-identical unless the change is a deliberate accuracy fix.

## Packaging

    python tools/install/make_package.py

makes `dist/ReBuriki-Setup/` and `dist/ReBuriki-Setup.zip`: Setup.exe, a trimmed Python, a trimmed clang++,
N64Recomp and this project's source. Test it by unzipping somewhere clean and running
`Setup.exe /rom=... /bios=... /dest=... /silent /noshortcuts /noregister` with PATH cut down to Windows' own
folders.
