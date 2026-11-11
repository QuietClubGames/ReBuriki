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
- Widescreen and menu art: done, see "Widescreen" and "Menu art" below.

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
- (Installer and player README: done, see "Installer" below.)

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

## Widescreen (2026-11-10): done

Display > Picture shape > **Widescreen** (`picture_shape=3`, needs Enhanced graphics). Fights and
the arena demo scenes fill the window's shape, from 4:3 up to 21:9. Everything else (title,
player select, VS, bracket, how to play, style intros) stays 4:3 in the middle. The HUD keeps its
arcade position. In a window, the window sizes become 16:9 at the same height.

What the game's picture is made of decided the design: in fights **only the fighters are 3D**. The
arena, crowd and floor are 2D tilemaps (the floor is a "line mode" perspective tilemap).

- **2D** (`video.cpp`): the tilemaps are 1024+ pixels wide and the hardware shows a 512-pixel
  window of them. With widescreen on, the two HD layers are drawn 912 wide (`HD_WIDE_MARGIN` =
  200 extra columns each side) by carrying the same per-line maths past the screen edges.
  - Sprites get the same margin (their x is 10-bit signed, so nothing wraps into view).
  - The back layers are drawn once, wide; the normal 512x448 picture starts as their middle 512
    columns, so it costs little extra.
  - Mosaic blocks are still counted from screen x = 0. Without that the 4:3 picture changed by a
    few hundred pixels near the horizon.
- **3D** (`video3d.cpp`): each polygon is also clipped to a view 4x wider than the screen
  (`polygon::hd`) and that copy is what the GPU gets. The software rasteriser still uses MAME's
  clip. The GPU clips to what it shows.
- **GPU** (`hd3d.cpp`): `wide` = output shape / (4/3). The 3D target is `wide` times wider and
  the vertex shader divides x by it. The compositor samples the 2D layers with the margin.
- **When it goes wide** (`wide_scene`): a line-mode tilemap is enabled and not faded to black
  (`HdLayers::arena`), and every polygon uses the full-screen window. The fade test keeps the
  style intros 4:3: their arena is blacked out and the opponent waits just off screen.
  `BURIKI_WIDE_ALWAYS=1` widens everything, which shows why the flat screens don't: their
  tilemaps wrap around and repeat in the margins.
- **Front end**: `picture_rect_wide` (as tall as the 4:3 area, up to 64:27 wide). If the HD
  renderer can't draw the frame, the normal 4:3 picture is shown.
- **Checked:**
  - The software picture is byte-identical with widescreen on and off (40 frames over the first
    6000 of the soak script, windowed), and headless still matches `frames_soak_ref`.
  - 15000-frame soak in 16:9: fights, READY/FIGHT, K.O., replay, winner, bracket, VS, continue.
  - 21:9 window with scanlines.
- **Known:** the winner banner's dark bar is only 4:3 wide.
- **Possible, not seen in testing:** a sprite that slides in from off screen would first appear
  inside the margin, at its starting position.
- **Test options added:** `--shot-every N --shot-dir DIR` (window screenshots),
  `BURIKI_WINDOW=WxH` (any window shape).

## Installer (2026-11-10): done

`python tools/install/make_package.py` makes `dist/ReBuriki-Setup/` and `dist/ReBuriki-Setup.zip`
(311 MB unpacked, 78 MB zipped). Modelled on ReLayer's installer (`tools/install` in D:/ReLayer).
The package has **no ROM data and nothing made from the ROMs**: Setup builds the game on the
player's PC from their own `buriki.zip` and `hng64.zip`.

- **Names** are in one place, `tools/install/product.py`: product "ReBuriki", exe `ReBuriki.exe`,
  version 0.1.0, publisher QuietClubGames. The user confirmed the name ReBuriki (2026-11-10).
- **Package contents:** `Setup.exe`, `README.txt` (player guide), `licenses/`, and `files/`:
  - `python/`: trimmed Python, standard library only;
  - `clang/`: 64-bit clang++ from llvm-mingw with the C++ and Windows headers and libraries;
  - `n64recomp/`: `N64Recomp.exe` and its `recomp.h`;
  - `source/`: the tracked build inputs (`recomp/runtime`, `recomp/buriki.toml`, the build tools,
    `tools/data`).
