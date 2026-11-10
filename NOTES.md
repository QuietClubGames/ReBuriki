# Buriki One recomp: working notes

## Folder layout
- `buriki.zip`, `hng64.zip`: original dumps (keep untouched)
- `roms/buriki`, `roms/hng64`: extracted copies
- `work/buriki_program_be.bin`: rebuilt main program image (big-endian, 8 MB)
- `tools/`: analysis scripts (`build_image.py` rebuilds the image above)
- `work/buriki_code_80000000.bin`: the 1 MB code block on its own (ROM 0x100000–0x1FFFFF)
- `work/ghidra/`: Ghidra project `buriki` (code loaded at 0x80000000)
- `third_party/`: toolchain (see below)
- `recomp/`: N64Recomp config (`buriki.toml`), generated C (`generated/`), the hand-written
  runtime (`runtime/`), and the built `buriki.exe`
- `work/recomp_rom.bin`: game code + BIOS joined into the single ROM file N64Recomp reads

## Toolchain (set up 2026-11-09, all in `third_party/`, nothing installed system-wide)
- `N64Recomp/` (commit 598ce20), built with clang: `N64Recomp/build/N64Recomp.exe`.
  Supports "raw ROM + symbols file" input (`[input] rom_file_path` + `symbols_file_path`),
  which is our route since there's no ELF.
- `mame/`: sparse checkout (commit d077ea2) holding only `src/mame/snk/hng64*` (driver,
  3D, sprites, video, audio) plus the MIPS, NEC V-series and TLCS-870 CPU cores.
  This is the hardware reference.
- `ghidra_12.2_PUBLIC/`: needs Java 25. It's pointed at `jdk-25.0.5+8/` via
  `support/launch.properties`. Headless runs:
  `support/analyzeHeadless.bat`, with scripts in `tools/ghidra_scripts/`.
  The first auto-analysis of the code block found 2,118 functions; the list is in
  `work/ghidra_functions.txt`.
- Downloads were checked against the published SHA-256 checksums. The installer zips are
  in `third_party/downloads/` (safe to delete).

## Findings (2026-11-09)

### BIOS (`hng64.zip`)
- `brom1.bin` (plus the `bios_us` / `bios_export` / `bios_korea` variants) is 512 KB of
  big-endian MIPS code. The CPU starts executing at its first byte (reset vector 0xBFC00000).
- The game calls BIOS services through a jump table at 0x9FC08000 (BIOS + 0x8000).
  So the recomp must either recompile the BIOS too or reimplement those services by hand.
- `tmp87ph40an.bin`: I/O microcontroller (inputs/coins).
- `rom1.bin`: starts with "POF", so it's an Altera FPGA configuration file. It's hardware
  config, not code.
- `from1.bin`: blank (all zeros).
- `lvs-*.u2/u3`: small 256-byte EEPROM/config chips.

### Game program (`007pr01b` + `007pr02b`)
- These two ROMs interleave in 16-bit chunks. Read the result as little-endian 32-bit words
  and byte-swap to get normal big-endian MIPS code.
- Header: "Buriki One" at 0x0, "HYPER NEOGEO64" at 0x60, boot code at about 0x88.
- **Main game code is ROM 0x100000–0x1FFFFF (1 MB, about 2,800 functions).** Calls in it
  point to addresses 0x100000 lower, so this block is copied to the start of RAM
  (0x80000000) and runs from there. 966 of 1,161 call targets match that rule.
- The rest of the 8 MB is data.
- `007pr03a`–`06a` don't decode as code under any byte order. They're data
  (models, animations, or similar).

### Graphics/sound ROMs
- `sc` = tilemaps, `sp` = sprites, `tx` = 3D textures, `vt` = polygon data,
  `sd` = sound program and samples.

## First recompile (2026-11-09): done

Rebuild everything with `tools\build_recomp.ps1` (add `-Ghidra` to redo function discovery).

1. Ghidra auto-analysis, then `tools/ghidra_scripts/SeedAndExport.java`. That script adds
   every call target, plus the code right after each return, as function entries.
   Result: **2,811 functions**, and the code ends at 0x800D1498. Everything after that in
   the 1 MB block is data.
