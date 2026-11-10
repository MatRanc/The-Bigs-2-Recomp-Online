# The Bigs 2 Online — PC port with online play

Play *The Bigs 2* (Xbox 360, 2K Sports, 2009) **online against a friend** again,
on Windows, Linux and macOS.

This is a fork of [The Bigs 2 Recompiled](https://github.com/mdomangue26/The-Bigs-2-Recompiled)
by **Michael (mdomangue26)**, a native PC port made by recompiling the Xbox 360
game with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). This fork
brings back the game's **online mode**: the original peer-to-peer netcode,
with a small stand-in for the Xbox Live service that shut down. Two players meet
in the Game Room, pick their teams and play a full match, in sync.

> **Clean room.** This repo contains no game code, game files or 2K data: no
> executable (`default.xex`), no disc contents, no recompiled output, no
> captures or memory dumps. It holds the port's own code and describes how the
> game's online mode talks to the other player, so people can keep playing a
> game they own. **You need your own copy of the game** (a disc image, `.iso`).
> *The Bigs 2* is © 2K Sports / Take-Two Interactive; MLB marks are the property
> of Major League Baseball. The logo used for the icons is a trademark of its
> owners. Not affiliated with 2K, Take-Two or MLB.

## Download and play

1. Download the file for your system from the
   **[Releases](https://github.com/MatRanc/The-Bigs-2-Recompiled/releases)** page:

   | System | File |
   |---|---|
   | Windows 10/11 (64-bit) | `TheBigs2-windows-x64.zip` |
   | Linux (and Steam Deck) | `TheBigs2-linux-x64.tar.gz` |
   | macOS | `TheBigs2-macos-arm64.zip` *(experimental, when available)* |

2. Unpack it into a folder of its own and put your `.iso` next to it.
3. Start **The Bigs 2 Launcher**:
   - **Windows:** double-click `The Bigs 2 Launcher.bat`. It needs
     [Python 3](https://www.python.org/downloads/) (the default install is fine).
   - **Linux:** run `./thebigs2_gui.py` (needs Python 3 with Tk, e.g. the
     `python3-tk` package).
   - **macOS:** run `python3 thebigs2_gui.py` in the folder. The first time,
     allow the unsigned game with `xattr -dr com.apple.quarantine .` in that
     folder.
4. Press **Launch Game**. The first start asks for your disc image (an `.iso`
   next to the game is preselected), copies its files once, and the game starts.

You can also start the game directly (`thebigs2.exe`, the AppImage, or
`thebigs2` on macOS) to play offline.

## Play online with a friend

Both players need this build and their own copy of the game.

1. **Get on the same network.** The easiest way is [Tailscale](https://tailscale.com)
   (free): both install it and join the same tailnet (the host can share their
   machine with the friend). Nothing to set up on your router.
2. **Host:** in the launcher's **Online** tab pick **Host a game**, type a
   player name, and send your friend your IP from *Your IPs* (the `100.x.y.z`
   one with Tailscale). Press **Launch Game**.
3. **Friend:** pick **Join a friend**, type a player name and the host's IP,
   press **Launch Game**.
4. **In the game, both:** ONLINE > Quick Match > Search.
   - The host sees *No Game Rooms found*: press **X (Create Game)**.
   - The friend lands in the host's **Game Room**.
5. Both press **A (Ready)**. In Team Setup move your controller to a side with
   the d-pad, pick your team and uniform, then the pitcher and lineup. The host
   picks the stadium, and the match starts.

**Good to know**
- Player names: up to 8 characters, and the two players need different names.
- Windows asks whether to allow the game on the network the first time: allow
  it. Without Tailscale the host has to forward **TCP 51000** and **UDP 1795**.
- The game runs in lockstep: both sides wait for each other, so if one PC
  stutters, both do.
- Testing on one PC with two copies works too, but they share the graphics card
  (expect ~20 fps each).

## Features

- **Online play** against a friend (see above), with a launcher to set it up.
- **Runs natively** on Windows and Linux (desktop and Steam Deck), macOS
  experimental.
- **Sharper picture**: renders at 2x (1440p) by default, with 1x and 3x (4K)
  available.
- **Experimental 60 fps** in matches (the original runs matches at 30).
- **Steam Deck build** with Deck-tuned defaults; works in Game Mode.
- **Setup screen** on first launch: pick your disc image and it's extracted
  once. Works with mouse, keyboard, controller and touchscreen.
- **Settings menu in the game's own style**: open it from **RECOMP SETTINGS**
  on the main menu, with **F4**, or by holding **Back + Y** on a controller.
  The launcher edits the same settings.
- **Main menu additions**: ONLINE, RECOMP SETTINGS and EXIT GAME.
- **Controller button remapping** and **keyboard controls**.
- **Portable**: settings, saves, logs and the extracted game live next to the
  game.
- Saving and loading work.

## Key commands

| Keyboard | Controller | Does |
|---|---|---|
| F4 | hold Back + Y (1 second) | Settings menu |
| F10 | hold Back + Start (2 seconds) | Quit the game |
| F3 | | Overlay with the frame rate |
| F7 / Backtick | | Achievements / console |

You can also quit with **EXIT GAME** on the main menu.

**In the settings menu:** LB / RB (Page Up / Page Down) switch tabs, up and
down pick a setting, left and right change it. Start (Ctrl+S) saves, Y saves
and quits, B (Escape) closes. Some settings apply at once, the others after a
restart; the menu marks which.

### Playing with the keyboard

The keyboard works as player 1's controller, alongside a gamepad:

| Controller | Keys |
|---|---|
| A / B / X / Y | `;` or Space / `'` or Backspace / L / P |
| LB / RB | 1 / 3 |
| LT / RT | Q or I / E or O |
| Back / Start | Z or Tab / X or Return |
| Left stick (click) | W A S D (F) |
| Right stick (click) | arrow keys (K) |
| D-pad | Shift + arrow keys |

Change the keys on the settings menu's **Keyboard** tab, and remap controller
buttons on its **Controller** tab (e.g. swap A and B). Button remapping takes
effect after a restart.

## Settings

The settings menu and the launcher edit `config/thebigs2.toml`. You can also
edit the file by hand: every setting is listed in it with an explanation. The
main ones:

| Setting | Default | |
|---|---|---|
| `fullscreen` | `true` | `false` for a window |
| `display_resolution` | `"desktop"` | Window size or fullscreen mode, e.g. `"1920x1080"` |
| `resolution_scale` | `2` | Render scale: `1` = 720p, `2` = 1440p, `3` = 4K |
| `frame_rate` | `30` | `60` for 60 fps in matches (**experimental**; use with `resolution_scale = 1`) |
| `present_effect` | `"bilinear"` | Upscaling: `bilinear`, `cas` or `fsr` |
| `present_letterbox` | `true` | Keep 16:9 with black bars |
| `mnk_mode` | `true` | Keyboard plays as player 1 |

Delete the file to get a fresh one with the defaults. For one run, any setting
can also be given on the command line, as `--name=value`.

Everything the game writes stays in its folder:

```
game/      the extracted game files
config/    thebigs2.toml (settings), launcher preferences
saves/     profile, save games, achievements
cache/     shader cache
logs/
```

If the folder can't be written to, the game uses `~/.config/thebigs2` and
`~/.local/share/thebigs2` (Linux) or `%APPDATA%\thebigs2` (Windows) instead.

**Requirements:** a GPU with Vulkan support and a current driver. Linux needs
glibc 2.38 or newer (Ubuntu 24.04, Fedora 39, current Arch or SteamOS).
Windows needs 64-bit Windows 10 or 11.

## Steam Deck

Use `TheBigs2-SteamDeck-x86_64.AppImage` from the Linux download. It's the same
game with defaults suited to the Deck: 1x resolution (fills the screen), lighter
settings, and the keyboard turned off as a controller (Steam Input sends key
presses).

1. In Desktop Mode, make a folder (e.g. `~/Games/TheBigs2`) and put the
   AppImage in it, ideally with your `.iso`.
2. *Add a Non-Steam Game to My Library*, pick the AppImage, switch to Game
   Mode and play. The setup screen works with the Deck's controls and
   touchscreen and can browse to an SD card.
3. Quit with **View + Menu held 2 seconds**, or EXIT GAME on the main menu.
   **View + Y held a second** opens the settings menu.
4. On the Deck OLED, set the refresh rate to **60 Hz** in the Quick Access
   performance menu, or motion judders.

It runs at 30 fps. With `frame_rate = 60` it holds 60 in gameplay and slows
down in transitions. The first minutes can stutter while shaders compile.

## Troubleshooting

- After a crash or a black screen, check `logs/`. An old settings file from
  an earlier build can cause trouble; deleting it resets everything.
- For smooth motion, use a 60 Hz or 120 Hz display mode, or VRR / G-Sync.
- Online: if the friend never reaches the Game Room, check the host's IP, that
  both are on Tailscale (or that the ports are forwarded), and that Windows was
  allowed to let the game through its firewall.

## For developers

- **[docs/BUILDING.md](docs/BUILDING.md)**: build the SDK and the game, package
  the releases (Linux, Windows cross-build, macOS).
- **[docs/ONLINE.md](docs/ONLINE.md)**: how the online mode works, the debug
  flags, and how to test two copies on one PC.
- The online build needs the SDK fork
  [MatRanc/rexglue-sdk](https://github.com/MatRanc/rexglue-sdk) (branch
  `tb2-online`).

## Credits

- **Michael (mdomangue26)**: [The Bigs 2 Recompiled](https://github.com/mdomangue26/The-Bigs-2-Recompiled),
  the PC port this fork builds on, and the SDK fixes it needs.
- The **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)** authors and
  contributors.
- [sv-ttk](https://github.com/rdbende/Sun-Valley-ttk-theme) for the launcher's
  theme (MIT).

## License

The port's own code is public domain ([Unlicense](LICENSE)). The game itself,
the fonts and the SDK keep their own terms; see [LICENSE](LICENSE).
