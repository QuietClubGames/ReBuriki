#!/usr/bin/env python3
"""
Make the Setup download (developer machine only):

    python tools/install/make_package.py     -> dist/<Product>-Setup/ and dist/<Product>-Setup.zip

The package holds Setup.exe, a trimmed Python, a trimmed 64-bit clang++, the N64Recomp translator and this
project's source (tracked files only). It holds NO ROM data and nothing made from the ROMs: Setup builds the
game from the player's own buriki.zip and hng64.zip. Needs the developer toolchain on PATH
(x86_64-w64-mingw32-clang++, llvm-mingw), N64Recomp built in third_party/N64Recomp/build, and Pillow (icon).

setup_art.png (or .jpg / .webp) in the repo root, if present, becomes the picture down the right-hand side of
the Setup window (files/setup_art.png); icon_art.png, if present, becomes the icon of Setup.exe and of the game.
Neither is kept in the repository (.gitignore): official artwork belongs to
the game's owners, so whether a public download carries one is a decision for whoever publishes it.

Adapted from ReLayer's tools/install/make_package.py (Copyright (c) 2026 The ReLayer project, MIT License).
The names (product, exe, version, publisher) are in product.py.
"""
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import product  # noqa: E402

DIST = os.path.join(ROOT, "dist")
NAME = product.PRODUCT + "-Setup"
OUT = os.path.join(DIST, NAME)
FILES = os.path.join(OUT, "files")
N64RECOMP = os.path.join(ROOT, "third_party", "N64Recomp")

# what the build needs from this repo (tracked or new files, never ignored ones)
SOURCE_PREFIXES = ("recomp/buriki.toml", "recomp/runtime/", "tools/data/", "tools/extra_functions.txt",
                   "tools/sections.py", "tools/make_symbols.py", "tools/find_kernel_funcs.py",
                   "tools/make_func_table.py", "tools/make_recomp_header.py", "tools/fix_generated.py",
                   "tools/install/build_from_rom.py", "tools/install/product.py", "LICENSE.md", "licenses/")
PY_LIB_SKIP = {"test", "idlelib", "tkinter", "turtledemo", "ensurepip", "site-packages", "__pycache__", "lib2to3",
               "pydoc_data", "venv", "unittest", "sqlite3", "curses", "msilib", "distutils"}
PY_DLL_SKIP = ("_tkinter", "tcl", "tk", "_test", "_ctypes_test", "xxlimited", "sqlite3", "_sqlite3", "libcrypto",
               "libssl", "_ssl", "_hashlib")
INCLUDE_SKIP = {"ddk", "GL", "KHR", "gdiplus", "mach-o", "wrl"}


def say(*a):
    print(*a, flush=True)


def toolchain_root():
    clang = shutil.which("x86_64-w64-mingw32-clang++")
    if not clang:
        sys.exit("x86_64-w64-mingw32-clang++ is not on PATH")
    return os.path.dirname(os.path.dirname(os.path.realpath(clang)))


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def copy_tree(src, dst, skip_dirs=(), skip_file=lambda n: False):
    for d, dirs, files in os.walk(src):
        dirs[:] = [x for x in dirs if x not in skip_dirs]
        for f in files:
            if not skip_file(f):
                copy(os.path.join(d, f), os.path.join(dst, os.path.relpath(os.path.join(d, f), src)))


ICON_SIZES = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]


