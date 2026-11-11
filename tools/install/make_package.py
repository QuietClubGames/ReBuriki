#!/usr/bin/env python3
"""
Make the Setup download (developer machine only):

    python tools/install/make_package.py     -> dist/<Product>-Setup/ and dist/<Product>-Setup.zip

The package holds Setup.exe, a trimmed Python, a trimmed 64-bit clang++, the N64Recomp translator and this
project's source (tracked files only). It holds NO ROM data and nothing made from the ROMs: Setup builds the
game from the player's own buriki.zip and hng64.zip. Needs the developer toolchain on PATH
(x86_64-w64-mingw32-clang++, llvm-mingw), N64Recomp built in third_party/N64Recomp/build, and Pillow (icon).

setup_art.png (or .jpg / .webp) in the repo root becomes the picture down the right-hand side of the Setup
window (files/setup_art.png); icon_art.png becomes the icon of Setup.exe and of the game. Both are official
Buriki One artwork (the only game material in the repository and the package, kept at the owner's choice).
Without them Setup is a plain dark window and the icon a plain "B1".

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
NEVER_PACKAGE = (".bin", ".zip", ".bmp", ".wav", ".log", ".ini", ".exe", ".o")   # dumps, saves, ROM sets, builds
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
    used (see make_logo_icon). Otherwise a plain icon in the
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
        if f.lower().endswith(NEVER_PACKAGE):
            continue
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


UTF8_MANIFEST = b"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
    </windowsSettings>
  </application>
</assembly>
"""


def add_utf8_manifest(exe):
    """Give an exe a manifest that makes its file paths UTF-8 (Windows 10 1903 and later). N64Recomp turns
    paths into narrow strings and stops on a folder name with letters outside the PC's code page (an install
    folder under a user name in another alphabet); with this it reads them as UTF-8."""
    import ctypes
    from ctypes import wintypes
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.BeginUpdateResourceW.restype = wintypes.HANDLE
    k.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
    k.UpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.LPVOID, wintypes.WORD, wintypes.LPVOID,
                                  wintypes.DWORD]
    k.EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]
    h = k.BeginUpdateResourceW(exe, False)
    if not h:
        sys.exit(f"can't open {exe} to add its manifest (error {ctypes.get_last_error()})")
    RT_MANIFEST, ID, LANG_EN_US = 24, 1, 0x0409
    ok = k.UpdateResourceW(h, RT_MANIFEST, ID, LANG_EN_US, UTF8_MANIFEST, len(UTF8_MANIFEST))
    if not k.EndUpdateResourceW(h, not ok) or not ok:
        sys.exit(f"can't add the manifest to {exe} (error {ctypes.get_last_error()})")


def package_n64recomp():
    dst = os.path.join(FILES, "n64recomp")
    exe = os.path.join(N64RECOMP, "build", "N64Recomp.exe")
    if not os.path.isfile(exe):
        sys.exit("third_party/N64Recomp/build/N64Recomp.exe is missing: build N64Recomp first")
    copy(exe, os.path.join(dst, "N64Recomp.exe"))
    add_utf8_manifest(os.path.join(dst, "N64Recomp.exe"))
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
   Setup installs into a folder of its own: a new or empty one, or one that already holds the game.
   Installing over an older version keeps your settings and saves.

Setup checks the ROMs, then builds the game from them on your PC and adds shortcuts. This takes a minute
or two on a recent PC and longer on an older one. It needs about 500 MB of free disk space; the installed
game takes about 180 MB of that.
Everything it needs is in the "files" folder; nothing is downloaded. You can delete this folder afterwards.

If Setup stops with a message, it says what is wrong with the ROM set. For anything else, setup.log in
the install folder has the details.

