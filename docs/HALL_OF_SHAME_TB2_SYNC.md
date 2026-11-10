# Hall of Shame: the TB2_SYNC side channel (session 11)

*Written 2026-11-09, after session 12. The code described here was deleted the same day.*

## TL;DR

In session 11 we decided The Bigs 2 had no way to sync two players. So we built our own: a UDP side channel
(`TB2_SYNC`) plus a pile of fake events and forced flags. With all of it we limped as far as Game Setup, and then the
match died with "Exiting match" every time.

The game had a complete deterministic lockstep engine the whole time. It never ran because on one PC both windows
connected to **their own** UDP port. A 20-line port-swap hook (`TB2_GAMECONN_SWAP`) fixed it, and the side channel,
the fake roster step, the forced Ready and the pad hacks all became unnecessary at once.

## The inductees

| Flag / hack | What it did | What it achieved | Verdict |
| --- | --- | --- | --- |
| `TB2_SYNC` | Our own UDP socket, host :51100, guest :51101, applied on the game thread from the Team Setup Update hook | Carried the other flags' packets | A second network stack bolted onto a game that already had one |
| `TB2_SYNC_TEAMS` | Sent ready (`822F98E8`) and team (`8223FFB0`) picks; resent our own team every 250 ms; poked the list widget via `824AD228` | Same teams on both windows, shown in swapped order, with different rank numbers | Reimplemented, badly, what lockstep input does for free |
| `8223FFB0` lr filter | Ignored the game re-setting a team to id 99 from our fake roster | Stopped both panels showing the same team | A hack to fix the hack's side effects |
| `TB2_FAKE_ROSTER_DONE` | Enqueued event 0x33 via `823EC170` 2 s after the peer's roster, retried 4 times | Event delivered and consumed; the dialog stayed up | Shouted into the void, four times |
| `TB2_ROSTER_STEP` | Forced the roster state machine `82376420` (obj+936=0, +940=3) to post the "exchange done" UI message | Got past "Exchanging Roster..." | Teleported past a door instead of finding the key |
| `TB2_FORCE_SIDE_READY` | Forced `8230FA80` ("side ready or empty") to 1 | Nothing; the 50 s timer still exited the match | Did not even fail interestingly |
| `TB2_INPUT_MIRROR` | Sent raw XINPUT_STATE over UDP; shallow-copied pad 0's record (0x4000 bytes) into pad slot 1 | Guest never saw the remote pad; both team lists showed the same team (shared inner pointers) | Doc said "did not work, do not use", and it was right |
| `TB2_REMOTE_ICON` | Wrote "pad 1 joined" into the Team Setup screen bytes | No visible effect | Painted a controller on the wall |
| Unfocused-input hack (`824DA080`) | Neutral input for the unfocused window | Stopped one physical pad driving both windows | Only needed because of testing on one PC |
| `8230FAE8` side-controller count | Claimed each side had a controller | Hid the "opponent re-connecting controller" dialog | Silenced the one message telling us the truth |
| `823355A8` may-move | Always allowed moving a pad onto a side | Pads could move | The check failed because the virtual pads never existed |
| `822B8460` pad connected | Marked pad 0 (and the clone) connected | Controller icon lit | Same |
| `82379C98` id 17 | Faked the OnlineMatchSetup request | Got Team Setup to open | The real one arrives by itself once GameConn connects |
| Field 9 (`823E3B18`) | Forced settings field 9 to 1 during Ready | Unclear | An "experiment" that stayed in for a session |

All of the above was removed in session 12. Run 280/281 played from Team Setup through the first pitch with none
of it, with 0 errors.

## What was actually going on

- **The claim that sank us:** "NO code sends subtype 2, so the game has NO native message for team pick / roster in
  Team Setup; sync must be our own." True about the Live game messages, and irrelevant. The game does not send team
  picks at all. It sends **controller input** every frame over a separate link ("GameConn"), and both machines run the
  same menus from the same input and the same RNG seed. Team Setup is just the first screen of the lockstep.
- **The second bad claim:** "Does the number of machines matter? No. These walls are game logic." The one real bug only
  happens on one machine. With local IP == remote IP the game splits ports (host 1795, guest 1796) but each side then
  connects to its own port number on the peer. The guest handshook with itself and the host heard nothing.
- **Why we never saw it:** GameConn uses overlapped `WSASend`/`WSARecv`, and the SDK implements those without `[net]`
  logging. "Nothing is exchanged on the wire" was really "nothing we were logging".
- **The symptoms we hacked one by one** (missing peer controller, may-move failing, the re-connecting dialog, the 50 s
  exit, OnlineMatchSetup never queued) were all the same thing: the lockstep link never came up, so the 8 virtual pads
  (slots 8-15 of the pad table 0x82633BF0) were never created.

## Lessons

1. When a networked game seems to have "no message" for something, look for lockstep input before building a sync
   protocol. Sports games of that era usually sync input plus an RNG seed, not game state.
2. Check that the logging covers every socket API the game imports before concluding "nothing is sent". Here that
   meant the overlapped WSA calls.
3. Be suspicious of testing on one PC when ports are involved. Both instances share an IP, and the game had special
   handling for exactly that case.
4. When the tenth hack is needed to silence the ninth hack's side effects, stop and trace instead. The function trace
   over 0x82370000-0x82430000 found the real cause in one run.
5. A dialog saying "opponent re-connecting controller" is information, not an obstacle.
