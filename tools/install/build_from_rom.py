#!/usr/bin/env python3
"""
Build a playable Buriki One from the player's own ROMs: the step Setup runs on their PC.

    python tools/install/build_from_rom.py --rom buriki.zip --bios hng64.zip --dest "D:/Games/ReBuriki"

1. check the ROM sets by checksum and unpack what the game reads   (any file names, any MAME version)
2. cut the game's code out of the program ROMs
3. translate it to C                        (N64Recomp, then tools/fix_generated.py)
4. compile and link                         (64-bit clang++, in parallel)
5. start the new game once without a window to make sure it boots
6. copy the game and its ROM data to --dest  (settings, saves and controls already there are kept)

Nothing made from the ROMs is ever shipped: this script makes it on the player's machine. Lines starting
with "@@" are for Setup: "@@progress <0-100> <text>", "@@error <text>", "@@done <path to exe>".

Everything is written under <dest>/_build (removed afterwards) and <dest>; the source folder is only read.
"""
import argparse
import concurrent.futures
import ctypes
import os
import shutil
import struct
import subprocess
import sys
import time
import traceback
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import product  # noqa: E402

NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)   # no console windows flashing up under Setup
LIBS = ["user32", "gdi32", "winmm", "d3d11", "dxgi", "ole32"]
RUNTIME_SUBDIRS = ["iomcu", "audiocpu", "frontend"]      # I/O board MCU, sound CPU, PC front end

# What the game reads at run time: (folder, file name, size, CRC-32). Buriki One rev. B (MAME "buriki") and
# the Hyper Neo Geo 64 system ROMs (MAME "hng64"). Files are found by size and checksum, whatever their names.
GAME_ROMS = [
    ("007pr01b.81", 0x400000, 0xA31202F5), ("007pr02b.82", 0x400000, 0xA563FED6),
    ("007pr03a.83", 0x400000, 0xDA5F6105), ("007pr04a.84", 0x400000, 0xBEFC7BCE),
    ("007pr05a.85", 0x400000, 0x013E28BC), ("007pr06a.86", 0x400000, 0x0620FCCC),
    ("007sc01a.97", 0x800000, 0x4E8300DB), ("007sc02a.99", 0x800000, 0xD5855944),
    ("007sc03a.101", 0x800000, 0xFF45C9B5), ("007sc04a.103", 0x800000, 0xE4CB59E9),
    ("007sc05a.98", 0x400000, 0x27F848C1), ("007sc06a.100", 0x400000, 0xC39E9B4C),
    ("007sc07a.102", 0x400000, 0x753E7E3D), ("007sc08a.104", 0x400000, 0xB605928E),
    ("007sp01a.105", 0x800000, 0x160ACAE6), ("007sp02a.109", 0x800000, 0x1A55331D),
    ("007sp03a.113", 0x800000, 0x3F308444), ("007sp04a.117", 0x800000, 0x6B81AA51),
    ("007sp05a.106", 0x400000, 0x32D2FA41), ("007sp06a.110", 0x400000, 0xB6F8D7F3),
    ("007sp07a.114", 0x400000, 0x5CAA1CC9), ("007sp08a.118", 0x400000, 0x7A158C67),
    ("007tx01a.1", 0x400000, 0xA7774075), ("007tx02a.2", 0x400000, 0xBC05D5FD),
    ("007tx03a.3", 0x400000, 0xDA9484FB), ("007tx04a.4", 0x400000, 0x02AA3F46),
    ("007vt01a.17", 0x400000, 0xF78A0376), ("007vt02a.18", 0x400000, 0xF365F608),
    ("007vt03a.19", 0x400000, 0xBA05654D),
    ("007sd01a.77", 0x400000, 0x1AFB48C6), ("007sd02a.78", 0x400000, 0xC65F1DD5),
    ("007sd03a.79", 0x400000, 0x356F25C8), ("007sd04a.80", 0x400000, 0xDABFBBAD),
]
SYSTEM_ROMS = [   # (name, size, crc, required)
    ("brom1.bin", 0x80000, 0xA30DD3DE, True),          # BIOS, Japan: the game's code is matched to this one
    ("tmp87ph40an.bin", 0x8000, 0xB70DF21F, True),     # the I/O board's microcontroller (controls)
    ("bios_us.bin", 0x80000, 0xAB5948D6, False),       # BIOS, USA: English (the default region)
    ("bios_export.bin", 0x80000, 0xBBF07EC6, False),   # BIOS, export: English
]