2. `tools/make_symbols.py` writes `recomp/buriki.syms.toml`: one section at
   vram 0x80000000, with each function cut at the next one's start.
3. `tools/find_kernel_funcs.py` found only **3 functions** using CPU control instructions
   N64Recomp can't translate (listed in `recomp/kernel_funcs.txt`). They're marked
   `ignored` in `recomp/buriki.toml`, with placeholders in `recomp/runtime/kernel.c`:
   - `func_800C2420`: exception handler (reads Cause/EPC)
   - `func_800C5C90`: TLB setup (not needed with a flat memory map)
   - `func_800C5D30`: reads the cycle counter. The placeholder *assumes* it returns in v0;
     confirm in Ghidra.
4. N64Recomp translated all 2,808 remaining functions with no errors (about 22 MB of C in
   `recomp/generated/`). Its only messages were 4 notes about tail calls.
5. clang compiles all of it with no errors. `recomp/buriki_linktest.exe` links and runs.
   It does **not** start the game yet.

What the generated code expects from the runtime (placeholders in `recomp/runtime/stubs.c`):
`get_function` (function-pointer lookup table), `cop0_status_read/write`, `switch_error`,
`do_break`, `pause_self`, `recomp_syscall_handler`.

Not yet verified:
- Whether every function boundary is right. A wrong split usually shows up later as a
  crash or a jump-table error. The boot step will expose these.
- The float mode (`uses_mips3_float_mode`). It's left at the default; check the FR bit
  the BIOS/game writes to Status.
- Code outside this 1 MB block: the BIOS services at 0x9FC08000, plus any code the game
  loads elsewhere at runtime.

> Corrections from step 3: the float mode *is* FR=1, and the game *does* call BIOS services.
> The 3 placeholder kernel functions are now real implementations (`runtime/kernel.cpp`),
> and `func_800C5D30` turned out to be the random number generator. See below.

## Step 3, boot to the main loop (2026-11-09): done

Build: `tools\build_recomp.ps1`. Run headless: `tools\run_recomp.ps1 -Frames N` (log in
`recomp\run.log`). Exe options: `--frames N`, `--ticks-per-frame N`, `--io-log N`,
`--trace-io`, `--roms DIR`.

**Result:** the game boots and runs its frame loop for 3,600+ frames (one minute of game
time) with no errors. Vblank (irq 0) and "3D list processed" (irq 3) interrupts are
delivered. The game uploads its sound program and starts the sound CPU, initialises save
memory, video registers, tilemaps and palette, and starts sending 3D display lists around
frame 770. Two runs give an identical RAM fingerprint: **fully deterministic**.
Nothing is shown on screen yet (no renderer), so "how far into attract mode" is unverified.

### How booting works (from BIOS 0xBFC06444)
- The cart header gives the copy: +0x50 source (0x04100000), +0x54 destination (0),
  +0x58 word count (0x340000 = 13 MB). So RAM 0x80000000-0x80D00000 = cart 0x100000+.
- Then the BIOS copies its service block (BIOS 0x8000-0xBFFF) to RAM 0x80004000. The game
  ROM has zeros there.
- It jumps to header +0x5C = **0x80008000** (sets sp=0x80400000, gp=0x800F11D0, copies data,
  clears BSS, calls main at 0x800080B0). The heuristics missed this entry point, so it's
  listed in `tools/extra_functions.txt`.
- The runtime does these copies itself (`hw_boot`) and skips the BIOS self-tests.
- CPU state the BIOS leaves: Status = 0x34000000 (CU1, CU0, **FR=1**), FCSR = 0x01000000.

### Memory map (`runtime/hw.cpp`, from MAME `main_map`)
Physical: RAM 16 MB @0, cart 32 MB @0x04000000, sysregs @0x1F700000 (irq ctrl +0x1100,
DMA +0x1200, RTC +0x2100), nvram @0x1F800000, I/O dualport @0x1F808000, BIOS @0x1FC00000,
sprites @0x20000000, tilemaps @0x20100000, video regs @0x20190000, palette @0x20200000,
tcram @0x20208000, 3D list @0x20300000, framebuffer @0x30000000, sound RAM @0x60000000,
sound comms @0x68000000. Everything above 0x1FFFFFFF is reached through the TLB, which the
game sets up (19 entries; e.g. 0xC0000000 -> 0x20000000, 0xE0000000 -> 0x60000000).

