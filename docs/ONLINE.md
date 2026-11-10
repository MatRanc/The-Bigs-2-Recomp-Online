# Online play: how it works and how to work on it

This is the developer guide to the online mode. Players only need the launcher's
**Online** tab (see the README). Everything here is written from observing the
game's behaviour and our own code; it contains no game code. Addresses are guest
addresses in the retail `default.xex` and match the names in `generated/`
(`sub_XXXXXXXX`), so you can find the same spot in your own recompiled output or
in a disassembler.

## The short version

*The Bigs 2* has no dedicated server. Two consoles find each other through Xbox
Live matchmaking, then play **deterministic lockstep**: each side sends only its
controller input every frame, both sides run the same simulation from the same
input and the same random seed, and so stay in sync. Team picks, menus and the
match itself all ride on that input stream.

Xbox Live is gone, so we replace the parts of it the game waits for:

| Live piece the game needs | What stands in for it |
| --- | --- |
| A signed-in Live account | `TB2_FAKE_ACCOUNT`: a synthetic account built by the game's own account code |
| Live profile / sign-in state (XAM) | SDK fork, `TB2_FAKE_LIVE` + `TB2_ACCOUNT_NAME` |
| Matchmaking ("Quick Match" search) | `TB2_FAKE_SEARCH` + `TB2_PEER_IP`: the joiner gets one search result pointing at the host |
| Hosting a session | `TB2_FAKE_HOST`: the host's Search turns into "create game" |
| Per-player stats (names in the Game Room) | SDK answers the stats lookup offline |
| Roster download server | `TB2_SKIP_ROSTER`: use the built-in roster |

Nothing else is emulated: the game's own peer-to-peer code does the rest.

## Network layout

| Port | Protocol | Used for |
| --- | --- | --- |
| 51000 | TCP | Session / Game Room traffic (join, participant table, settings, Ready). Host listens. |
| 1795 | UDP | **GameConn**, the lockstep input link, both directions |
| 1796 | UDP | GameConn of the second instance, only when both players have the same IP (one PC) |

Over Tailscale nothing needs forwarding. On a LAN or the internet the host must
accept TCP 51000 and UDP 1795; Windows asks to allow the game on first use.