def say(*parts):
    print(*parts, flush=True)


def progress(pct, text):
    say(f"@@progress {int(pct)} {text}")


_cleanup = []     # the scratch folder, removed if the build stops (unless --keep-work)
SCRATCH = "_build"
SCRATCH_MARK = ".setup-scratch"     # in <dest>/_build: Setup made this folder, so Setup may delete it


def fail(text):
    say(f"@@error {text}")
    for d in _cleanup:
        shutil.rmtree(d, ignore_errors=True)
    sys.exit(1)


def plain_path_hint(path):
    """Older Windows versions can't give the build tools a folder name with letters outside the PC's own set."""
    if path.isascii():
        return ""
    return " Try an install folder whose path has only plain English letters, such as C:\\Games\\" + product.PRODUCT + "."


def run(cmd, cwd, what, env=None, hint=""):
    """Run a step, echoing its output into the log; stop with a readable error if it fails."""
    say("> " + " ".join(os.path.basename(c) if i == 0 else c for i, c in enumerate(cmd)))
    p = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       encoding="utf-8", errors="replace", creationflags=NO_WINDOW)
    if p.stdout:
        say(p.stdout.rstrip())
    if p.returncode != 0:
        fail(f"{what} failed (see setup.log).{hint}")
    return p.stdout or ""


def total_ram_gb():
    class MS(ctypes.Structure):
        _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                    ("avail", ctypes.c_ulonglong), ("pf_total", ctypes.c_ulonglong), ("pf_avail", ctypes.c_ulonglong),
                    ("v_total", ctypes.c_ulonglong), ("v_avail", ctypes.c_ulonglong), ("ext", ctypes.c_ulonglong)]
    try:
        m = MS()
        m.len = ctypes.sizeof(MS)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.total / 2**30
    except Exception:
        return 8.0


def find_tool(folder, name, what):
    """<folder>/name or <folder>/bin/name; without a folder, look on PATH (developer machine)."""
    if folder:
        for p in (os.path.join(folder, name), os.path.join(folder, "bin", name)):
            if os.path.isfile(p):
                return p
        return None
    return shutil.which(name)


# ------------------------------------------------------------------ 1. the ROM sets
def open_sets(paths):
    """All files in the given zips, by (size, crc): MAME names differ between versions, checksums don't."""
    found = {}
    zips = []
    for path in paths:
        if not path:
            continue
        try:
            z = zipfile.ZipFile(path)
        except (OSError, zipfile.BadZipFile):
            fail(f"{os.path.basename(path)} can't be read as a zip file. Is the download complete?")
        zips.append(z)
        for info in z.infolist():
            found.setdefault((info.file_size, info.CRC), (z, info))
    return found, zips


def check_sets(found, game_zip, bios_zip):
    missing = [n for n, size, crc in GAME_ROMS if (size, crc) not in found]
    if missing:
        if len(missing) == len(GAME_ROMS):
            fail(f"{os.path.basename(game_zip)} is not Buriki One. Choose the MAME set buriki.zip "
                 "(Buriki One: World Grapple Tournament '99 in Tokyo, rev. B).")
        fail(f"{os.path.basename(game_zip)} is not the Buriki One set this needs: {len(missing)} of its "
             f"{len(GAME_ROMS)} files are missing or different (the first is {missing[0]}). "
             "It needs MAME's buriki.zip (rev. B), complete.")
    missing = [n for n, size, crc, req in SYSTEM_ROMS if req and (size, crc) not in found]
    if missing:
        if not bios_zip:
            fail("The Hyper Neo Geo 64 system ROMs are needed too. Choose the MAME set hng64.zip.")
        fail(f"{os.path.basename(bios_zip)} is missing {' and '.join(missing)}. It needs MAME's hng64.zip from a "
             "recent MAME version (one that includes the I/O board file tmp87ph40an.bin).")
    optional = [n for n, size, crc, req in SYSTEM_ROMS if not req and (size, crc) not in found]
    for n in optional:
        say(f"note: {n} is not in the system ROM set; that region will fall back to Japan")