### Runtime design
- **Memory**: generated code is compiled as **C++** so `MEM_W(...)` etc. become proxy objects
  (`runtime/hw_mem.h`). RAM through KSEG0/KSEG1 is an inline fast path; everything else goes
  to `hw_read*/hw_write*` (TLB, then device registers or memory areas). Storage is
  host-native 32-bit words, like N64Recomp. `runtime/recomp.h` is generated from
  N64Recomp's header by `tools/make_recomp_header.ps1`.
  `tools/fix_generated.py` makes the output valid C++ (it splits `jr_addend` declarations).
- **Time**: deterministic "ticks" = function entries + branch labels (the fix-up script adds
  `HW_LABEL_TICK()` after every label, so loops without calls still advance time).
  One frame = 50,000 ticks (`--ticks-per-frame`). This number is a guess and not tuned.
- **Interrupts**: at those safe points, if an irq is pending and Status has IE + IM2 with no
  EXL, the runtime saves the whole CPU context, calls the game's dispatcher
  `func_800C2B20` (it reads the level at 0x1F701104 and jumps through the table at
  0x80000400), then restores the context. Buriki's handlers: vblank 0x800C49C4, etc.
- **Float mode**: FR=1, so `uses_mips3_float_mode = true` and `ctx->f_odd = &ctx->f1.u32l`.
  Getting this wrong (the N64 value `&f0.u32h`) made odd float registers read garbage:
  that was the NaN bug.
- **NaN checks** log instead of aborting (`[nan]` lines); there are none now.
- **Diagnostics**: `[io]` first access to each hardware address, `[tlb]`, `[dma]`,
  `[sound]`, `[io-mcu]`; a summary table at exit. A watchdog reports a hang after 3 s.
  On a fatal error it prints the last 64 functions entered.

### BIOS services
The game calls the RAM copy of the BIOS services through function pointers (e.g.
0xA0005B24 = BIOS 0x9B24, cache flush). `runtime/stubs.cpp:get_function` maps
0x80004000/0xA0004000/0x9FC08000/0xBFC08000 + offset onto one copy at 0x9FC08000.
The BIOS is hand-written assembly that N64Recomp mis-parses in places, so the `.bios`
section in `tools/sections.py` only includes listed functions (`only=`). All 4 listed so far
use CPU control instructions and are hand-written in `kernel.cpp` (TLB write, cache
control). If the game calls another service, the runtime stops with
"indirect call to ..."; add it to `only=` (or hand-write it).

### Kernel functions (`runtime/kernel.cpp`, listed `ignored` in `buriki.toml`)
`func_800C2420` crash screen (stops with a report), `func_800C5C90` TLB write,
`func_800C5D30` random number generator (uses a tick-based Count, so it's deterministic),
`func_9FC08100` BIOS TLB write, `func_9FC09300` BIOS TLB setup, `func_9FC09B04` and
`func_9FC09B24` cache control (no-ops). The build fails if `find_kernel_funcs.py` finds one
that isn't in the ignored list.

### Known gaps (next steps)
- **I/O MCU** isn't emulated. The game polls dualport 0x002 twice a frame; it reads 0
  (probably "no buttons"). The MAME hack for dualport 0x600 (= 1, fighting board) is in.
- **Sound CPU** isn't emulated. The game uploads 1 MB to sound RAM, turns the V53 on and
  polls the sound latch at 0x68000006 every frame; it appears to time out gracefully.
- **No video output** yet: that's step 4.
- Ticks per frame is untuned; if the game needs more work per frame than that, vblank cuts
  in early (gameplay would run slow, not break).

## Step 4, pictures (2026-11-09): done

`recomp\buriki.exe` opens a 1024x896 window (512x448 doubled) at ~59.43 Hz. Esc or closing
the window quits. Headless + frame dumps: `tools\run_recomp.ps1 -Frames N -Extra
'--dump-dir DIR --dump-every 60'`, and `tools\contact_sheet.py DIR out.png [div] [cols] [first] [last]`
turns the .bmp dumps into one PNG.

**Result:** the whole attract sequence draws correctly: HNG64 logo, warning, SNK logo, title,
the fighter intros ("KARATE", "SUMO WRESTLING") with **textured, lit 3D fighters**, arena,
ring floor, and blended Japanese text overlays. Headless speed is ~160 fps, so a window has
plenty of headroom. The RAM fingerprint at frame 3600 is unchanged from step 3, so rendering
has no effect on game logic.

### Renderer (`runtime/video.cpp`, `runtime/video3d.cpp`)
- A port of MAME's `hng64_v.cpp`, `hng64_sprite.ipp`, `hng64_3d.ipp` and the triangle /
  clipper parts of `devices/video/poly.h`, without MAME's tilemap/gfx/bitmap classes.
  Function names match MAME for side-by-side comparison. **Licence: MAME's hng64 files are
  LGPL-2.1+ (poly.h is BSD-3), so these two files carry that licence.**
- Graphics ROMs are loaded and arranged the way MAME's `ROM_START(buriki)` does: scroll tiles
  are 16-bit interleaved plus `hng64_reorder`, sprites are 32-bit interleaved, textures are 16
  pages of 1024x1024, and vertex ROMs are 3-way word-interleaved little-endian.
- Tiles and sprites are decoded on the fly from MAME's gfx_layout bit offsets (nothing is
  pre-decoded). Palettes, including both fade palettes and the 16-brightness 3D palette, are
  rebuilt once per frame from palette RAM + tcram.