PLAYING
-------
To start a game: insert a coin, then press Start. With "Free play" on (the menu's Game page), Start
alone is enough.
Buriki One plays differently from most fighting games: the buttons move your fighter and the stick
attacks. The game's own "How to play" screens show the moves for each fighter.

The game uses two buttons, which it calls L and R: in a fight they step left and step right, and held
together they raise your guard. On the cabinet the same two buttons also confirm on the game's own
screens, such as choosing your fighter. {product} treats stepping, guarding and confirming as separate
controls, so a trigger doesn't pick a fighter and A doesn't step.

CONTROLLER (Xbox-style; the first controller is player 1, the second player 2)
----------
There are three ready-made control schemes (menu > Controls > Control scheme) and Custom, your own.

  Modern (the default): plays like a modern game
    Step left / right     left stick, or LT / RT
    Attack                right stick, or D-pad
    Guard                 LB, RB or X
  Classic: the stick attacks, as on the cabinet
    Step left / right     LT, LB / RT, RB
    Attack                either stick, or D-pad
    Guard                 X, or both triggers
  Buttons attack
    Step left / right     left stick, or LT / RT
    Attack                Y A X B (up, down, left, right), right stick or D-pad
    Guard                 LB or RB
  In every scheme
    On the game's screens either stick or the D-pad moves the cursor, A selects, B is the other button
    Start                 Start (Menu)
    Insert coin           Back (View)
    Menu                  Back + Start together

KEYBOARD (the same in every scheme)
--------
                          Player 1           Player 2
  Stick                   arrow keys         R F D G  (up, down, left, right)
  Step left / right       Z  X               A  S     (and Select / Other on the game's screens)
  Guard                   Space, or Z + X    A + S
  Buttons 3 and 4         C  V               Q  W     (spare)
  Start                   1                  2
  Insert coin             5                  6

  Tab / Esc          menu (quit from its System page)
  Alt+Enter, F11     window / full screen
  F9                 enhanced graphics on / off
  F2                 the arcade's Test button (operator menu: its other settings, tests, records)
  9                  the arcade's Service button

Every key and controller button can be changed in the menu's Controls page. "Show" switches between
its three groups: in a fight; the game's screens and the cabinet; sticks. Choose a control and press
Enter (A on a controller) to open its box: press a controller button to add it and again to take it
off (up to three), or press a key to make it the keyboard key. The same key or button can be on several
controls. Each stick can step left and right, attack, or be switched off.
Change anything about the controller and the scheme becomes Custom, starting from the scheme you were
on. Custom is remembered, so you can try another scheme and come back to it.

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
            picture shape (arcade 4:3, stretch, whole-number, widescreen), where the health bars sit in
            widescreen, enhanced or original graphics, quality, screen filter (sharp / smooth /
            scanlines), frame pacing, graphics card, frame-rate counter
  Audio     volume, mute
  Controls  control scheme; change any key or button; what each stick does; dead zone; restore defaults
  Game      unlock Silber (on by default); free play; language / region: Japan (Japanese), USA or
            World (English), which applies after a restart
  Rules     the arcade operator's settings: difficulty, attack power and timer speed (1 player and
            versus), continue, replay, blood, move commands on screen, sound in the title demo.
            They apply from the next fight and are kept in the arcade's save memory.
  Online    experimental: host a game, join one by address, input delay, leave (see PLAYING ONLINE)
  System    resume, the arcade's operator menu, restart, quit

Silber, the final boss, is a hidden twelfth fighter ("Original Karate", at the bottom of the style
list). With "Unlock Silber" on he can be picked from the start. Turn it off to earn him the arcade's
way; what you have earned is kept either way.

Widescreen (Display > Picture shape): fights fill a 16:9 screen, up to 21:9, with more of the arena at
the sides. Menus and other flat screens stay 4:3. It needs Graphics set to Enhanced.
"Widescreen display" chooses where the health bars and the boxes at the top sit: where the arcade
put them, or out at the edges of the screen.

Settings are saved in settings.ini, keys and buttons in controls.ini, and the arcade board's own saved
settings and records in nvram.bin (all in the install folder).

PLAYING ONLINE (EXPERIMENTAL)
-----------------------------
This part is experimental. It has been tested thoroughly with two copies of the game on one PC through
simulated slow and unreliable connections, but not yet much between two real PCs over the internet.
It connects the two players' PCs directly: there is no server, lobby or matchmaking, so you arrange a
game with someone yourselves. Nothing here affects ordinary play.

Two players, each on their own PC with their own copy of {product}. Each plays with their own
Player 1 keys or first controller. The host is player 1 in the game and the joiner player 2.

  Host:    menu > Online > Host a game. The game restarts and shows a waiting screen with your address.
  Joiner:  menu > Online > Address to join: type the host's address (Enter to start typing, Ctrl+V
           pastes). Then Join a game.