- **`Setup.exe`** (`setup_win32.c`, adapted from ReLayer's): two ROM rows, install folder,
  shortcuts, progress from the build's `@@progress` lines, `setup.log`, per-user "Apps" entry,
  and `Uninstall.exe` (asks whether to keep settings). It finds the zips next to itself, one
  folder up, or in Downloads. Test switches: `/rom= /bios= /dest= /silent /noshortcuts
  /noregister /shot=`.
- **Look (2026-11-10, asked for by the user):** dark, in the pause menu's colours. Only the text
  boxes and buttons are Windows controls (buttons and tick boxes owner-drawn); labels, hints,
  progress bar and status are painted over a cached background. The title bar is dark on
  Windows 10 2004+.
- **Setup picture:** `setup_art.png` (or .jpg/.webp) in the repo root becomes
  `files/setup_art.png`, shown full height down the right of the window under a dark overlay
  that fades it into the panel. The window widens to fit the picture's shape. Without the file
  the window is the plain dark column. The picture is official Buriki One artwork; the user
  chose to keep it in the repository and the package (2026-11-10).
- **Icon:** `icon_art.png` (or .jpg/.webp) in the repo root becomes the icon of `Setup.exe` and of
  the game: on a black rounded tile, with a thin light outline so the logo's dark navy lettering
  shows against the black. Without the file it's the plain "B1" icon. It is the official
  Buriki One logo, kept in the repository like `setup_art.png` at the user's choice.
- **`build_from_rom.py`** (also runs on the dev machine with the tools on PATH):
  1. finds the 33 game files and 4 system files by size + CRC-32, whatever they're named
     (`brom1.bin` and `tmp87ph40an.bin` required; `bios_us.bin`, `bios_export.bin` optional);
  2. cuts the code images, makes the symbols, runs N64Recomp and `fix_generated.py`;
  3. compiles 60 files in parallel (generated code -O1, runtime -O2) and links `-static
     -mwindows` with icon and version details;
  4. runs the new exe headless for 300 frames as a boot test;
  5. moves the ROM data to `<dest>/roms` and copies the exe.
  Everything is written under `<dest>\_build` (removed afterwards); the source is only read.
- **Function lists** are now tracked in `tools/data/functions_game.txt` and `functions_bios.txt`
  (addresses only, from Ghidra), so a build needs neither Ghidra nor Java. `BURIKI_WORK` points
  the symbol tools at a scratch folder. `make_recomp_header.ps1` became `make_recomp_header.py`
  (same output).
- **Runtime changes for an installed game** (`main.cpp`, `hw.cpp`):
  - the ROM folder defaults to `roms` next to the exe, else one folder up (the source tree);
  - with no console, output goes to `buriki.log` next to the exe (previous run:
    `buriki.old.log`), and a fatal error or crash shows a message box.
- **Licences:** `LICENSE.md` (a draft: MIT for the project's own code, MAME-ported files keep
  LGPL-2.1+ / BSD-3-Clause, ReLayer and N64Recomp parts MIT) and `licenses/`.
- **Checked:**
  - Unzipped the package to a clean temp folder and ran Setup silently with PATH cut down to
    Windows' own folders: installed in ~10 s on this 28-thread PC.
  - The installed exe imports only Windows DLLs. Its frames at 600-3000 of the soak script are
    byte-identical to `frames_soak_ref`. A windowed widescreen run wrote `buriki.log`.
  - Wrong sets give plain messages (not Buriki One; system ROMs missing; files missing).
- **Not tested:** a normal interactive install (shortcuts, the Apps entry) and Uninstall, which
  need clicking through; and a slow or low-memory PC.

## Silber, the hidden twelfth fighter (2026-11-10): found; "Unlock Silber" in the menu

Read from the game's code and confirmed by running it. Addresses are for rev. B.

- **The select screen** (`func_800A85A0`) calls `func_800BE9E4(13)`; non-zero gives 12 selectable
  fighters instead of 11 (count at gp+0x27A8 = 0x800F3978; gp = 0x800F11D0). The twelfth slot,
  blank when locked, becomes "ORIGINAL KARATE": Silber, Germany, rated HARD. He plays as a normal
  player character (checked: selected by script, VS screen, a fight with his name on the bar).
- **The flags** are one 16-bit word in the save memory at nvram offset 0x20F0 (pointer at
  0x80144700 = 0xBF802000, +0xF0), cached at gp+0x27C0 with 0x4000 as the "loaded" marker.
  Bits 0-10: one per normal fighter. Bit 13: Silber unlocked.
  `func_800BE980(n)` sets bit n, and when bits 0-10 are all set it sets bit 13 as well.
  `func_800BEA18(n)` clears a bit. Unlocked = 0x27FF in the file.
- **Two ways the arcade sets them:**
  1. `func_8000D454` sets the bit of the fighter in 0x800F71E5 when gp+0x2847 is non-zero. This
     looks like "finished the game with that fighter", so clearing it with all eleven unlocks
     Silber. Read from the code only: not confirmed by playing through.
  2. A cheat code on the operator menu's COIN DATA > TOTAL COIN DATA screen (`func_800B5B74`,
     table of 12 steps at cart 0x1DBA5C): **Up, Up, Down, Down, Left, Right, Left, Right, Start,
     Start, R button, Start**. A wrong press starts it over. It sets all eleven bits, shows a
     blinking "SILBER" box and plays a sound. Confirmed by script: nvram 0x20F0 became 27 FF.
- Input word bits (0x800FF530 held / 0x800FF532 new presses): 1 up, 2 down, 4 left, 8 right,
  0x10 L button (button 1), 0x20 R button (button 2), 0x40 / 0x80 buttons 3 / 4, 0x4000 start.
- Other fighter variables: 0x80144691 / 0x80144693 are the two sides' fighters (the attract
  demo picks them with random % 11, so Silber never appears in demos); gp+0x27AA is player 1's
  select-screen cursor.