def unpack(found, folder, name, size, crc):
    z, info = found[(size, crc)]
    os.makedirs(folder, exist_ok=True)
    out = os.path.join(folder, name)
    check = 0
    try:
        with z.open(info) as src, open(out, "wb") as dst:
            while True:
                block = src.read(1 << 20)
                if not block:
                    break
                check = zlib.crc32(block, check)
                dst.write(block)
    except (zipfile.BadZipFile, zlib.error, EOFError):
        check = None                                   # damaged data inside the zip
    except (NotImplementedError, RuntimeError) as e:   # a compression method or encryption Python can't read
        say(f"{name}: {e}")
        fail(f"{name} is packed in the zip in a way Setup can't read. Unpack the set and zip it again normally.")
    if check != crc:
        fail(f"{name} did not unpack correctly (the zip file may be damaged).")
    return out


def unpack_sets(found, roms):
    todo = [("buriki", n, s, c) for n, s, c in GAME_ROMS] + \
           [("hng64", n, s, c) for n, s, c, _ in SYSTEM_ROMS if (s, c) in found]
    total = sum(t[2] for t in todo)
    done = 0
    for folder, name, size, crc in todo:
        unpack(found, os.path.join(roms, folder), name, size, crc)
        done += size
        progress(4 + 14 * done / total, f"Unpacking your ROMs ({done >> 20} of {total >> 20} MB)")


# ------------------------------------------------------------------ 2. the code images
def make_images(roms, work):
    """The game's code: program ROMs 1 + 2 interleaved (16 bits each), big-endian, 0x100000-0x1FFFFF."""
    lo = open(os.path.join(roms, "buriki", "007pr01b.81"), "rb").read()
    hi = open(os.path.join(roms, "buriki", "007pr02b.82"), "rb").read()
    il = bytearray(len(lo) * 2)
    il[0::4], il[1::4], il[2::4], il[3::4] = lo[0::2], lo[1::2], hi[0::2], hi[1::2]
    n = len(il) // 4
    be = struct.pack(">%dI" % n, *struct.unpack("<%dI" % n, il))
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(work, "buriki_code_80000000.bin"), "wb") as f:
        f.write(be[0x100000:0x200000])
    shutil.copyfile(os.path.join(roms, "hng64", "brom1.bin"), os.path.join(work, "bios_9fc00000.bin"))


# ------------------------------------------------------------------ 4. compile + link
def compile_all(build, clangxx, jobs):
    rt = os.path.join(SRC, "recomp", "runtime")
    gen = os.path.join(build, "recomp", "generated")
    hdr = os.path.join(build, "recomp", "gen_runtime")           # recomp.h, func_table.cpp
    odir = os.path.join(build, "obj")
    os.makedirs(odir, exist_ok=True)
    inc = ["-I", rt, "-I", hdr, "-I", gen]
    tasks = []
    for f in sorted(os.listdir(gen)):
        if f.startswith("funcs_") and f.endswith(".c"):
            tasks.append((os.path.join(gen, f), "gen_" + f, "-O1"))
    tasks.append((os.path.join(hdr, "func_table.cpp"), "func_table.cpp", "-O2"))
    for sub in [""] + RUNTIME_SUBDIRS:
        d = os.path.join(rt, sub)
        for f in sorted(os.listdir(d)):
            if f.endswith(".cpp") and f != "func_table.cpp":
                tasks.append((os.path.join(d, f), (sub + "_" if sub else "rt_") + f, "-O2"))
    tasks.sort(key=lambda t: -os.path.getsize(t[0]))             # biggest first, so the slowest don't finish last

    def one(task):
        src, name, opt = task
        obj = os.path.join(odir, name + ".o")
        cmd = [clangxx, "-x", "c++", "-std=c++20", opt, "-w"] + inc + ["-c", src, "-o", obj]
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                           creationflags=NO_WINDOW)
        return src, obj, p.returncode, p.stdout

    objs, done = [], 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = [ex.submit(one, t) for t in tasks]
        for f in concurrent.futures.as_completed(futs):
            src, obj, rc, out = f.result()
            done += 1
            if out.strip():
                say(out.rstrip())
            if rc != 0:
                fail(f"Compiling {os.path.basename(src)} failed (see setup.log).")
            objs.append(obj)
            progress(34 + 56 * done / len(tasks), f"Compiling the game ({done} of {len(tasks)})")
    return objs