- The whole frame is drawn at vblank (MAME can split mid-frame on raster interrupts; Buriki
  hasn't needed that so far). The 3D display list is rendered on each upload to 0x20300200,
  as MAME does. The 3D buffers are cleared at vblank when tcram 0x50 bit 16 is set.
- Sprite-clear registers (0x2000D800-0x2000EFFF) are implemented in `hw.cpp`.
- Output: a plain Win32 window (StretchDIBits), with no extra libraries.

### Fix: "how to play" 3D window (deviation from MAME, 2026-11-09)
The projection packet (0x0012) words 10-13 are the 3D window in screen pixels from the screen
centre. Every scene sends full screen (+-256 x +-224), except "how to play": right 0x60,
left -0xE0, top 0x90, bottom -0x60 (screen x 32-352, y 80-320, the framed monitor), and words
1-2 = -0x40, +0x18. MAME stretches the window over the whole screen (fighters ~1.7x too big,
outside the frame). We project at normal scale, shift the image by words 1-2 (x, and y up),
and clip to the window (`setCameraProjectionMatrix` in `video3d.cpp`). Checked against
hardware footage the user supplied: the fighters now sit centred in the frame, feet on the
health-bar line. Sample frames outside how-to-play are byte-identical before and after.
Debug: set env `BURIKI_LOG3D=1` to log each distinct projection packet.

### Known gaps / next steps
- MAME's own known HNG64 graphics inaccuracies carry over (some blending/priority guesses,
  the "how to play" 3D position).
- No raster-interrupt mid-frame splits.
- **No input** yet (the I/O MCU isn't emulated). **No sound** (the V53 + L7A1045 DSP aren't
  emulated).
- Enhancements (high resolution, widescreen) need the 3D side moved to a GPU or a
  higher-resolution software path. The current renderer is the accurate baseline to
  compare against.

## Step 5a, controls (2026-11-09): done

**Result:** coin -> title -> PLAYER SELECT -> VS -> READY -> a full match works, tested both
with a script and by keyboard.

- **I/O board** (`runtime/io_board.cpp`): runs the real TMP87PH40AN firmware on a TLCS-870 core
  (`runtime/iomcu/`, ported from MAME by a helper agent; BSD-3). Wiring follows MAME's ioport
  handlers: port 1 selects one of 8 input ports read on port 3; port 7 drives the dual-port RAM
  address counter, and its bit-7 falling edge raises main-CPU irq 17; port 0 is the data. A
  main-CPU write to 0x1F7021C4 pulses the MCU's INT0. The MCU gets 8 MHz worth of cycles from
  the tick clock (`io_board_run` every 256 ticks and before any dual-port access), so it stays
  deterministic. Its power-on RAM test takes ~1 s of emulated time; the game waits for it.