The game then starts from power on for both, with the host's settings (free play, the rules, Silber).
Put in a coin, press Start, and the second player pressing Start is "here comes a new challenger".
To stop, either player chooses Leave the online game. Both go back to ordinary play.

  Same home network: the joiner types the address shown on the host's waiting screen.
  Over the internet: the joiner needs the host's public address (search the web for "what is my IP"),
    and the host's router must forward UDP port 7600 to the host's PC. A virtual-LAN tool such as
    Tailscale, ZeroTier or Radmin VPN avoids the router setup: both players install it, and the joiner
    types the host's address in that tool.
  Windows Firewall asks the first time you host. Choose Allow.
  Both players need the same Language / region (Game page), game files and version of {product}.
  Pausing pauses both players. The other one sees "Waiting for the other player".
  Input delay (the host's choice): 2 suits most connections. Raise it if the picture often jumps.
  With the frame-rate counter on (Display page), the ping is shown next to it.

If something goes wrong online, please report it with buriki.log from both players.

PICTURE AND GRAPHICS (menu > Display)
--------------------
  Graphics         Enhanced redraws the 3D fighters on your graphics card at high resolution. Original
                   arcade is the board's own 512x448 picture. F9 switches while you play.
  Quality          Auto picks the sharpest setting your PC holds at full speed and steps down if it
                   ever can't. The fixed settings run from Performance (2x) to Extreme (8x).
  Picture shape    Arcade 4:3, stretch to fill, 4:3 at a whole-number scale, or Widescreen (needs
                   Enhanced graphics; fights only, the flat screens stay 4:3).
  Screen filter    Sharp pixels, smooth scaling, or arcade-monitor scanlines.
  Frame pacing     Arcade speed is the board's own 59.43 frames a second. Display vsync is smoother
                   on a 60 Hz screen but about 1% fast.
  Display mode     Window, borderless full screen, or exclusive full screen. Alt+Enter / F11 switch.

YOUR FILES (all in the install folder)
----------
  settings.ini     the menu's settings
  controls.ini     keys, controller buttons and schemes (it can be edited by hand)
  nvram.bin        the arcade's own save memory: operator settings, records, what you have unlocked
  buriki.log       what happened in the last run (buriki.old.log is the run before)
  setup.log        what Setup did
The game sends nothing over the internet except, in an online game, your button presses to the other
player. To start completely fresh, delete settings.ini, controls.ini and nvram.bin with the game closed.
To update, install a newer version into the same folder: your settings and saves are kept.

WHAT YOUR PC NEEDS
------------------
Windows 10 or 11 (64-bit) and a graphics card with Direct3D 11. Without Direct3D 11 the game still
runs, with the original picture and no pause menu.
A controller must be Xbox-style (XInput). PlayStation controllers work through Steam or a tool such as
DS4Windows, and many arcade sticks have an XInput mode. The keyboard always works.

IF SOMETHING GOES WRONG
-----------------------
Setup says a ROM file is missing or wrong
  Setup names the file. buriki.zip must be the rev. B set, and hng64.zip must include the I/O board
  file tmp87ph40an.bin (older MAME sets don't have it). Both must be complete.
Windows or an antivirus stops Setup
  Setup is not signed. On "Windows protected your PC" choose "More info", then "Run anyway". If an
  antivirus removed Setup.exe or something in the "files" folder, restore and allow it (or unzip the
  download again) and run Setup again. Please don't turn your antivirus off.
The picture looks wrong, or the 3D is missing
  Press F9 to switch between Enhanced and Original graphics. If Original looks right and Enhanced
  doesn't, update your graphics driver. On a PC with two graphics cards, try the other one (Display >
  Graphics card; it applies after System > Restart game).
The game runs slowly or stutters
  Set Quality to Auto or Performance. The frame-rate counter (Display) should read about 59 or 60.
  If Frame pacing is on Display vsync and your screen is not close to 60 Hz, use Arcade speed.
The pause menu doesn't open
  It is drawn with Direct3D 11. On a PC without it the menu can't be shown; settings.ini and
  controls.ini can still be edited by hand.
The controller doesn't work, or the buttons are odd
  The Controls page shows whether each player's controller is connected. Choose a Control scheme
  there, or Restore defaults.
There is no sound
  Check Volume and Mute (Audio page), and that Windows is playing to the right device. The title demo
  is silent if "Sound in the title demo" is off (Rules page).
Online: the joiner gets "no answer"
  The host must be on the waiting screen first. On the same network use the address shown there. Over
  the internet use the host's public address, and the host's router must forward UDP port 7600 to
  the host's PC (or both use a virtual-LAN tool). The host must have allowed the game through
  Windows Firewall.
Online: "the two players' games differ"
  Both need the same Language / region (Game page), the same ROM sets and the same version.

KNOWN GAPS
----------
  The sound chip's echo effect is not reproduced. The game uses it, but how the chip does it is not
  documented, and no emulator has it yet.
  Online play is experimental and connects directly: no lobby, and over the internet the host needs a
  forwarded port or a virtual-LAN tool.
  Setup is not code-signed, so Windows warns about it.
  Windows only.
Everything else is believed to match the arcade.

REMOVING THE GAME
-----------------
Windows Settings > Apps > {display} > Uninstall, or run Uninstall.exe in the install folder.
It asks whether to keep your settings. Only the game's own files are removed.

REPORTING A PROBLEM
-------------------
Please include buriki.log from the install folder (the run before it is buriki.old.log), and setup.log
if the problem was with installing. For an online problem, the log from both players helps.
The project's page, with this guide and pictures: https://github.com/QuietClubGames/ReBuriki

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