def make_logo_icon(src, ico):
    """The icon from a logo picture (icon_art.* in the repo root): on a black rounded tile, with a thin light
    outline so the logo's dark parts still show against the black."""
    from PIL import Image, ImageDraw, ImageFilter
    big = 1024
    logo = Image.open(src).convert("RGBA")
    box = logo.getchannel("A").getbbox()             # trim empty margins
    if box:
        logo = logo.crop(box)
    k = min(big * 0.94 / logo.width, big * 0.94 / logo.height)
    logo = logo.resize((round(logo.width * k), round(logo.height * k)), Image.LANCZOS)
    tile = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(tile).rounded_rectangle((0, 0, big - 1, big - 1), radius=176, fill=(0, 0, 0, 255))
    pos = ((big - logo.width) // 2, (big - logo.height) // 2)
    grown = logo.getchannel("A").filter(ImageFilter.MaxFilter(11)).filter(ImageFilter.GaussianBlur(2))
    outline = Image.new("RGBA", logo.size, (0xD8, 0xDA, 0xE2, 255))
    outline.putalpha(grown)
    tile.alpha_composite(outline, pos)
    tile.alpha_composite(logo, pos)
    tile.resize((256, 256), Image.LANCZOS).save(ico, sizes=ICON_SIZES)
    say(f"icon: {os.path.basename(src)}")
    return ico


def make_icon():
    """The icon for Setup.exe and the game. icon_art.png (or .jpg / .webp) in the repo root, if present, is
    used (see make_logo_icon; like setup_art it is not kept in the repository). Otherwise a plain icon in the
    menu's colours: dark tile, white "B1", gold bar, no game artwork."""
    from PIL import Image, ImageDraw, ImageFont
    ico = os.path.join(HERE, "game.ico")
    for ext in (".png", ".jpg", ".jpeg", ".webp"):
        src = os.path.join(ROOT, "icon_art" + ext)
        if os.path.isfile(src):
            return make_logo_icon(src, ico)
    n = 256
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, n - 1, n - 1), radius=44, fill=(0x16, 0x18, 0x20, 255))
    d.rounded_rectangle((3, 3, n - 4, n - 4), radius=41, outline=(0xFF, 0xFF, 0xFF, 40), width=3)
    try:
        font = ImageFont.truetype(os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", "segoeuib.ttf"), 150)
    except OSError:
        font = ImageFont.load_default()
    box = d.textbbox((0, 0), "B1", font=font)
    d.text(((n - (box[2] - box[0])) / 2 - box[0], 34 - box[1]), "B1", font=font, fill=(0xF4, 0xF5, 0xF8, 255))
    d.rounded_rectangle((56, 186, 200, 200), radius=7, fill=(0xE8, 0xB6, 0x4C, 255))
    im.save(ico, sizes=ICON_SIZES)
    say("icon: plain (no icon_art picture)")
    return ico


SETUP_RC = """1 ICON "%(ico)s"
1 24 "%(manifest)s"
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
      VALUE "FileDescription", "%(product)s Setup"
      VALUE "FileVersion", "%(v)s"
      VALUE "InternalName", "Setup"
      VALUE "LegalCopyright", "%(copyright)s"
      VALUE "OriginalFilename", "Setup.exe"
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

MANIFEST = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="%(publisher)s.%(product)s.Setup" version="%(n4)s"/>
  <description>%(product)s Setup</description>
  <dependency>
    <dependentAssembly>
      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0"
                        processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>
    </dependentAssembly>
  </dependency>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security><requestedPrivileges><requestedExecutionLevel level="asInvoker" uiAccess="false"/></requestedPrivileges></security>
  </trustInfo>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true</dpiAware>
    </windowsSettings>
  </application>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application><supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/></application>
  </compatibility>
</assembly>
"""


def build_setup_exe(tc, ico):
    clang = os.path.join(tc, "bin", "x86_64-w64-mingw32-clang.exe")
    windres = os.path.join(tc, "bin", "llvm-windres.exe")
    tmp = os.path.join(DIST, "obj")
    os.makedirs(tmp, exist_ok=True)
    v = product.VERSION
    n = (v.split(".") + ["0", "0", "0"])[:4]
    names = {"ico": ico.replace("\\", "/"), "manifest": os.path.join(tmp, "setup.manifest").replace("\\", "/"),
             "n": ",".join(n), "n4": ".".join(n), "v": v, "publisher": product.PUBLISHER,
             "product": product.PRODUCT, "copyright": product.COPYRIGHT}
    with open(os.path.join(tmp, "setup.manifest"), "w", encoding="utf-8") as f:
        f.write(MANIFEST % names)
    # version details (publisher, product, version) as Windows shows them under Properties > Details; an exe
    # without them looks like throwaway malware to antivirus heuristics
    with open(os.path.join(tmp, "setup.rc"), "w", encoding="utf-8") as f:
        f.write(SETUP_RC % names)
    with open(os.path.join(tmp, "product.h"), "w", encoding="utf-8") as f:
        f.write("/* written by make_package.py from product.py */\n")
        for key, val in (("P_PRODUCT", product.PRODUCT), ("P_GAME", product.GAME), ("P_EXE", product.EXE),
                         ("P_PUBLISHER", product.PUBLISHER), ("P_DISPLAY", product.DESCRIPTION),
                         ("P_VERSION", product.VERSION)):
            f.write('#define %s L"%s"\n' % (key, val))
    res = os.path.join(tmp, "setup_res.o")
    subprocess.check_call([windres, "--no-preprocess", "--target=pe-x86-64", os.path.join(tmp, "setup.rc"),
                           "-O", "coff", "-o", res])
    subprocess.check_call([clang, "-O2", "-s", "-municode", "-mwindows", "-Wall", "-I", tmp,
                           os.path.join(HERE, "setup_win32.c"), res,
                           "-o", os.path.join(OUT, "Setup.exe"), "-lcomctl32", "-lcomdlg32", "-lshell32", "-lole32",
                           "-luuid", "-lgdi32", "-luser32", "-ladvapi32"])


def package_source(ico):
    out = subprocess.check_output(["git", "ls-files", "-co", "--exclude-standard"], cwd=ROOT, text=True)
    n = 0
    for f in out.splitlines():
        if f.startswith(SOURCE_PREFIXES) and os.path.isfile(os.path.join(ROOT, f)):
            copy(os.path.join(ROOT, f), os.path.join(FILES, "source", f))
            n += 1
    copy(ico, os.path.join(FILES, "source", "tools", "install", "game.ico"))
    say(f"source: {n} files")


def package_python():
    base = sys.base_prefix
    dst = os.path.join(FILES, "python")
    for f in os.listdir(base):
        if f.lower().endswith((".exe", ".dll")) or f == "LICENSE.txt":
            copy(os.path.join(base, f), os.path.join(dst, f))
    copy_tree(os.path.join(base, "DLLs"), os.path.join(dst, "DLLs"),
              skip_file=lambda n: n.lower().startswith(PY_DLL_SKIP))
    copy_tree(os.path.join(base, "Lib"), os.path.join(dst, "Lib"), skip_dirs=PY_LIB_SKIP)
    # a ._pth file makes this Python ignore any other Python installed on the player's PC
    ver = "python%d%d" % sys.version_info[:2]
    with open(os.path.join(dst, ver + "._pth"), "w") as f:
        f.write("Lib\nDLLs\n")
    say(f"python {sys.version.split()[0]}")


def package_clang(tc):
    dst = os.path.join(FILES, "clang")
    ver = sorted(os.listdir(os.path.join(tc, "lib", "clang")))[-1]
    major = ver.split(".")[0]
    for f in ["clang.exe", "clang++.exe", f"clang-{major}.exe", "x86_64-w64-mingw32-clang.exe",
              "x86_64-w64-mingw32-clang++.exe", "ld.lld.exe", "llvm-windres.exe",
              f"libLLVM-{major}.dll", "libclang-cpp.dll", "libc++.dll", "libunwind.dll", "libwinpthread-1.dll",
              "x86_64-w64-windows-gnu.cfg", "mingw32-common.cfg"]:
        copy(os.path.join(tc, "bin", f), os.path.join(dst, "bin", f))
    copy_tree(os.path.join(tc, "lib", "clang", ver, "include"), os.path.join(dst, "lib", "clang", ver, "include"))
    copy(os.path.join(tc, "lib", "clang", ver, "lib", "windows", "libclang_rt.builtins-x86_64.a"),
         os.path.join(dst, "lib", "clang", ver, "lib", "windows", "libclang_rt.builtins-x86_64.a"))
    # libraries: the static ones the game links, and the import libraries of Windows' own DLLs
    copy_tree(os.path.join(tc, "x86_64-w64-mingw32", "lib"), os.path.join(dst, "x86_64-w64-mingw32", "lib"),
              skip_file=lambda n: n.endswith((".la", ".json")) or n.startswith(("libSDL", "libz.")))
    os.makedirs(os.path.join(dst, "x86_64-w64-mingw32", "include"), exist_ok=True)
    # Windows headers and the C++ standard library headers (include/c++/v1)
    copy_tree(os.path.join(tc, "include"), os.path.join(dst, "include"), skip_dirs=INCLUDE_SKIP,
              skip_file=lambda n: n.endswith(".idl"))
    copy(os.path.join(tc, "LICENSE.TXT"), os.path.join(OUT, "licenses", "LLVM.txt"))
    say(f"clang {ver}")


def package_n64recomp():
    dst = os.path.join(FILES, "n64recomp")
    exe = os.path.join(N64RECOMP, "build", "N64Recomp.exe")
    if not os.path.isfile(exe):
        sys.exit("third_party/N64Recomp/build/N64Recomp.exe is missing: build N64Recomp first")
    copy(exe, os.path.join(dst, "N64Recomp.exe"))
    copy(os.path.join(N64RECOMP, "include", "recomp.h"), os.path.join(dst, "recomp.h"))
    copy(os.path.join(N64RECOMP, "LICENSE"), os.path.join(dst, "LICENSE.txt"))


def package_art():
    """The Setup window's picture, as a PNG no taller than it is ever shown (Setup scales it to the window)."""
    for ext in (".png", ".jpg", ".jpeg", ".webp"):
        src = os.path.join(ROOT, "setup_art" + ext)
        if os.path.isfile(src):
            from PIL import Image
            im = Image.open(src).convert("RGB")
            if im.height > 1400:
                im = im.resize((round(im.width * 1400 / im.height), 1400), Image.LANCZOS)
            im.save(os.path.join(FILES, "setup_art.png"), optimize=True)
            say(f"setup picture: {os.path.basename(src)} {im.width}x{im.height}")
            return
    say("setup picture: none (plain dark window)")


def licenses():
    lic = os.path.join(OUT, "licenses")
    copy(os.path.join(ROOT, "LICENSE.md"), os.path.join(lic, product.PRODUCT + ".md"))
    for f in os.listdir(os.path.join(ROOT, "licenses")):
        copy(os.path.join(ROOT, "licenses", f), os.path.join(lic, f))
    copy(os.path.join(sys.base_prefix, "LICENSE.txt"), os.path.join(lic, "Python.txt"))
    with open(os.path.join(lic, "mingw-w64.txt"), "w") as f:
        f.write("The Windows headers and import libraries are from the mingw-w64 project (https://www.mingw-w64.org/),\n"
                "distributed under the Zope Public License, MIT-style licenses, and public domain dedications.\n"
                "The compiler is llvm-mingw (https://github.com/mstorsjo/llvm-mingw).\n")


README = """{product}: an unofficial PC version of Buriki One (SNK, 1999)
=================================================================

{product} does not include the game. You need your own legally obtained copies of two arcade ROM sets,
as MAME names them:

  buriki.zip   Buriki One: World Grapple Tournament '99 in Tokyo (rev. B)
  hng64.zip    the Hyper Neo Geo 64 system ROMs (the arcade board's startup and control programs)

Any MAME version's sets work as long as they are complete: Setup finds the files it needs by checksum,
whatever they are named. hng64.zip must hold the I/O board file (tmp87ph40an.bin), which MAME sets have
had for some years. No game data comes with this download.

INSTALLING
----------
1. Unzip this whole folder anywhere.
2. Run Setup.exe. Windows may say "Windows protected your PC", because Setup is not signed:
   choose "More info", then "Run anyway".
3. Choose your buriki.zip and your hng64.zip, and where to install. Then press Install.
   (If the two zips are next to Setup.exe or in your Downloads folder, Setup finds them itself.)

Setup checks the ROMs, then builds the game from them on your PC and adds shortcuts. This takes a minute
or two on a recent PC and longer on an older one. It needs about 500 MB of free disk space; the installed
game takes about 180 MB of that.
Everything it needs is in the "files" folder; nothing is downloaded. You can delete this folder afterwards.

If Setup stops with a message, it says what is wrong with the ROM set. For anything else, setup.log in
the install folder has the details.

PLAYING
-------
To start a game: insert a coin, then press Start.
Buriki One plays differently from most fighting games: the buttons move your fighter and the stick
attacks. The game's own "How to play" screens show the moves for each fighter.

CONTROLLER (Xbox-style; the first controller is player 1, the second player 2)
----------
  Stick              D-pad or left stick
  Buttons 1 2 3 4    X  Y  A  B
  Start              Start (Menu)
  Insert coin        Back (View)
  Menu               Back + Start together

KEYBOARD
--------
                     Player 1           Player 2
  Stick              arrow keys         R F D G  (up, down, left, right)
  Buttons 1 2 3 4    Z X C V            A S Q W
  Start              1                  2
  Insert coin        5                  6

  Tab / Esc          menu (quit from its System page)
  Alt+Enter, F11     window / full screen
  F9                 enhanced graphics on / off
  F2                 the arcade's Test button (operator menu)
  9                  the arcade's Service button

Every key and controller button can be changed in the menu's Controls page.

THE MENU
--------
Controller: press Back and Start together (on Xbox Series / One pads: View + Menu).
Keyboard: Tab or Esc.
The game pauses while the menu is open. Its picture is one of the game's ten fighting-style portraits,
a different one each time.
  Controller: D-pad or stick to move, A to choose, B to go back, LB / RB to switch pages, Start to close.
  Keyboard: arrows to move, Enter to choose, Esc to go back, Q / E to switch pages, Tab to close.
Pages:
  Display   window / borderless / exclusive full screen, window size, monitor,
            picture shape (arcade 4:3, stretch, whole-number, widescreen),
            enhanced or original graphics, quality, screen filter (sharp / smooth / scanlines),
            frame pacing, graphics card, frame-rate counter
  Audio     volume, mute
  Controls  change any key or button ("press the one you want"), stick dead zone, restore defaults
  Game      language / region: Japan (Japanese), USA or World (English). Applies after a restart.
  System    resume, the arcade's operator menu, restart, quit

Widescreen (Display > Picture shape): fights fill a 16:9 screen, up to 21:9, with more of the arena at
the sides. Menus and other flat screens stay 4:3. It needs Graphics set to Enhanced.

Settings are saved in settings.ini, keys and buttons in controls.ini, and the arcade board's own saved
settings and records in nvram.bin (all in the install folder).

WHAT YOUR PC NEEDS
------------------
Windows 10 or 11 (64-bit) and a graphics card with Direct3D 11.

REMOVING THE GAME
-----------------
Windows Settings > Apps > {display} > Uninstall. It asks whether to keep your settings.

REPORTING A PROBLEM
-------------------
Please include buriki.log from the install folder (the run before it is buriki.old.log), and setup.log
if the problem was with installing.

The licenses of {product} and of the tools Setup uses are in the "licenses" folder. {product}'s source
is in files\\source.

{product} is an unofficial fan project, not affiliated with or endorsed by SNK.
Buriki One and Hyper Neo Geo 64 are trademarks of their owners.
"""


def main():
    tc = toolchain_root()
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(FILES)
    ico = make_icon()
    build_setup_exe(tc, ico)
    package_source(ico)
    package_python()
    package_clang(tc)
    package_n64recomp()
    package_art()
    licenses()
    with open(os.path.join(OUT, "README.txt"), "w", newline="\r\n") as f:
        f.write(README.format(product=product.PRODUCT, display=product.DESCRIPTION))
    shutil.rmtree(os.path.join(DIST, "obj"), ignore_errors=True)

    zpath = OUT + ".zip"
    total = 0
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for d, _, files in os.walk(OUT):
            for f in files:
                p = os.path.join(d, f)
                total += os.path.getsize(p)
                z.write(p, os.path.join(NAME, os.path.relpath(p, OUT)))
    say(f"package: {total / 2**20:.0f} MB unpacked, {os.path.getsize(zpath) / 2**20:.0f} MB zipped -> {zpath}")


if __name__ == "__main__":
    main()