# The manifest makes Windows (10 version 1903 and later) give the game its file paths as UTF-8, so it
# runs from folders whose names have characters outside the PC's own code page (a user name in another
# alphabet, for example). The game's code uses the ordinary narrow file functions throughout.
# It also declares the game DPI-aware: on a display scaled to 125% or more, Windows would otherwise
# draw the game at a lower resolution and stretch it, blurring the picture.
GAME_MANIFEST = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
    </windowsSettings>
  </application>
</assembly>
"""

GAME_RC = """%(icon)s
1 24 "game.manifest"
1 VERSIONINFO
FILEVERSION %(n)s
PRODUCTVERSION %(n)s
FILEOS 0x40004
FILETYPE 0x1
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "CompanyName", "%(publisher)s"
      VALUE "FileDescription", "%(description)s"
      VALUE "FileVersion", "%(v)s"
      VALUE "InternalName", "%(product)s"
      VALUE "LegalCopyright", "%(copyright)s"
      VALUE "OriginalFilename", "%(exe)s"
      VALUE "ProductName", "%(product)s"
      VALUE "ProductVersion", "%(v)s"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"""


def resource_object(build, clangxx):
    """The game's icon (tools/install/game.ico, if make_package.py made one) and version details."""
    windres = os.path.join(os.path.dirname(clangxx), "llvm-windres.exe")
    if not os.path.isfile(windres):
        return []
    ico = os.path.join(HERE, "game.ico")
    have_icon = os.path.isfile(ico)
    if have_icon:
        shutil.copyfile(ico, os.path.join(build, "game.ico"))
    with open(os.path.join(build, "game.manifest"), "w", encoding="ascii") as f:
        f.write(GAME_MANIFEST)
    v = product.VERSION
    rc = os.path.join(build, "game.rc")
    with open(rc, "w", encoding="ascii") as f:
        f.write(GAME_RC % {
            "icon": '1 ICON "game.ico"' if have_icon else "",
            "n": ",".join((v.split(".") + ["0", "0", "0"])[:4]), "v": v, "publisher": product.PUBLISHER,
            "description": product.DESCRIPTION, "product": product.PRODUCT, "copyright": product.COPYRIGHT,
            "exe": product.EXE})
    obj = os.path.join(build, "game_res.o")
    p = subprocess.run([windres, "--no-preprocess", "--target=pe-x86-64", "game.rc", "-O", "coff", "-o", "game_res.o"],
                       cwd=build, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                       creationflags=NO_WINDOW)
    if p.returncode != 0:
        say("icon and version details skipped: " + p.stdout.strip())
        return []
    return [obj]


def link(build, clangxx, objs, exe):
    objs = objs + resource_object(build, clangxx)
    rsp = os.path.join(build, "link.rsp")
    with open(rsp, "w") as f:
        for o in objs:
            f.write('"' + o.replace("\\", "/") + '"\n')
    # -static: the C++ runtime goes into the exe (no DLLs to ship); -mwindows: no console window
    run([clangxx, "-O2", "-static", "-mwindows", "@" + rsp, "-o", exe] + ["-l" + l for l in LIBS], build,
        "Linking the game")