The host connects GameConn to the address the joiner's TCP connection came from
(not the joiner's own LAN address), so NAT-free tunnels like Tailscale work.

## The flow, step by step

1. **Sign-in.** The game keeps a pending sign-in object that Live normally fills
   in. Our hook (`online_debug.cpp`, `TB2_FAKE_ACCOUNT`) fills it and raises the
   event that makes the game build the account itself, so the account has a
   valid vtable. The 16-byte player-id record and the sign-in XUID are set from
   the player name (see *Names*).
2. **Search / host.** Quick Match → Search. With `TB2_FAKE_SEARCH` the joiner
   gets one result for `TB2_PEER_IP:TB2_PEER_PORT`; with `TB2_FAKE_HOST` the
   host gets "No Game Rooms found" and creates a room (X).
3. **Game Room.** The joiner connects over TCP. The host's own code answers
   with its participant table, the session settings and the session id; both
   sides then show the Game Room. Ready (A) on both starts the match setup.
4. **Lockstep start.** Start-match (`822DC1D8`) calls the online object
   (global `825E7174`, init `8237AAA8`), which opens GameConn (`8241DDD8`,
   socket class ctor `8241D808`) towards the peer and waits for it to connect.
   Once connected it seeds the RNG from the host and switches input to
   lockstep (`8237A690`).
5. **Virtual pads.** In lockstep the input system skips the physical pads and
   creates virtual pad records (pad table `82633BF0`, slots 8-15) that are fed
   from the lockstep frames. That is why the opponent's controller appears in
   Team Setup only once GameConn is up.
6. **Team Setup → match.** Sides, teams, uniforms, pitcher, lineup and (host
   only) stadium are all just inputs; the match loads on both and stays in sync.

## Things that were hard to find

- **GameConn uses overlapped WSASend/WSARecv**, which the SDK did not log, so
  for a long time it looked like the game sent nothing after Ready.
- **One PC, two instances:** when both ends have the same IP the game puts the
  host on UDP 1795 and the joiner on 1796, but each side then connects to its
  *own* port number on the other end, so each talks to itself. Our hook on the
  connect wrapper (`823CDD10`) swaps 1795/1796 when the target is this machine.
  Two machines use 1795 on both ends and are left alone.
- **Names:** the Game Room draws the account XUID's 8 bytes as the player's
  name (a real Live XUID starts with a zero byte and would draw nothing). So the
  synthetic XUID *is* the name: up to 8 characters, and two players need names
  that differ in those 8.
- **Avatars:** with no gamer pictures, the picture fetch (`8230E510`) left the
  remote slot as a white square; we skip it so both slots show "?".
- **Floats:** lockstep needs bit-identical results on every machine. The game
  targets are built with `-ffp-contract=off` (clang fuses `a*b+c` by default on
  arm64). See `docs/HALL_OF_SHAME_TB2_SYNC.md` for the approach that did *not*
  work before the lockstep was found.

## Environment variables

Online (set by the launcher, see `online_env()` in `thebigs2_gui.py`):

| Variable | Who | Meaning |
| --- | --- | --- |
| `TB2_FAKE_LIVE=1` | both | SDK: signed-in Live profile, LAN IP as the title address |
| `TB2_FAKE_ACCOUNT=1` | both | Game: synthetic Live account and the Game Room fixes |
| `TB2_SKIP_ROSTER=1` | both | Use the built-in roster (the roster server is gone) |
| `TB2_SESSION_TICK=1` | both | Keep the session alive with the synthetic account (health check uses user 0) and advance the joiner past the step that would join a real Live XSession |
| `TB2_ACCOUNT_NAME` | both | Player name (max 8 characters) |
| `TB2_FAKE_HOST=1`, `TB2_HOST_PORT=51000` | host | Host the Game Room |
| `TB2_FAKE_SEARCH=1`, `TB2_PEER_IP`, `TB2_PEER_PORT=51000` | joiner | Join the host |

Debugging: `TB2_ONLINE_DEBUG`, `TB2_NET_LOG` (SDK socket/XNet calls),
`TB2_UI_TRACE` (UI screens and messages), `TB2_CTRL_TRACE` (pads, Team Setup),
`TB2_XMSG_LOG` (XAM app calls), `TB2_FN_TRACE=1` with a build configured
`-DTB2_FN_TRACE=ON -DTB2_FN_TRACE_RANGE=lo-hi` (logs which guest functions run
per key press), `--fps_log_interval=5` (frame rate and per-thread CPU).

## Testing on one PC

```bash
tools/to_gameroom.sh   # two instances (User1 hosts, User2 joins), both in the Game Room
tools/to_match.sh      # ... through Team Setup to the first at-bat
DEBUG=0 tools/to_match.sh                       # only the launcher's env, no trace flags
GAME_ARGS=--fps_log_interval=5 tools/to_match.sh
tools/shot.sh tag      # screenshot every game window to /tmp/tag_<pid>.png
```

`tools/run_pair.sh` starts the pair with the launcher's env. Keys are injected
through `TB2_AUTOKEY` files (`/tmp/host.keys`, `/tmp/guest.keys`; lines like
`tap space`, `tap shift+Left`, `wait 1500`; space = A, l = X, BackSpace = B,
shift+arrows = d-pad). Logs are in `out/build/linux-amd64-release/logs/`.
Two instances share one GPU, so expect ~22 fps each at resolution scale 2.
The scripts use fixed waits and sometimes press during a screen change; check
the screenshots.

## SDK changes

The game needs the SDK fork **[MatRanc/rexglue-sdk](https://github.com/MatRanc/rexglue-sdk)**,
branch `tb2-online` (Michael's `fix/stability` plus the online commit):
WinSock ioctl/socket-option translation on Linux/macOS, non-blocking accepted
sockets, `XNetXnAddrToInAddr`, Live startup notifications, a Windows LAN-IP
lookup, the fake Live profile and the offline stats lookup.

## Open items

- Tested across machines: a Linux host and an Apple Silicon Mac (same home
  network) met in the Game Room and played a match. Needed two macOS fixes:
  hooks must add the 0xE0000000 host offset (`src/guest_memory.h`; Windows has
  the same offset), and the SDK must convert socket addresses to the BSD
  `sockaddr` layout (otherwise the UDP lockstep `connect` fails with
  WSAEWOULDBLOCK and the match never starts). A long Mac-vs-PC match has not
  been checked for desync yet.
- macOS is experimental: spiked/stretched player models and a floating bat
  (likely the Metal/MoltenVK path; the unpacked game data matches Linux), and
  slower matches. Leads: upstream issue rexglue/rexglue-sdk#446 (a Fable II
  port switched to XeniOS's native Metal backend).
- Windows: built and packaged, not yet run on a Windows PC.
- `TB2_NET_LOG=1` logs a per-second summary of the lockstep link
  (WSASend/WSARecv); about 45 packets/s each way in a running match.
- The host's game crash-loops (guest access violations) if a match fails to
  start and the session times out; harmless otherwise, still to fix.
- The game's desync path (`8237A950` ends the session) has not been seen in
  testing (two full innings so far); a long match is still worth playing.
- `online_debug.cpp` mixes the needed hooks with debug probes; split it before
  proposing it upstream.
- Launcher settings reach the game only in portable setups (AppImage, Windows
  zip); online play works everywhere.
