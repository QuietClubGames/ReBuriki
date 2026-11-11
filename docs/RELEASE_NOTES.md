# ReBuriki release notes

## 0.1.0 (first release)

ReBuriki is an unofficial PC version of Buriki One (SNK, 1999, Hyper Neo Geo 64). It does not include the
game: you need your own legally obtained copies of the MAME sets `buriki.zip` (Buriki One, rev. B) and
`hng64.zip` (the Hyper Neo Geo 64 system ROMs). Setup turns them into a native Windows program on your PC.

**Download:** `ReBuriki-Setup.zip`. Unzip it, run `Setup.exe` and choose your two ROM sets.

What's in it:

- The whole game, running natively: one and two players, the attract mode, the arcade's operator menu and
  its saved settings and records.
- **Enhanced graphics:** the fighters are redrawn at your screen's resolution with smooth edges and textures.
  F9 switches back to the arcade's own picture.
- **Widescreen:** fights fill a 16:9 screen, up to 21:9. Menus and other flat screens stay 4:3. The health
  bars can stay in the arcade's position or move to the screen edges.
- **A pause menu** (Tab / Esc, or Back + Start on a controller): display, audio, controls, game, rules and
  system pages. Its picture is one of the game's ten fighting-style portraits, a different one each time.
- **English or Japanese** by choosing the board's region.
- **Silber**, the hidden twelfth fighter, selectable from the start (it can be turned off).
- **Free play** and the arcade operator's rules (difficulty, attack power, timer speed, continue, replay,
  blood and more) from the menu.
- Xbox-style controllers for two players, keyboard, and remapping for both. Three ready-made controller
  schemes and a fully custom one. The default, Modern, moves on the left stick and attacks on the right,
  with a guard button. Stepping, guarding and selecting are separate controls (on the cabinet two
  buttons did all three), each control can have up to three buttons, and a button can be on several.
- **Online play (experimental)** for two players (menu: Online). One hosts, the other joins by address. It uses
  rollback, so your own moves respond at once and the game corrects itself when the other player's
  arrive late.

Known gaps:

- The sound chip's echo effect is not reproduced (no emulator has it yet).
- Online play connects directly: over the internet the host needs UDP port 7600 forwarded on their
  router, or both players use a virtual-LAN tool. There is no lobby or matchmaking. It has been tested
  thoroughly on one PC with simulated bad connections, and not yet widely between different PCs.
- Setup is not code-signed, so Windows shows "Windows protected your PC" (More info → Run anyway), and an
  antivirus may want you to allow it.

If something goes wrong, please open an issue and attach `buriki.log` from the install folder (the run before
it is `buriki.old.log`), and `setup.log` if the problem was with installing.