def check_dest(dest):
    """Setup deletes and replaces things inside its install folder (roms/buriki, roms/hng64, _build), so it only
    installs into a folder that is new, empty, already holds this game, or holds only what a stopped install
    left. Anything else (someone's MAME folder, say) is refused."""
    if not os.path.isdir(dest) or os.path.isfile(os.path.join(dest, product.EXE)):
        return
    ours = {"setup.log"}
    if os.path.isfile(os.path.join(dest, SCRATCH, SCRATCH_MARK)):
        ours.add(SCRATCH.lower())
    others = [n for n in os.listdir(dest) if n.lower() not in ours]
    if others:
        say(f"the install folder has other files in it: {others[:8]}")
        fail(f"{dest} already has other files in it. Choose an empty or new folder for the game, for example "
             f"{os.path.join(dest, product.PRODUCT)}")


def install(build, roms, exe_tmp, dest):
    exe = os.path.join(dest, product.EXE)
    if os.path.exists(exe):                       # before touching anything: can the old game be replaced?
        try:
            open(exe, "ab").close()
        except OSError:
            fail(f"{product.PRODUCT} is running. Close the game, then press Try again.")
    for sub in ("buriki", "hng64"):               # the ROM data: only these two folders are ever replaced
        target = os.path.join(dest, "roms", sub)
        if os.path.isdir(target):
            try:
                shutil.rmtree(target)
            except OSError as e:
                say(f"{target}: {e}")
                fail(f"Setup couldn't replace the old ROM data in {target}. Close anything that has that folder "
                     "open, then press Try again.")
        if os.path.isdir(os.path.join(roms, sub)):
            os.makedirs(os.path.join(dest, "roms"), exist_ok=True)
            shutil.move(os.path.join(roms, sub), target)
    shutil.copyfile(exe_tmp, exe)
    return exe