- **Diagnostics added for this** (`hw.cpp`): `BURIKI_RAMDUMP=dir:f1,f2` saves main RAM
  big-endian on those frames; `BURIKI_POKE=f0-f1:ADDR=VV;...` writes RAM bytes at the end of
  each frame in a range (experiments only). The script control name for the Test button is
  `test`.
- **Menu: Game > Unlock Silber** (`unlock_silber` in settings.ini, on by default, asked for by the
  user: "like the Fighting Layer option, unlocked from the start").
  - `hw_set_unlock_silber` (`hw.cpp`): with it on, reads of the flag word return bit 13 set, so
    the select screen offers twelve fighters. Nothing is written for it.
  - When the game writes the word back, bit 13 is stored only if it was already stored or the
    eleven fighter bits are complete. So the save memory holds what was really earned, and
    turning the option off goes back to that.
  - Switching it also fixes up the game's cached copy in RAM, so it applies from the next
    player select screen without a restart.
  - Headless runs leave it off unless given `--set unlock_silber=1`, so reference runs are the
    arcade's behaviour.
  - Checked: blank save + option on: the twelfth slot is "ORIGINAL KARATE", Silber plays a
    fight; the save file's flag word stayed 0000 over two runs and its play counters carried on
    (1 then 2), so the game doesn't treat the save as damaged. Headless frames still match
    `frames_soak_ref`. Not checked by hand: switching the option in the menu mid-session.

## Arcade options in the menu, clock, HUD at the edges (2026-11-10): done

### Arcade options (menu: Game > Free play, and the new Rules page)

- **Where they live** (found by changing each option in the Test menu by script and comparing
  the save memory; one byte each, no checksum, saved when the options page is left through EXIT):
  - coin settings: nvram 0x2030 + n. Free play = 0x2032 (0 / 1). Coin counter / credit display =
    0x2031, 0x2033; coins per credit = 0x2034 and 0x2037.
  - game options: nvram 0x2050 + n in the Test menu's order: DIFFICULTY (1-4), DEMO SOUND,
    COMMAND DISP., ATTACK POWER CPU (1-3), ATTACK POWER VS., PLAY TIME CPU (1-3), PLAY TIME VS.,
    REPLAY, CONTINUE, LANGUAGE (1 Japanese, 2 English), SECOND DISP (0-2), SECOND BTN DISP (0-2),
    1 CREDIT VS, BLOOD DISP. A second copy at 0x2060 looks like the factory defaults.
- **The game copies them into RAM at power on** and then reads only the copies: coin block at
  0x801446E0 + n, game options at 0x801446F0 + n. Changing only the nvram does nothing until
  the next boot; changing the RAM copy works at once (free play: the attract text switches to
  PRESS 1P OR 2P START / FREE PLAY and Start works without a coin).
- `hw_game_option_get / set` (`hw.cpp`) write both, so a change is kept and applies now. The menu
  rows read the game's own values (not settings.ini), so the Test menu and our menu always agree.
  Rows are greyed out until the game has set up its settings (`hw_game_options_ready`).
- **LANGUAGE is left out on purpose:** changing its RAM copy mid-run stopped the game (bad read
  at 08ED83B4 in `func_80060B00`). Region in our menu already covers language, with a restart.
- Left to the Test menu (F2): SECOND DISP, SECOND BTN DISP, 1 CREDIT VS, coin counts. I don't
  know what the first three do well enough to describe them to a player.
- The one-line descriptions on the Rules page come from the option names, and "applies from the
  next fight" is an assumption. Neither is confirmed by play.
- Checked: both pages by screenshot; free play by RAM poke. Not checked: changing an option from
  our menu by hand, and that each rule really changes the fight.

### Clock

- The RTC62423 (MSM6242-style) at 0x1F702100 now returns the PC's local time and date, read once
  per frame (`rtc_read` in `hw.cpp`). Register F reports 24-hour mode. Writes are ignored.
- Headless runs keep the old all-zero clock (`BURIKI_RTC=1` forces the real one, `=0` the zero
  one), so reference runs don't depend on the time of day.
- Checked: the Test menu's CALENDER SETTING screen showed 2026/11/10(TUE) 14:48:59, matching
  the PC.
- For netplay later: both sides would need the same clock value.

### Widescreen: HUD at the screen edges (Display > Widescreen display)

- The fight display is all sprites in the front priority groups (0-3). The arena's decorations
  are sprites in groups 6-7, behind the 3D.
- With the option on, `screen_update` draws the sprite buffer a second time for the wide front
  layer, with each front sprite moved by `hud_shift_for` (`video.cpp`): top band (ends above
  line 80) moves out unless its centre is in the clock's zone (x 192-320); bottom band (from
  line 360) moves by half; in lines 200-350 only what lies wholly left of x 208 or right of
  x 304 (the button guides). Anything across the middle stays (K.O., banners, the coin prompt).
- The shift is the width of the extra picture each side, in arcade pixels, from `hd3d.cpp`
  (`video_set_wide_layers(on, shift)`), and is 0 unless the previous frame went wide. So there is
  a one-frame lag when a scene turns wide or back.
