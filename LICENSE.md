# Licences

This project contains no game data and nothing made from the game. Buriki One and Hyper Neo Geo 64 are
trademarks of their owners; this is an unofficial fan project.

## This project's own code

Everything not listed below: MIT License.

    Copyright (c) 2026 The ReBuriki project

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

## Code ported from MAME

These files are ports of parts of MAME (https://www.mamedev.org/) and keep MAME's licence for the part
they come from. Each file's header names its source and copyright holders.

| Files (under `recomp/runtime/`) | From MAME | Licence |
| --- | --- | --- |
| `video.cpp`, `video3d.cpp`, `hw.cpp`, `sound.cpp`, `io_board.cpp`, `input.cpp`, `hd3d.cpp` (the parts that follow the hardware) | `src/mame/snk/hng64*.cpp`, `hng64*.ipp` (David Haywood, Angelo Salese, ElSemi, Andrew Gardner, Andrew Zaferakis) | LGPL-2.1-or-later: `licenses/LGPL-2.1.txt` |
| `video3d.cpp` (triangle setup and clipping) | `src/devices/video/poly.h` (Ville Linde, Aaron Giles) | BSD-3-Clause: `licenses/BSD-3-Clause.txt` |
| `l7a1045.cpp`, `l7a1045.h` | `src/devices/sound/l7a1045_l6028_dsp_a.cpp` (R. Belmont, O. Galibert) | BSD-3-Clause |
| `audiocpu/*` | `src/devices/cpu/nec/*` (Bryan McPhail, Patrick Mackinlay) and `src/devices/machine/` pic8259, pit8253, am9517a (see each file) | BSD-3-Clause |
| `iomcu/*` | `src/devices/cpu/tlcs870/*` (David Haywood) | BSD-3-Clause |

The game is always built from this source on the player's own PC, so the source the LGPL asks for is the
source in hand.

## Other parts

| Part | Licence |
| --- | --- |
| `recomp/runtime/frontend/*` and `tools/install/setup_win32.c`: adapted from ReLayer, Copyright (c) 2026 The ReLayer project | MIT: `licenses/ReLayer-MIT.txt` |
| `recomp.h` (made at build time from N64Recomp's header) and the N64Recomp translator the installer runs, Copyright (c) 2024 Wiseguy | MIT: `licenses/N64Recomp-MIT.txt` |