- **Controls** (`runtime/input.cpp`), sampled once per frame at vblank:
  P1 arrows + Z X C V, start 1, coin 5. P2 R F D G + A S Q W, start 2, coin 6. Service 9,
  Test F2. XInput gamepads 1/2: stick/d-pad, X Y A B = buttons 1-4, Start, Back = coin.
  Coins are short pulses, like MAME's PORT_IMPULSE.
- **Scripted input** for tests: `--input-script "1300=coin1;1306=;1400=p1start;1406="`.
  From each frame on, exactly the listed controls are held.

## Step 5b, sound (2026-11-09): done

**Result:** music and effects play through the speakers (Windows waveOut, no extra
libraries). `--wav FILE` records them and `tools\wav_stats.py FILE` prints the loudness over
time. Two runs give a byte-identical WAV and the same RAM fingerprint.

- **Sound CPU** (`runtime/audiocpu/`): NEC V53A (V33A core + TCU/ICU/DMAU/SCU), ported from
  MAME by a helper agent (BSD-3). It runs the sound driver the game uploads ("Sound Driver
  Ver 1.15").
- **Sound chip** (`runtime/l7a1045.cpp`): L7A1045 L6028 DSP-A, a port of MAME's (BSD-3). It
  has 32 voices at 44.1 kHz. Only the stereo pair is mixed; the rear and subwoofer sends are
  dropped.
- **Board glue** (`runtime/sound.cpp`, after MAME `hng64_a.cpp`, LGPL):
  - The V53 sees 16 x 64 KB banks of the 2 MB sound RAM. That RAM is the main CPU's
    0x60200000 area, and V53 byte B = main-CPU byte 0x60200000+B.
  - I/O map: 0x00-0x0F sound chip, 0x100-0x10F mailboxes, 0x200-0x21F bank select.
  - Interrupts: timer 2 -> IRQ2; a main-CPU write of 1 to 0x68000008 -> IRQ5, cleared by
    the V53 writing port 0x10A.
  - 0x6F000000: 0x55AA starts the sound CPU, 0xAA55 holds it.
- **DMA:** the sound chip raises a DMA request every 64 of its clocks (60.47 V53 clocks)
  while DMA is on. **Getting this wrong was the "silent" bug**: at 8 samples per request, the
  driver's sample-RAM clear took 12 s, so the game gave up on sound. Unlike MAME, 16-bit DMA
  memory reads are also bound, so 16-bit transfers carry real data.
- **Timing:** the V53 gets 32 MHz from the tick clock. The chip's output time comes from the
  V53 cycle count, and the chip catches up before every register access (MAME
  `stream->update()`).
- **Debug:** set env `BURIKI_LOGSND=1` to log mailbox traffic, chip control writes, and a
  status line every 30 frames.
- **Speed:** ~79 fps headless with sound (needs 59.4). The V53 interpreter is now the biggest
  cost; it can be optimised if it ever gets tight.

## Step 6b, high-resolution 3D (2026-11-09): done

Display > Graphics: Enhanced (default) or Original; F9 toggles. Quality Auto renders the 3D at
the window's resolution; the presets render the 512x512 buffer at 2x-8x.

- **Recording** (`video3d.cpp`, `hd3d.h`): every polygon the software rasteriser draws is also
  recorded as triangles with full-precision window x/y, depth, clip w, s/t, light, and its
  palette/texture parameters and clip rectangle. `video3d_hd_snapshot()` copies the list when a
  frame is finished, before the vblank clear, so the pause menu can redraw it.
- **GPU** (`hd3d.cpp`, Direct3D 11 via the front end's `hd_hook.h`):
  - The texture ROM goes to the GPU as an R8_UINT array; the 16-brightness 3D palette is
    uploaded each frame.
  - The 3D pass uses the same page/wrap/scroll maths and the same 16-bit colour rule as MAME,
    with a depth test (less) and 4x MSAA. Enhanced also smooths textures: a palette-aware
    bilinear filter that weights out transparent texels.
  - Shaders are compiled at start-up with Windows' d3dcompiler_47.dll.
- **2D split** (`video.cpp`): with Enhanced on, `screen_update` copies the picture before the
  3D into `g_below`. It then draws the front layers a second time into `g_over`, recording
  how each pixel combines with what's behind (alpha FF opaque, C0 50%, 80 additive).
- **Composite** (`hd3d.cpp`): below -> 3D (50% where the hardware blends) -> front layer, with
  MAME's 3D row/scroll mapping. The 2D follows the screen filter (Sharp = nearest, Smooth =
  bilinear).
- The software 512x448 picture is still made every frame. It's used for dumps, for Original
  mode, and whenever the screen can't be split (screen off).
- **Checked:** the intro and the how-to-play screen (viewport + offset) match the software
  picture's layout, with smooth 3D edges and textures. Full 59 fps either way on an RTX 5070 Ti.
- **Not done yet:** widescreen. (Menu art: done, see "Menu art" below.)

## Review, 2026-11-10

Checked:
- **8-minute scripted play** (`recomp/soak_script.txt`, pseudo-random moves): character
  select, several matches, VS screens, continue, game over, title, new credit, result
  screen. No crashes, hangs or NaNs.
- **CPU budget:** with `BURIKI_LOGVBL=1`, every vblank in fights lands in the idle loop
  `func_800C5C20`, so 50,000 ticks/frame is enough and the game never drops frames. Only scene
  loads (big clears) run past a vblank.
- **Speed** (`[perf]` lines): fights ~100 fps headless with sound, after the parallel 2D
  renderer. The sound board (V53 interpreter) is now the largest cost, ~40%.
- **How-to-play gauges:** present in arcade footage too.

Resolved from the game's own register writes (2026-11-10), without arcade footage:
- **Intro discipline-name screens: FIXED.** The fade register per background group is
  chosen by tcram 0x14 bits 16-17 (bit 16: group L0 uses fade 1; bit 17: group L1 uses fade 1).
  MAME hard-codes the 0001 case. On the style intros the game sets 0007 and loads fade 1 with
  full black, so the arena now fades out behind the title, portrait and 3D fighter. Only those
  5 screens change across the attract loop and 12,000 frames of play. Details in
  `video.cpp` (tilemap_draw_roz_core_line); `BURIKI_MAME_FADE=1` restores MAME's mapping.
- **Fighter entrance "door run": nothing to fix.** Projection words 14-15 hold one value for
  the whole entrance while the camera moves and zooms, so they aren't camera data; most likely
  leftovers in the packet. The renders look right.

Diagnostics for this kind of work: `BURIKI_LOGVID=1` (video/transition register changes per
frame), `BURIKI_HIDE_TM=mask` (hide tilemap layers), `BURIKI_LOG3D=1` (projection packets).

Known gaps (graphics: carried over from MAME's own unknowns, see hng64_v.cpp/hng64_3d.ipp):
- **Tile animation:** the enable bit (videoregs[1] bit 16) is a guess.
- **Sprite-vs-3D priority:** a few doubtful cases.
- **Mosaic + line-scroll:** the interaction is unknown.
- **Raster interrupt (irq level 1):** not generated. Buriki's handler (0x800C4748) only acks
  it, so it has no effect.

Other gaps:
- RTC reads 0 (the operator menu clock).
- The I/O board's serial EEPROM uses MAME's dualport 0x600 hack.
- Sound: the rear/sub outputs and the L7A1045 delay effect aren't implemented (MAME doesn't
  have them either).
- Widescreen, an installer/release package, and a README for players.

## Roadmap

1. **Toolchain**: N64Recomp (MIPS to C translator), Ghidra (disassembler), and a MAME
   build to use as the reference.
2. **First recompile**: turn the 1 MB code block (and the BIOS) into C that compiles,
   even if nothing runs yet.
3. **Boot to a log**: memory map, BIOS calls, and interrupts, until the game reaches
   its main loop. Compare against MAME traces.
4. **Pictures**: port MAME's HNG64 renderer as a correct software baseline.
5. **Sound and input**: emulate the sound CPU and chip, and map controllers through SDL.
6. **Enhancements**: GPU renderer for high resolution and widescreen, then extras.
7. **Netplay/rollback (later)**: the groundwork is keeping all game state in one memory
   block that can be saved and restored, and keeping timing deterministic.
   Plan for that from step 3 on.

## Step 6a, front end (2026-11-09): done

Ported from ReLayer's PC front end (MIT; `recomp/runtime/frontend/`, licence in `LICENSE-ReLayer.txt`).
The build compiles `frontend/` with the other runtime subfolders and links d3d11, dxgi and ole32.

- **Presentation** (`fe_d3d11.cpp`, `fe_window.cpp`): Direct3D 11 flip-model swap chain, GDI fallback
  (`BURIKI_FORCE_GDI=1` to try it). Window / borderless / exclusive full screen (Alt+Enter, F11), window
  sizes, monitor, picture shape (4:3, stretch, whole multiples of 448 lines), filter (sharp, smooth,
  scanlines at 224 line pairs), FPS counter. Pacing: the arcade's 59.43 Hz timer by default; "Display
  vsync" in the menu locks to a ~60 Hz display (1% fast).
- **Pause menu** (`menu.cpp`, `fe_menu_host.cpp`): Tab / Esc or Back + Start. Esc no longer quits; quit
  from System or by closing the window. The pause is inside `video_frame`: `fe_present_frame` loops
  (present, menu, messages) without returning, so no emulated time passes; waveOut is paused meanwhile;
  controls are kept from the game while it's open and until the closing button is released. The watchdog
  in main.cpp also watches `fe_activity()`, so a long pause isn't reported as a hang.
- **Settings**: `settings.ini`, `controls.ini` (keys + pad buttons per player), `nvram.bin` next to the exe.
  Headless runs ignore the region (Japanese BIOS as before) and keep no save memory unless `--nvram FILE`.
- **Region** (menu Game page): Japan `brom1.bin`, USA `bios_us.bin` (default), World `bios_export.bin`;
  applies after restart. **Restart** relaunches the exe with the same command line.
- **Save memory** (`fe_nvram.cpp`): the 16 KB nvram at 0x1F800000, big-endian in the file; loaded after
  hw_init, saved when changed (checked once a second) and at exit. Checked: a second run loads it and the
  game's counters carry on (bytes 0x2087 / 0x20B3 went 1 -> 2).
- **HD hook** (`frontend/hd_hook.h`): `fe_set_hd_draw(fn)`; when set and Graphics = Enhanced, `fn(rtv, x, y,
  w, h)` draws the picture area and the scanlines, menu backdrop, menu and FPS counter go on top.
- **Tests**: `--menu-test PAGE[:pad] --menu-test-at F --menu-shot X.bmp` (pages display audio controls game
  system capture none toast pause), `--test-keys "N:key,..."`, `--set key=value`. The 2400-frame scripted
  run (`1300=coin1;1306=;1400=p1start;1406=`) still gives RAM fingerprint 465E4B3381689AC5, headless and
  windowed with a 4 s menu pause at frame 1350.

## Menu art (2026-11-10): done

The pause menu shows one of the ten fighting-style portraits from the attract mode's style-intro
screens, a different one each time it opens (shuffled rounds, never the same twice in a row).

- **Nothing is shipped.** `runtime/menu_art.cpp` holds only recipes: each portrait's sprite tile
  numbers and block positions, as the game's own sprite list has them. At start-up (after
  `video_init`, ~instant) `video_build_sprite_picture` builds the pictures from the player's sprite
  ROMs with the 256-colour palette at **cart offset 0x3C3440**. The game loads that palette into
  bank 4 for these screens and dims entries 209-223 to ~62%. We use the undimmed ROM colours.
  The 0x3C0400 table only shares the first 17 UI colours.
- How they were found: `BURIKI_SPRITES=<frame>` on each style intro. The portrait is the chained
  block at depth 0x2F5. `tools/find_palette.py` and `tools/find_colours.py` search the cart for a
  captured `[pal N]` palette.
- `BURIKI_DUMP_ART=<dir>` saves them as `portrait_NN.bmp`.
- Front end (`fe_ui_d3d11.cpp`):
  - one texture per portrait; their black backdrop is made see-through;
  - drawn whole at full panel height against the right edge (the 432-wide Professional
    Wrestling one is cropped on the left), under a lighter gradient than photo art gets;
  - `uip_art_next()` runs on each menu open;
  - a `menu_art.png`/`.jpg` next to the exe still replaces them (drawn to cover the panel).
- The eighth portrait (second loop, third style) is Aikido (confirmed by the user). Names aren't shown anywhere.