- The normal 512x448 picture is untouched (20 frames compared with the option on and off).
- Checked in a scripted 16:9 fight. Not checked: two-player fights (player 2's guide), 21:9.

### Winner screen

- The dark bar behind the winner's name is inset about 32 pixels from each side in 4:3 as well,
  so its width in widescreen is the arcade's own design. Nothing to change. (An earlier note
  called it a widescreen shortcoming; that was wrong.)

### Sound: the delay ("echo") effect is used by the game but still not emulated

- `BURIKI_LOGFX=1` prints what the game asks of it. Over 6000 frames: voices are sent to the
  delay (register 7, destination 0xF) with send levels 0xFC-0xFE and the "e" nibble from 4 to
  0xC, and register A sets taps on voices 1-3 and 8-10 (feedback, dd = 3), 11 (address 0x2001,
  dd = 2), 12-16 (left) and 22-26 (right) with rising volumes, and 31.
- MAME's source documents the register fields but not how the chip combines them ("How does the
  delay effect work?", "delay effect parameters, unknown encoding"). Writing one from this alone
  would be invention. It needs a recording of a real board to match against, or documentation of
  the Akai S3000's effect section, which the chip comes from.

## Release test pass and review fixes (2026-11-10)

### What was run

- **Soak:** 30,050 frames headless with the soak script: no crash, NaN or stall; all 50 frames
  shared with `frames_soak_ref` byte-identical. Again after the fixes below (12,050 frames, 20
  frames identical).
- **The pause menu driven by `--test-keys`:** picture shape through to Widescreen, HUD to the
  screen edges, Unlock Silber off and on, Free play on, difficulty and attack power changed, then
  a game started with Start alone, Silber picked and a fight played. All took effect live.
- **Modes, each a 2,500-frame windowed run, exit 0:** regions Japan / USA / World; Original
  graphics with Widescreen (stays 4:3); Stretch; whole-number + scanlines; Smooth + widescreen +
  edge HUD; quality Performance and Extreme in widescreen; borderless full screen; the GDI
  fallback; sound on; vsync pacing. Missing ROM folder: plain message, exit 3.
- **Two players:** versus fight in widescreen with the HUD at the edges; both sides mirror.
- **An installed copy** (made with Setup, the 13:42 build, read-only look
  at its logs): 76,200 frames (21 minutes) at a steady 59 fps, no errors. So a click-through
  install with shortcuts and the Apps entry works. That copy predates Silber, the Rules page and
  the edge HUD.
- **Installer edge cases** (silent, temp folder, PATH cut down, never registering):
  - a folder with spaces, letters outside the code page and a trailing backslash: installs, and
    the game runs and logs from it;
  - a folder with other files in it (a `roms` folder): refused before anything is created;
  - reinstall over an install: settings kept;
  - `Uninstall.exe /silent`: game and ROM data removed, settings and save kept, another
    install's shortcut and Apps entry untouched;
  - uninstall from a folder that isn't an install: nothing touched;
  - a damaged zip: plain message, scratch folder removed.

### Fixed from an independent review of the session's code (a subagent, read-only)

- **Setup could delete someone else's `roms` folder.** Install and uninstall removed `<dest>\roms`
  and `_build` whole. Now Setup only installs into a folder that is new, empty, already this
  game's, or a stopped install's leftovers (`has_other_files` in `setup_win32.c`, `check_dest`
  in `build_from_rom.py`); only `roms\buriki` and `roms\hng64` are ever replaced or removed; the
  scratch folder carries a marker file (`_build\.setup-scratch`) and is only deleted with it;
  uninstall does nothing unless the game's exe is in its folder.
- **Folder names outside the PC's code page** broke the build (N64Recomp) and the game (narrow
  file functions). The game and the packaged N64Recomp.exe now carry a manifest with
  `activeCodePage` UTF-8 (Windows 10 1903+). On older Windows the error message suggests a
  plain-letter folder. The game's icon and manifest are copied next to its `.rc`.
- **The game's manifest also declares it DPI-aware**, so a display scaled to 125%+ no longer
  gets a stretched, blurry picture. Not tested on a scaled display (this PC is at 100%).
- **A trailing backslash on the install folder** broke the build's command line. Arguments are
  now quoted properly (`add_arg`), and the folder is trimmed and made absolute.
- **Unexpected problems in the build script** (game running during a reinstall, damaged or
  oddly packed zips, disk full, no write access, timeouts) now end in a plain message and
  remove the scratch folder. The game exe is checked for being in use before the ROM data is
  touched.
- **Closing Setup during a build** now removes the scratch folder (and the folder itself if
  nothing was installed there before). Not tested: it needs a click on a question box.
- **Uninstall:** running the copy named `Uninstall.exe` on its own uninstalls; `/silent` and
  `/purge` added; the Apps entry and shortcuts are only removed if they belong to that folder.
- **A second "stuck" message box** appeared over any fatal error (the watchdog firing while the
  first box was open). One error is reported at a time now (`hw_fatal_claim`).
- **The window's system menu held open for 3 s** stopped the game as "stuck". A 250 ms window
  timer keeps the activity counter moving in Windows' own modal loops. Not tested by hand.
