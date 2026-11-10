"""The names the installer and the installed game go by. The one place to change them.

Used by make_package.py (Setup.exe, README) and build_from_rom.py (the game's exe name and version details).
"""
PRODUCT = "ReBuriki"                       # the PC version's own name: Setup title, shortcuts, install folder
GAME = "Buriki One"                        # the arcade game it runs
EXE = "ReBuriki.exe"
VERSION = "0.1.0"
PUBLISHER = "QuietClubGames"
DESCRIPTION = "%s (%s PC port)" % (PRODUCT, GAME)
COPYRIGHT = "%s code: see the licenses folder. %s is a trademark of its owners." % (PRODUCT, GAME)
