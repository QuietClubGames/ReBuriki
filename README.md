<div align="center">

# ReBuriki

**Buriki One, rebuilt for Windows.**<br>
An unofficial PC version of SNK's 1999 Hyper Neo Geo 64 arcade fighter.

[![Latest release](https://img.shields.io/github/v/release/QuietClubGames/ReBuriki?label=download&style=for-the-badge&color=e8a33d)](../../releases/latest)
[![Downloads](https://img.shields.io/github/downloads/QuietClubGames/ReBuriki/total?style=for-the-badge&color=4c8bf5)](../../releases)
![Windows](https://img.shields.io/badge/Windows-10%20%2F%2011-0078D6?style=for-the-badge&logo=windows&logoColor=white)

**[Download](../../releases/latest)** · [Install](#install) · [The menu](#the-menu) · [Controls](#controls) · [FAQ](#faq) · [For developers](docs/DEVELOPING.md)

</div>

> [!IMPORTANT]
> ReBuriki does **not** include the game. You need your own legally obtained copies of two arcade ROM sets, as
> MAME names them: `buriki.zip` (Buriki One, rev. B) and `hng64.zip` (the Hyper Neo Geo 64 system ROMs). Setup
> builds the game from them on your PC.

## What ReBuriki adds

| | |
|---|---|
| **Sharper 3D** | The fighters are redrawn on your graphics card at your screen's resolution, with smooth edges and textures. Or keep the original picture (F9 switches). |
| **Widescreen** | Fights fill a 16:9 screen, up to 21:9, with more of the arena at the sides. The health bars can stay where the arcade put them or sit at the screen edges. |
| **Your screen, your way** | Window, borderless or exclusive full screen; 4:3, stretched or pixel-perfect scaling; sharp, smooth or scanline filter. |
| **English or Japanese** | Choose the board's region from the menu. |
| **Controllers** | Xbox-style controllers for two players, with a layout made for this game (triggers and bumpers move, A selects, and the two don't get in each other's way), keyboard, and remapping for both. |
| **Arcade settings** | Free play, difficulty, attack power, timer speed and more from an in-game menu, plus an option to have the hidden fighter Silber from the start. |
| **Online play (experimental)** | Two players over the internet or a home network, with rollback so it stays responsive on an ordinary connection. See [Playing online](#playing-online-experimental). |
| **Native** | Not an emulator: the arcade code is recompiled into a Windows program. |

## Install

1. Download **`ReBuriki-Setup.zip`** from the [latest release](../../releases/latest) and unzip it anywhere.
2. Run **`Setup.exe`**.
3. Choose your `buriki.zip` and `hng64.zip` and where to install, then press **Install**. It takes a minute or
   two and adds desktop and Start menu shortcuts. No administrator rights are needed.

Any MAME version's sets work as long as they are complete: Setup finds the files it needs by checksum, whatever
they are named. `hng64.zip` must include the I/O board file `tmp87ph40an.bin`.

> [!NOTE]
> Setup isn't code-signed, so Windows may show **"Windows protected your PC"**. Choose **More info**, then
> **Run anyway**. If your antivirus blocks `Setup.exe`, that is an automatic guess about new, unsigned
> programs: allow it in the antivirus's history list and run Setup again. Please don't turn your antivirus off.

To uninstall: **Windows Settings → Apps → ReBuriki (Buriki One PC port) → Uninstall**.

## The menu

Press **Back + Start** together on a controller (View + Menu on newer Xbox pads), or **Tab** / **Esc** on the
keyboard. The game pauses while the menu is open.

| | Controller | Keyboard |
|---|---|---|
| Move | D-pad or stick | Arrow keys |
| Choose | A | Enter |
| Back | B | Esc |
| Switch pages | LB / RB | Q / E |
| Close | Start | Tab |

| Page | What's there |
|---|---|
| **Display** | Window / borderless / exclusive full screen, window size, monitor, picture shape (4:3, stretch, whole-number, widescreen), where the health bars sit in widescreen, enhanced or original graphics, quality, screen filter, frame pacing, graphics card, frame-rate counter |
| **Audio** | Volume, mute |
| **Controls** | In three groups (in a fight; the game's screens and the cabinet; sticks): change any key or controller button, up to three buttons per control, which stick attacks, stick dead zone, restore defaults |
| **Game** | Unlock Silber, free play, language / region (applies after a restart) |
| **Rules** | The arcade operator's settings: difficulty, attack power and timer speed (1 player and versus), continue, replay, blood, move commands on screen, sound in the title demo |
| **Online** | Experimental. Host a game, join one by address, input delay, this PC's address, leave |
| **System** | Resume, the arcade's operator menu, restart, quit |

## Controls

Insert a coin, then press Start. With free play on, Start alone is enough. The first controller is player 1,
the second is player 2.

Buriki One plays differently from most fighting games: two buttons move your fighter and the stick
attacks. The game calls the buttons L and R: step left and step right. The game's own "How to play"
screens show the moves.

On the arcade cabinet those same two buttons also confirm on the game's own screens, such as choosing
your fighter. ReBuriki treats the two jobs as separate controls, so a trigger doesn't pick a fighter
and A doesn't step:

| | Controller | Keyboard (player 1) | Keyboard (player 2) |
|---|---|---|---|
| **In a fight** | | | |
| Stick (attacks) | D-pad, or either stick | Arrow keys | R F D G (up, down, left, right) |
| Move left | LT or LB | Z | A |
| Move right | RT or RB | X | S |
| **On the game's screens** | | | |
| Move the cursor | D-pad, or either stick | Arrow keys | R F D G |
| Select | A | Z | A |
| Other button | B | X | S |
| **The cabinet** | | | |
| Start | Start | 1 | 2 |
| Insert coin | Back | 5 | 6 |
| Buttons 3 and 4 (spare) | X, Y | C, V | Q, W |
| Menu | Back + Start | Tab or Esc | |

On the keyboard the same two keys do both jobs, as on the cabinet.

All of it can be changed on the menu's Controls page. **Show** switches between the three groups.
Choose a control and press Enter (A on a controller) to open its box:

- Press a controller button to add it, and again to take it off. A control can have up to three.
- Press a key to make it the keyboard key.
- The same key or button can be on several controls. Put A on Move left as well, for example, if
  you want it to do both jobs the way the cabinet did.
- A stick direction can go on a button too, if you would rather attack with buttons.
- **Attack stick** (in the Sticks group) chooses the left stick, the right stick, or either.

| Key | Does |
|---|---|
| Alt+Enter or F11 | Window / full screen |
| F9 | Enhanced graphics on / off |
| F2 | The arcade's Test button (operator menu) |
| 9 | The arcade's Service button |

## Playing online (experimental)

**This part is experimental.** It has been tested thoroughly with two copies of the game on one PC,
through simulated slow and unreliable connections, and the two stayed in step every time. It has not
yet been played much between two real PCs over the internet. It connects the two players' PCs directly:
there is no server, lobby or matchmaking, so you arrange a game with someone yourselves. Nothing here
affects ordinary play. If something goes wrong, please report it with `buriki.log` from both players
(see the FAQ); the game writes a line there if the two sides ever disagree.

Two players, each on their own PC with their own copy of ReBuriki. One hosts and is player 1; the other
joins and is player 2. Each of you plays with your own Player 1 keys or first controller.

1. **Host:** open the menu, go to **Online**, choose **Host a game**. The game restarts and shows a
   waiting screen with your address.
2. **Joiner:** on the Online page choose **Address to join**, type the host's address (Enter to start
   typing, Ctrl+V pastes), then **Join a game**.
3. The game starts from power on for both of you, with the host's settings: free play, difficulty and
   the other rules, and whether Silber is unlocked. Put in a coin (unless free play is on), press Start,
   and the second player pressing Start is the arcade's "here comes a new challenger".

To stop, either player chooses **Leave the online game** (or closes the game). The other is told, and
both go back to ordinary play.

Things to know:

- **Same network** (two PCs at home): the joiner types the address the host's waiting screen shows.
- **Over the internet:** the joiner needs the host's public address (the host can search the web for
  "what is my IP"), and the host's router must forward **UDP port 7600** to the host's PC. A virtual-LAN
  tool such as Tailscale, ZeroTier or Radmin VPN avoids the router setup: both install it and the joiner
  types the host's address in that tool.
- **Windows Firewall** asks the first time you host. Choose Allow.
- **Both players need the same Language / region** (Game page) and the same game files and version of
  ReBuriki. If not, the joiner is told so and nothing starts.
- **Pausing pauses both.** While one player has the menu open, the other sees "Waiting for the other
  player".
- **Input delay** (Online page, the host's choice): the number of frames before the game sees a button
  press. 2 suits most connections. Raise it if the picture often jumps; lower it on a very good
  connection.
- With the frame-rate counter on (Display page), the ping is shown next to it.
- The joiner's own arcade save memory is not touched; the host's is used and kept.

## FAQ

<details>
<summary><b>Is this an emulator?</b></summary>

No. ReBuriki is a *static recompilation*: a tool translates the arcade game's MIPS processor code into C,
which is compiled into a native Windows program, with the arcade hardware (graphics, sound, inputs) replaced
by portable code. This happens on your PC when you install, from your own ROMs.
</details>

<details>
<summary><b>Which ROMs do I need?</b></summary>

The MAME sets `buriki.zip` (Buriki One: World Grapple Tournament '99 in Tokyo, rev. B) and `hng64.zip` (the
Hyper Neo Geo 64 system ROMs). Please don't ask where to get them.
</details>

<details>
<summary><b>How do I play as Silber?</b></summary>

He is the twelfth style, "Original Karate", at the bottom of the list on the player select screen. The menu's
"Unlock Silber" option (Game page) is on by default. Turn it off to earn him the arcade's way; what you have
earned is kept either way.
</details>

<details>
<summary><b>My controller doesn't work.</b></summary>

ReBuriki recognises Xbox-style (XInput) controllers. PlayStation controllers work through Steam or a tool such
as DS4Windows, and many arcade sticks have an XInput mode. The keyboard always works.
</details>

<details>
<summary><b>Where are my settings saved?</b></summary>

In the install folder: `settings.ini` (menu settings), `controls.ini` (keys and buttons) and `nvram.bin` (the
arcade's own settings and records).
</details>

<details>
<summary><b>What do I need to run it?</b></summary>

64-bit Windows 10 or 11 and a graphics card with Direct3D 11. Without one the game falls back to the original
picture.
</details>

<details>
<summary><b>Is anything missing compared with the arcade?</b></summary>

The sound chip's echo effect isn't reproduced yet (no emulator has it). Everything else is believed to match;
reports are welcome.
</details>

<details>
<summary><b>Something went wrong. How do I report it?</b></summary>

Open an [issue](../../issues) and attach `buriki.log` from the install folder (the previous run's is
`buriki.old.log`), and `setup.log` if the problem was with installing.
</details>

## About

ReBuriki was made with Claude, Anthropic's AI model. Its licences are in [LICENSE.md](LICENSE.md): the
project's own code is MIT, and the parts ported from MAME keep MAME's licences. The licences cover ReBuriki
only, not the game. Building it yourself is described in [docs/DEVELOPING.md](docs/DEVELOPING.md).

<sub>ReBuriki is a fan project, not affiliated with or endorsed by SNK. Buriki One and Hyper Neo Geo 64 are
trademarks of their owners.</sub>