- **`buriki.log`** is written unbuffered, so it is complete after a Task Manager kill; "Restart
  game" lets go of it first so the new copy can keep it as `buriki.old.log`; if it can't be
  opened the error box still shows. A player's log has 40 hardware-trace lines instead of 300,
  and the first `[perf]` line no longer prints garbage.
- **The HD renderer** no longer keeps stale sizes after a failed render-target allocation, and
  steps the quality down (8, 6, 4, 3, 2) when the card can't provide the target, remembering the
  limit.
- Setup scales itself down to fit small or heavily scaled desktops; one unterminated copy in
  the folder picker fixed; `*.bin` is gitignored and dumps / saves are never packaged.
- Left as is: the one-frame lag of the edge HUD when a scene turns wide or back (cosmetic).

### For netplay later: where a frame ends

`BURIKI_LOGVBL=1` now prints the game's stack pointer with the function it is in at each vblank.
Over 12,000 frames of the soak script, every frame of every fight ended in the idle loop
`func_800C5C20` with the same stack pointer (803FFFC0): 600 of 600 in each window from frame
2400 to 9600. Outside fights (boot, scene loads, select and result screens) a handful of
frames per 600 end elsewhere, at other depths. So during fights the game is always at the same
place in the same chain of calls when a frame ends, which is what a save state without copying
the host stack needs. Not yet tried: actually saving and restoring.

## Save states: the feasibility test for rollback netplay (2026-11-10)

**Result: saving and restoring the whole machine works, exactly. Replays are fast enough for one or
two frames of rollback today; the sound board is what limits more.**

### Why it works

The recompiled game is native code on the host's own stack, so a state can only be put back where
the host stack is the same as when it was taken. The game's main loop (`func_800080B0`) sets a
flag at gp+0x2C16 and spins at label `L_800080F0` until the vblank interrupt clears it. Every
frame of a fight ends there: one place in the code, one stack depth (stack pointer 803FFFC0),
with nothing live in host locals. At that point the machine is fully described by its emulated
state, so a copy of that state is a save state. No host stack is copied.

### What a state holds (`savestate.h`, `*_state` in each module), 23.8 MB

- `hw_state`: every writable memory region (main RAM 16 MB, video and sprite RAM, palette, the 3 MB
  framebuffer region, sound RAM 4 MB, save memory, dual-port RAM, system registers), the CPU's
  `recomp_context`, the TLB, interrupt, DMA and timing state.
- `sound_state`: the V53A and L7A1045 objects as raw bytes, the chip's 128 KB sample RAM, the
  latches and scheduler counters. Sound waiting to be played is dropped on a load.
- `io_board_state`: the TLCS-870 object as raw bytes and the board's port state.
- `input_state`: the control latches and coin pulses.
- Not saved: the 3D renderer's buffers and matrices and the 2D picture (rebuilt every frame,
  never read by the game), diagnostics counters, the audio device.
- The chip objects are copied raw because their callbacks and buffer pointers never change after
  start-up (`V53A::state_prepare` empties the one string that owns heap memory first). That is
  fine for rollback, where states never leave the process. A state sent to another PC or kept on
  disk would need field-by-field saving.

### The test (`savestate.cpp`, env `BURIKI_SAVETEST=N,K[,R][,novideo][,nohash][,loop][,nosound]`)

Save at the end of frame N, play K frames noting a fingerprint of main RAM each frame, put the
state back, play the same K frames again with the same scripted controls, and compare every frame
and the whole machine at the end. `loop` chains R such cycles; `novideo` skips drawing in replays.
A frame that doesn't end at the wait point can't be loaded at; such cycles are counted as skipped.

| Run (headless, soak script) | Result |
|---|---|
| `3000,300,2`: one fight, 300 frames, replayed twice | identical both times, all 300 frames |
| `2500,8,1500,loop`: 1,500 cycles of 8 frames, 12,000 frames of fights and scene changes | 1,498 identical, 0 different, 2 skipped |
| `2600,1800,1`: one replay of 1,800 frames, across a K.O. and the next fight starting | identical |
| `600,60,40,loop`: the attract mode, before any credit | 40 of 40 identical |
| `3000,1,600,loop`: save and replay every single frame for 600 frames | 600 of 600 identical |

### Speed (this PC, one process, 8-frame cycles, 400 of them)

| | |
|---|---|
| Save | 1.75 ms (worst 4.1) |
| Load | 1.27 ms (worst 3.4) |
| Replay one frame without drawing | 3.89 ms: sound board about 2.0, I/O board about 0.5, the game itself about 1.4 |
| Play one frame with the software picture | 5.28 ms |

A frame at 60 Hz has 16.7 ms. A frame that rolls back R frames costs about load 1.3 + R x 3.9 +
the frame itself 5.3 + a save 1.75. That fits for R = 1 (12.2 ms) and just about for R = 2
(16.2 ms), before the graphics card's share. Rollback netcode usually wants room for 4 to 8.

### What that means for netplay