# ------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", required=True, help="the MAME buriki.zip")
    ap.add_argument("--bios", help="the MAME hng64.zip (system ROMs); not needed if --rom holds them too")
    ap.add_argument("--dest", required=True, help="folder to install the game into")
    ap.add_argument("--clang", help="folder with x86_64-w64-mingw32-clang++.exe (default: PATH)")
    ap.add_argument("--n64recomp", help="folder with N64Recomp.exe and its recomp.h "
                                        "(default: third_party/N64Recomp in the source tree)")
    ap.add_argument("--jobs", type=int, default=0, help="parallel compiles (default: by CPU count and memory)")
    ap.add_argument("--keep-work", action="store_true", help="keep the intermediate files in <dest>/_build")
    a = ap.parse_args()

    t0 = time.time()
    rom = os.path.abspath(a.rom)
    bios = os.path.abspath(a.bios) if a.bios else None
    dest = os.path.abspath(a.dest)
    clangxx = find_tool(a.clang, "x86_64-w64-mingw32-clang++.exe", "the compiler")
    if a.n64recomp:
        recomp_exe = find_tool(a.n64recomp, "N64Recomp.exe", "the translator")
        recomp_h = os.path.join(a.n64recomp, "recomp.h")
    else:
        recomp_exe = os.path.join(SRC, "third_party", "N64Recomp", "build", "N64Recomp.exe")
        recomp_h = os.path.join(SRC, "third_party", "N64Recomp", "include", "recomp.h")
    if not clangxx or not recomp_exe or not os.path.isfile(recomp_exe) or not os.path.isfile(recomp_h):
        fail("Setup's files folder is incomplete (the compiler or the translator is missing). "
             "Unzip the whole Setup download again.")
    jobs = a.jobs or max(1, min(os.cpu_count() or 2, int(total_ram_gb() / 1.5)))   # up to ~1 GB per compile
    build = os.path.join(dest, SCRATCH)
    say(f"{product.PRODUCT} {product.VERSION} build: rom={rom} bios={bios} dest={dest} clang={clangxx} "
        f"jobs={jobs} python={sys.executable}")

    progress(1, "Preparing")
    check_dest(dest)
    if os.path.isdir(build):
        shutil.rmtree(build)
    rdir = os.path.join(build, "recomp")
    work = os.path.join(build, "work")
    roms = os.path.join(build, "roms")
    hdr = os.path.join(rdir, "gen_runtime")
    os.makedirs(hdr)
    os.makedirs(work)
    open(os.path.join(build, SCRATCH_MARK), "w").close()
    if not a.keep_work:
        _cleanup.append(build)
    shutil.copyfile(os.path.join(SRC, "recomp", "buriki.toml"), os.path.join(rdir, "buriki.toml"))

    progress(2, "Checking your ROMs")
    found, zips = open_sets([rom, bios])
    check_sets(found, rom, bios)
    unpack_sets(found, roms)
    for z in zips:
        z.close()

    progress(19, "Reading the game's code")
    make_images(roms, work)
    tools = os.path.join(SRC, "tools")
    py = [sys.executable]
    env = dict(os.environ, BURIKI_WORK=work, PYTHONPATH=tools)
    # the compiler's folder first on PATH: N64Recomp.exe needs its C++ runtime DLLs from there
    env["PATH"] = os.path.dirname(clangxx) + os.pathsep + os.path.dirname(recomp_exe) + os.pathsep + env.get("PATH", "")
    run(py + [os.path.join(tools, "make_symbols.py"), "buriki.syms.toml"], rdir, "Listing the game's functions", env)
    run(py + [os.path.join(tools, "find_kernel_funcs.py"), "buriki.syms.toml", "kernel_funcs.txt"], rdir,
        "Checking the game's low-level functions", env)
    run(py + [os.path.join(tools, "make_func_table.py"), "buriki.syms.toml", os.path.join(hdr, "func_table.cpp")],
        rdir, "Listing the game's functions", env)
    run(py + [os.path.join(tools, "make_recomp_header.py"), recomp_h,
              os.path.join(SRC, "recomp", "runtime", "recomp_mem_section.txt"), os.path.join(hdr, "recomp.h")],
        rdir, "Preparing the translator", env)

    progress(22, "Translating the game's code to C")
    out = run([recomp_exe, "buriki.toml"], rdir, "Translating the game", env, hint=plain_path_hint(dest))
    if "Error" in out:
        fail("Translating the game failed (see setup.log)." + plain_path_hint(dest))
    run(py + [os.path.join(tools, "fix_generated.py"), "generated"], rdir, "Tidying the translated code", env)

    progress(34, "Compiling the game")
    objs = compile_all(build, clangxx, jobs)

    progress(91, "Linking")
    exe_tmp = os.path.join(build, product.EXE)
    link(build, clangxx, objs, exe_tmp)

    progress(94, "Testing the new game")
    p = subprocess.run([exe_tmp, "--headless", "--no-sound", "--frames", "300", "--roms", roms], cwd=build,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                       creationflags=NO_WINDOW, timeout=300)
    say("\n".join((p.stdout or "").splitlines()[-12:]))
    if p.returncode != 0:
        fail("The game was built but did not start correctly (see setup.log)." + plain_path_hint(dest))

    progress(96, "Installing")
    exe = install(build, roms, exe_tmp, dest)
    if not a.keep_work:
        shutil.rmtree(build, ignore_errors=True)
    say(f"built in {time.time() - t0:.0f} s")
    progress(100, "Done")
    say(f"@@done {exe}")


if __name__ == "__main__":
    try:
        main()
    except subprocess.TimeoutExpired:
        fail("A step took far too long and was stopped (see setup.log).")
    except OSError as e:                         # anything not handled where it happened
        say(traceback.format_exc())
        if e.errno == 28 or getattr(e, "winerror", 0) in (39, 112):
            fail("The disk is full. Setup needs about 500 MB free in the install folder.")
        if isinstance(e, PermissionError):
            fail("Setup isn't allowed to write to the install folder. Choose a folder of your own, such as one "
                 "in Documents or on another drive.")
        fail(f"Setup couldn't write its files: {e.strerror or e} (see setup.log).")
    except Exception:
        say(traceback.format_exc())
        fail("Setup ran into a problem it didn't expect (see setup.log).")