- The hard unknown is settled: states are exact, cheap, and available on every frame of a fight.
- (Settled in the next section: the board can be left out of replays.)
- **The sound board has to get cheaper to replay, or be left out of replays.** It can't simply be
  left out: the game reads the sound CPU's replies (`func_800CC434` reads the latch at 0x68000004
  into gp+0x2C78), so its behaviour feeds back into the game's memory. Options, to be tried in
  this order:
  1. Speed up the V53 interpreter. It takes 2 ms for about 540,000 clocks a frame; there is
     likely a several-fold gain in it.
  2. Find out how much of the game's logic really depends on those replies. If only the sound
     driver's own variables do, the board could run outside the rollback.
- The software picture is not needed in a replay, and neither is the audio output.
- Saves could use dirty-page tracking later; at 1.75 ms it isn't the bottleneck.
- Both players need the same ROMs, region, arcade options, save-memory contents and clock. The
  clock must be fixed or agreed (it follows the PC's by default with a window).
- Not looked at yet: the networking itself, how players connect, input delay, and what happens
  on the frames that don't end at the wait point (scene changes: a rollback would have to wait
  for the next frame that does).

## Sound board and rollback: making replays cheap (2026-11-10)

Follows the save-state test above, which found a replayed frame cost 3.89 ms, most of it the
sound board. Two things were done: the board was made faster without changing a single bit of
what it does, and then it was shown that the game does not need it in a replay at all.

| Replay of one frame, not drawing (this PC, `3000,8,400,loop,novideo,nohash`) | |
|---|---|
| At the start | 3.89 ms (save 1.75, load 1.27) |
| Sound board faster, still replayed | 2.86 ms (save 1.30, load 1.29) |
| Sound board left out of replays (`nosound`) | **0.65 ms** (save 1.05, load 1.06, state 19.7 MB) |

A rollback of 8 frames now costs about 1.1 + 8 x 0.65 = 6.3 ms on top of the frame being played,
where it was 32 ms. That is the room rollback netcode wants.

### Part 1: exact speed-ups (every one checked: same RAM fingerprint at frame 6050, same WAV byte for byte, same reference frames)

- **Direct memory for the V53** (`set_fast_memory`, `mem_rb/rw/wb/ww`): its 2 MB of RAM no
  longer goes through a callback per byte. `BURIKI_V53_CALLBACKS=1` puts the old path back.
- **Polygons are not prepared in replays** (`command3d` returns early for packets 0x0100 to
  0x0102 when `savestate_skip_video()`): they carry no state from frame to frame.
- **Saves write into a buffer that is reused** (`StateIO::raw`): 1.75 to 1.3 ms.
- **The idle serial clock.** The V53's built-in serial port is never used by the game, but its
  baud-rate clock was ticking 8,428 times a frame, and every tick ended the CPU's run: 8,846 runs
  a frame for 95,000 instructions. A tick with the port idle only counts a divider
  (`Scu::clock_tick_is_idle`, `skip_idle_ticks`), so the ticks are now applied later, in one go,
  whenever something looks at the port (`brc_sync`, `brc_resched`). Runs a frame: 484.
- **What that broke, and why.** The audio changed slightly. Cause: a REP string instruction
  stops at the end of a run and carries on in the next one, and each stop costs a few extra
  clocks (the loop conditions were `m_icount>0`). MAME's timing has those 8,400 stops a frame,
  so they are part of "the same as MAME". Kept: the REP loops now stop where a tick would have
  ended the run (`m_soft_icount`, `soft_cut_update`, all 56 loop conditions), without the run
  really ending. Audio identical again.

The honest result of that last one: 8,400 fewer runs saved only 0.1 ms (1.91 to 1.82 ms a
frame). The cost is the interpreter itself, about 19 ns an instruction.

Tools added: `BURIKI_SNDPROF=1` (per-frame counts and times), `BURIKI_PROFILE=file` (a sampling
profiler in `main.cpp`; `BURIKI_PROFILE_REPLAY=1` samples the replays only) and
`tools/profile_report.py <exe> <file>`.

### Where a replayed frame's time went (profile of replays only, board still in)

| | |
|---|---|
| V53 sound CPU | 43% |
| I/O board microcontroller | 16% |
| L7A1045 sound chip | 15% |
| saving (once per 8 frames) | 17% |
| the game itself | **5%** |

Neither CPU is idling. A count of where they execute over 3,050 frames: the V53's busiest loop
(0x96F9 to 0x970E) is 13.5% of its instructions and the rest is spread over its sequencer;
the microcontroller's busiest (0xC71A to 0xC73B) is a third of its 45,000 instructions a frame.
So there is no idle loop to skip, and squeezing the interpreters would give tens of percent,
not multiples.

### Part 2: the game does not depend on the sound board in a fight

`BURIKI_SOUND_FREEZE=N` stops the board dead at frame N (no CPU, no chip, replies frozen).

- Frozen from frame 3000 and run to 6050 through the soak script's fights: **main RAM is
  byte-identical to the normal run** at all twelve frames compared (3004, 3100, 3288, 3500,
  3851, 3852, 4200, 4600, 5000, 5400, 5800, 6040) and the final fingerprint matches.
- In those frames the game read the board's reply latch (0x68000004) 141 times, about once per
  command it sent, and the reply was different every time in the normal run. It made no
  difference to anything the game kept.
- `BURIKI_SAVETEST=...,nosound`: the board is left out of the state and not run during
  replays; what the game sends it in a replay is dropped (it was sent the first time those
  frames played). `2500,8,1500,loop,nosound` with a fingerprint every frame: 1,498 of 1,500
  cycles identical, 0 different, 2 skipped, the same score as with the board in.

What the code says (`func_800CC434`, the game's sound-command manager): there *is* a handshake.
gp+0x2C80 holds a command waiting to be acknowledged (0xFFFF = none); the routine compares bit 1
of the reply word at 0xE8000006 with it, copies the reply at 0xE8000004 to gp+0x2C78, and
gp+0x2C82 counts down while waiting for the reply 0x55AA. In all the fight frames tested that
path is idle (the frozen run would have left different values in those variables otherwise).
It is most likely the start-up and sound-bank loading conversation. **That is the caveat:**
tested over 12,000 frames of fights and scene changes, not proven for every screen.

### What this means for netplay (replaces options 1 and 2 above)

- Design: the game, the I/O board and the controls are rolled back; **the sound board runs
  forward only**, like the picture. Replays cost 0.65 ms a frame.
- Still to build for that: when a replay sends the board different commands from the first
  time (the other player's attack arrived late, so its sound starts now), the new ones must be
  passed on and the already-sent ones not repeated. The test only drops everything, which is
  right when the replay is the same as the first pass.
- Safeguard for the caveat: both players' games compare a fingerprint of game memory every so
  often (standard in rollback netcode). If the handshake path ever matters during play it shows
  up as a mismatch in testing instead of a silent drift.
- If it does matter somewhere, the exact fallback is known: keep a log of what the game sent
  the board and when, and re-run the board from a snapshot only when a replay's log differs
  from the first pass. Costs the old 2.3 ms a frame, but only on those frames.
- Saves are now the larger part (1.05 ms each, 19.7 MB). Dirty-page tracking would cut that.

## Online play: rollback, a direct link, and the Online page (2026-11-10): done; untested between two PCs

Two players, a direct UDP link, rollback. Menu: Online. `netplay.cpp` (the session), `pagestate.cpp` (cheap
states), hooks in `hw.cpp`, `input.cpp`, `sound.cpp`. The command-line form:

    ReBuriki.exe --netplay-host 7600 [--netplay-delay 2]
    ReBuriki.exe --netplay-join ADDRESS[:PORT]

`tools\netplay_demo.bat` starts two linked copies of the play copy on one PC.

### How a session starts

Both copies run the whole game from power-on, so there is nothing to synchronise except what
the machine starts with. After the ROMs are loaded and before the game boots (`netplay_start`):

- The joiner says hello with a fingerprint of the cartridge and BIOS and the build name
  (`BUILD` in `netplay.cpp`: **change it whenever the emulation changes**). Different game
  files, region or version: turned away with a message.
- The host sends its input delay, its Unlock Silber setting, its clock, and its 16 KB save
  memory (so free play, difficulty and the rest are the host's). The joiner uses that copy for
  the session and never writes it to its own `nvram.bin`.
- The clock both games read is the host's local time at the start plus game time
  (`netplay_clock`, used by `rtc_read`).

Then the arcade's own flow does the rest: coin, start, "here comes a new challenger".

### Frames, controls, guesses

- `hw_events` calls `netplay_frame()` at every frame boundary, where the controls used to be
  read. A player's controls read at boundary F are for frame F + delay (default 2) and are sent
  at once; every packet repeats all the controls the other side has not acknowledged, so a lost
  packet costs nothing but time.
- Each player uses their own Player 1 keys or first controller; the host is player 1 in the
  game and the joiner player 2. No Service or Test button online.
- When the other player's controls for a frame are not here yet the game plays on with a guess
  (their last known controls), for at most 8 frames (`MAX_PRED`). When the real controls differ,
  the machine is put back to the state saved at that frame and the frames since are played again
  with nothing drawn (`savestate_skip_video`) and the sound board not run.
- A state can be saved when the frame ends with the game waiting in its main loop: not in an
  interrupt, stack pointer 0x803FFFC0 (`WAIT_SP`; `func_800080B0`'s own level, so the chain of
  calls above it is always the same). It can be put back at a boundary that ended at the same
  place in the code (`site`). That is nearly every frame from about frame 15 after power-on.
- A guess is only allowed while a state from no later than the first unconfirmed frame is being
  kept. Otherwise the game waits for the real controls. So start-up and scene changes run in
  step, and everything else runs ahead. A wrong guess found at a boundary that is not a wait
  point is undone at the next one that is.
- Each side works out from the frame numbers and the ping whether it is ahead of the other, and
  sleeps a few milliseconds a frame if so (`pace`). Not tuned yet: in the tests the host tends
  to be about half a frame ahead, which gives it slightly longer rollbacks.

### States that only copy what changed (`pagestate.cpp`)

A full state was 19.7 MB and took 1.4 ms to copy, every frame and again inside rollbacks: a
6-frame rollback took 14.7 ms. Now the large areas (main RAM, sprite, tilemap and framebuffer
memory) are allocated with Windows' write watch, each 4 KB page carries a stamp of when it last
changed, and bringing a saved copy up to date or putting one back only copies pages with a
newer stamp than the copy. In a fight that is about 275 KB.

| | full copies | changed pages only |
|---|---|---|
| Save | 1.45 ms | 0.14 ms |
| Longest rollback in the same test | 14.7 ms (6 frames) | 8.9 ms (6 frames) |
| Replayed frame, all in | 1.87 ms | 1.27 ms (0.87 ms in the windowed test) |

`BURIKI_NOWATCH=1` goes back to plain memory and full copies. Ordinary play is not affected:
same fingerprint (C307A015EBF7970F at frame 6050 of the soak script), and the save-state test
still passes with and without `nosound`.

### Sound in a rollback

The board runs forward only. What the game tells it is noted per frame (a command is the latch
at 0x68000000 when the game raises the command interrupt through 0x68000008; high word the
command, low word 0x0040). In frames played again, commands the board already had for those
frames are dropped; new ones are given to it when the rollback ends (`sound_inject_command`:
latch, interrupt, run the V53 until it acknowledges). A sound that played on a wrong guess is
not taken back. In a 4,000-frame match with 500 rollbacks: about 255 commands, 3 or 4 given
late, 0 or 1 played wrongly.

### Checking the two games against each other

Every 60 frames each side fingerprints main RAM in the state of a frame that is settled (all
controls before it real, no wrong guess outstanding) and sends it; the other compares. A
difference is logged as "OUT OF STEP". None has occurred.

### Tests (`tools\netplay_test.py`)

Two copies on this PC with separate scripts of random controls and a simulated connection
(`BURIKI_NET_SIM=delay,jitter,loss%` on what each copy sends), and a third, ordinary run given
both scripts merged and shifted by the delay. Pass = all three end with the same main RAM and
every comparison on the way matched. The scripts put in a coin each and play a two-player
"Special Match" from about frame 1900.

| Run | Rollbacks (host / joiner) | Deepest | Result |
|---|---|---|---|
| 3,000 frames, perfect link, as fast as possible | 9 / 11 | 6 | pass |
| 6,000 frames, 50 ms +20, 5% loss | 506 / 521 | 6 | pass, 99 of 99 comparisons |
| 6,000 frames, 120 ms +40, 10% loss, as fast as possible | 521 / 526 | 8 | pass, 98 of 98 |
| 5,500 frames, delay 0, 30 ms +10, 2% loss | 489 / 492 | 6 | pass, 91 of 91 |
| 5,500 frames, delay 4, one side 80 ms +60 and 15% loss, the other 20 ms | 92 / 416 | 5 | pass, 91 of 91 |
| 3,600 frames in two real windows (host's save memory, live clock), 40 ms +15, 3% loss | 243 / 252 | 4 | pass, 59 of 59; longest rollback 4.6 ms |
| 3,200 frames with the installer-built exe, 60 ms +20, 5% loss | 194 / 199 | 8 | pass, 53 of 53 |

### The Online page and the screens (2026-11-10)

- Menu page ONLINE (`menu.cpp`): Status, Host a game, Address to join (a typed line, new item type
  `IT_TEXT`; letters, digits, `.`, `:` and `-`; Ctrl+V pastes), Join a game, Input delay, This PC's
  address, Leave the online game. `settings.ini` gains `online_delay`, `online_port` (7600, no menu
  row) and `online_address`.
- Hosting, joining and leaving restart the program with the online options changed
  (`fe_relaunch`): the same mechanism as Restart game, since both machines must start from power on.
- Screens (`ui_dialog`, `ui_notice`; `fe_dialog_frame`, `fe_stall_frame` in `fe_window.cpp`):
  a waiting screen for the host with its address (Esc cancels), a joining screen, "could not
  start" with the reason, "Waiting for the other player" at the top of the picture when the game has
  waited more than 0.4 s (the pause menu still works then), and "The online game has ended" when the
  other player leaves or nothing has arrived for 10 s. Each ends by going back to ordinary play.
- While online the rows that change how the game behaves are switched off (Unlock Silber, free play,
  region, the Rules page, the operator menu, Restart game), and `hw_game_option_set` does nothing.
  The pause badge reads "PAUSED FOR BOTH PLAYERS". The frame-rate counter also shows the ping.
- Tested with simulated key presses (`--test-keys`, `--shot-every` now also saves `menu_`, `dialog_`
  and `stall_` pictures): menu to Host to a joined game; the joiner leaving; the joiner killed
  (notice, then "connection lost", then back to ordinary play); typing an address and a join with
  no answer; Esc on the waiting screen.

### Not done yet

- **The internet.** Direct UDP needs the host's port (7600) reachable: a forwarded port on
  their router, or a VPN-style tool. No hole punching, no lobby. Windows asks once whether to
  allow the game through the firewall.
- **Both players need the same region** (BIOS) chosen before starting; a mismatch is refused,
  not fixed.
- **Only tested on one PC.** Real connections, two different PCs and CPUs (the game's own
  floating-point maths runs on the host CPU: basic operations only, which should give the same
  results everywhere, and the memory comparison would show it if not).
- Speed: the I/O board's microcontroller is now the largest part of a replayed frame.
