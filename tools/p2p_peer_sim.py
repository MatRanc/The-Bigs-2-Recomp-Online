#!/usr/bin/env python3
"""
The Bigs 2 — P2P Mock Peer Simulator (wire format v2)

Captured from the real game (client handshake):

    [ uint32 BE total_len ][ uint32 BE opcode ][ u8 payload_len ][ 7 pad ][ payload ]

`total_len` counts itself + the 12-byte header + payload (so an empty packet is
16 bytes). The 7 padding bytes are uninitialised by the game and ignored by the
receiver.

Handshake (client = joiner, host = the side that listens):
  * client sends opcode 8 (208-byte participant table) and opcode 9/11.
  * receiving opcode 9 makes the client reply opcode 10.
  * receiving opcode 10 creates the remote participant and queues MATCH_START
    (event 0x14), advancing the UI into the match.

To drive a game acting as the joiner, reply to its opcode 8 with opcode 10.

Usage:
  # Simulate the host (game runs as client via TB2_FAKE_SEARCH=1):
  python3 tools/p2p_peer_sim.py --mode server --port 51000 -v

  # Raw capture: dump every byte/decoded packet, never reply:
  python3 tools/p2p_peer_sim.py --mode server --port 51000 --capture
"""

import argparse
import os
import select
import socket
import struct
import sys
import time

HEADER_LEN = 12

OPCODE_NAMES = {
    1: "STATE_UPDATE",
    2: "LEAVE",
    3: "SETTINGS",
    4: "ROSTER_SELECT",
    5: "PARAMS_REQUEST",
    6: "PARAMS_ACK",
    7: "TEAM_VALIDATE",
    8: "PARTICIPANT_TABLE",
    9: "HANDSHAKE",
    10: "HANDSHAKE_ACK",
    11: "HEARTBEAT",
    20: "MATCH_START",
}


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def hex_dump(data, prefix="  "):
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hex_str = " ".join(f"{b:02X}" for b in chunk)
        ascii_str = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{prefix}{i:04X}:  {hex_str:<48}  |{ascii_str}|")
    return "\n".join(lines)


def build_packet(opcode, payload=b""):
    pkt = struct.pack(">IB7s", opcode, len(payload), b"\x00" * 7) + payload
    return struct.pack(">I", 4 + len(pkt)) + pkt


def decode_packet(pkt):
    opcode = struct.unpack(">I", pkt[0:4])[0]
    payload_len = pkt[4]
    payload = pkt[HEADER_LEN:HEADER_LEN + payload_len]
    return opcode, payload


def parse_participant_table(payload):
    if len(payload) < 8:
        return {"raw_len": len(payload)}
    host_slot, count = struct.unpack(">II", payload[0:8])
    # Best-effort record walk: records are 48 bytes from payload+8.
    records = []
    off = 8
    while off + 48 <= len(payload) and len(records) < max(count, 0) + 1:
        rec = payload[off:off + 48]
        xuid = struct.unpack(">Q", rec[4:12])[0] if len(rec) >= 12 else 0
        tag = rec[20:40].split(b"\x00", 1)[0].decode("ascii", "replace") if len(rec) >= 40 else ""
        records.append({"xuid": f"0x{xuid:016X}", "tag": tag})
        off += 48
    return {"host_slot": host_slot, "count": count, "records": records, "raw_len": len(payload)}


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def pump(sock, mode, heartbeat_interval, verbose, capture, reply_opcode, local_tag):
    sock.setblocking(True)
    last_heartbeat = time.time()
    tx_handshake = (mode == "client")

    try:
        while True:
            if tx_handshake:
                # Same shape the real game sends: [id 0xFF][count 1][entry: 16-byte player-id
                # record (XUID first) ... 0x10: XUID prefix], unused slots marked 0xFF.
                xuid = int(os.environ.get("TB2_SIM_XUID", "0xB13EBABEBABEBAB3"), 0)
                t = bytearray(208)
                t[0:4] = struct.pack(">I", 0xFF)
                t[4:8] = struct.pack(">I", 1)
                t[8:16] = struct.pack(">Q", xuid)
                t[0x18:0x20] = struct.pack(">Q", xuid)
                t[0x60:0x68] = b"\xFF" * 8
                t[0x90:0x98] = b"\xFF" * 8
                payload = bytes(t)
                pkt = build_packet(8, payload)
                log("[TX] opcode 8 (PARTICIPANT_TABLE)")
                if verbose:
                    print(hex_dump(pkt, prefix="  [TX] "))
                sock.sendall(pkt)
                tx_handshake = False

            timeout = max(0.05, heartbeat_interval - (time.time() - last_heartbeat))
            rlist, _, xlist = select.select([sock], [], [sock], timeout)
            if xlist:
                log("socket exception")
                break
            if not rlist:
                if time.time() - last_heartbeat >= heartbeat_interval:
                    pkt = build_packet(11)
                    log("[TX] opcode 11 (HEARTBEAT)")
                    sock.sendall(pkt)
                    last_heartbeat = time.time()
                continue

            hdr = sock.recv(4)
            if not hdr:
                log("peer closed (EOF)")
                break
            if len(hdr) < 4:
                rest = recv_exact(sock, 4 - len(hdr))
                if rest is None:
                    log("peer closed mid-length")
                    break
                hdr += rest
            total = struct.unpack(">I", hdr)[0]
            if total < 4 or total > 65536:
                log(f"bad frame length {total}; dumping remaining and stopping")
                if capture:
                    data = sock.recv(4096)
                    print(hex_dump(hdr + data, prefix="  [RX] "))
                break
            body = recv_exact(sock, total - 4)
            if body is None:
                log("peer closed mid-frame")
                break
            if len(body) < HEADER_LEN:
                log(f"short packet ({len(body)} bytes)")
                print(hex_dump(hdr + body, prefix="  [RX] "))
                continue
            opcode, payload = decode_packet(body)
            name = OPCODE_NAMES.get(opcode, f"UNKNOWN_{opcode}")
            log(f"[RX] opcode {opcode} ({name}) payload_len={len(payload)} total={total}")
            if verbose or capture:
                print(hex_dump(hdr + body, prefix="  [RX] "))
            if opcode == 8:
                info = parse_participant_table(payload)
                log(f"     participant table: {info}")
            if capture:
                continue
            if opcode == 8 and not capture:
                # The game arms +0x58=2 itself under TB2_SESSION_TICK, so just
                # answer the participant table with opcode 10 (empty) which
                # creates the remote participant and queues MATCH_START.
                # Send a zero-filled 208-byte payload: the game copies 296 bytes
                # into a scratch buffer and reads a count from payload+4, so an
                # empty packet makes it walk uninitialised memory.
                # The game parses this as the host's participant table
                # (sub_823F3878: [u32 id][u32 count][entries, stride 0x30]; entry
                # = 16-byte gamertag, u64 XUID at +0x10). All-zero gives a
                # participant with id 0 and the Gameroom then dereferences null.
                # Reuse the joiner's own captured table as the template.
                tbl = bytearray(payload.ljust(208, b"\x00")[:208])
                tbl[0:4] = struct.pack(">I", int(os.environ.get("TB2_SIM_HOSTID", "1")))
                tbl[4:8] = struct.pack(">I", 1)
                # Entry layout used by the game: first 16 bytes = player-id record whose first 8 bytes
                # are the XUID; names are resolved from the XUID by the stats lookup (the SDK
                # answers with TB2_PEER_NAME), so no gamertag goes on the wire.
                tbl[8:24] = struct.pack(">Q", 0xB13EBABEBABEBAB2) + b"\x00" * 8
                tbl[0x18:0x20] = struct.pack(">Q", 0xB13EBABEBABEBAB2)
                pkt = build_packet(10, bytes(tbl))
                log("[TX] opcode 10 (HANDSHAKE_ACK, host participant table) in reply")
                if verbose:
                    print(hex_dump(pkt, prefix="  [TX] "))
                sock.sendall(pkt)
                # Real host (sub_823F74E8) follows its accept with type 7
                # (44-byte session settings, optional here) and type 6 (8-byte
                # session id): the joiner's type-6 handler raises event 8,
                # which is what pushes the Gameroom screen.
                if os.environ.get("TB2_SIM_SETTINGS"):
                    # byte 12 of the struct is a flag the Game Room checks (P+0xE0): when set it requests
                    # the per-player stats records that fill names/slots. Default on.
                    settings = struct.pack(">IIIB", 2, 2, 0, int(os.environ.get("TB2_SIM_FLAG12", "1"))) + b"\x00" * 31
                    sock.sendall(build_packet(7, settings))
                    log("[TX] opcode 7 (SESSION_SETTINGS, 44 bytes)")
                # Room needs both players listed: send a participant table (type 3, same
                # parser as the type-10 table) for the host and one echoing the joiner's own
                # entry back, so the joiner sees itself as a second player.
                if not os.environ.get("TB2_SIM_NO_TABLES"):
                    host_tbl = bytes(tbl)
                    me = bytearray(payload.ljust(208, b"\x00")[:208])
                    me[0:4] = struct.pack(">I", int(os.environ.get("TB2_SIM_JOINERID", "2")))
                    me[4:8] = struct.pack(">I", 1)
                    sock.sendall(build_packet(3, host_tbl))
                    log("[TX] opcode 3 (host participant table)")
                    sock.sendall(build_packet(3, bytes(me)))
                    log("[TX] opcode 3 (joiner's own entry as participant)")
                sock.sendall(build_packet(6, struct.pack(">Q", 0x4249473200000001)))
                log("[TX] opcode 6 (SESSION_ID, event 8 -> Gameroom)")
            elif opcode in (1, 2) and mode == "client" and os.environ.get("TB2_SIM_START_ACK"):
                # Types 1 and 2 are the start handshake (receiving 1 sets a flag and raises event
                # 0x1B; receiving 2, only after 1, raises 0x1C). Experiment: acknowledge the host's
                # 1 with a 1, then send a 2.
                if opcode == 1:
                    sock.sendall(build_packet(1))
                    log("[TX] opcode 1 (start handshake ack)")
                    sock.sendall(build_packet(2))
                    log("[TX] opcode 2 (start handshake confirm)")
                else:
                    log("     host sent opcode 2 (start confirm)")
            elif opcode == 6 and mode == "client" and not os.environ.get("TB2_SIM_NO_READY"):
                # Joined: act like a second player who presses Ready first (before the host does).
                time.sleep(float(os.environ.get("TB2_SIM_READY_DELAY", "2")))
                sock.sendall(build_packet(16))
                log("[TX] opcode 16 (our Ready, sent first)")
            elif opcode == 16 and mode == "client" and not os.environ.get("TB2_SIM_NO_READY"):
                # Game message subtype 1 (empty) = "I am ready". The host sent it when its
                # player pressed Ready; answer with our own as the second player.
                sock.sendall(build_packet(16))
                log("[TX] opcode 16 (our Ready, answering the host's Ready)")
            elif opcode >= 15 and opcode != 19 or (opcode == 19 and not os.environ.get("TB2_SIM_ECHO19")):
                log(f"     game message subtype {opcode - 15} ({len(payload)} bytes), not answered")
            elif opcode == 19:
                # Subtype 4, empty: sent once by the joiner when the Game Room opens.
                # Experiment: echo it back as a real host's UI would acknowledge it.
                sock.sendall(build_packet(19))
                log("[TX] opcode 19 (echo of the Game Room entry message)")
            elif opcode == 9 and reply_opcode:
                pkt = build_packet(reply_opcode)
                log(f"[TX] opcode {reply_opcode} in reply to opcode 9")
                if verbose:
                    print(hex_dump(pkt, prefix="  [TX] "))
                sock.sendall(pkt)
            elif opcode == 10:
                log("     peer ACKed handshake (opcode 10) — should enter the match")
            elif opcode == 20:
                log("     peer signalled MATCH_START")
    except KeyboardInterrupt:
        log("interrupted")


def run_server(host, port, heartbeat, verbose, capture, reply_opcode, gamertag):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    log(f"[SERVER] listening on {host}:{port}")
    try:
        while True:
            c, addr = srv.accept()
            log(f"[SERVER] accepted {addr[0]}:{addr[1]}")
            pump(c, "server", heartbeat, verbose, capture, reply_opcode, gamertag)
            c.close()
            log("[SERVER] session ended")
    except KeyboardInterrupt:
        log("[SERVER] shutting down")
    finally:
        srv.close()


def run_client(host, port, heartbeat, verbose, capture, reply_opcode, gamertag):
    log(f"[CLIENT] connecting to {host}:{port}")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((host, port))
    log("[CLIENT] connected")
    try:
        pump(s, "client", heartbeat, verbose, capture, reply_opcode, gamertag)
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description="The Bigs 2 P2P mock peer")
    ap.add_argument("--mode", choices=["server", "client"], default="server")
    ap.add_argument("--host", default=None)
    ap.add_argument("--port", type=int, default=1000)
    ap.add_argument("--gamertag", default="PeerHost")
    ap.add_argument("--heartbeat", type=float, default=3.0)
    ap.add_argument("--reply-opcode", type=int, default=10,
                    help="opcode sent in reply to the peer's opcode 8/9 (default 10 = MATCH_START ack)")
    ap.add_argument("--capture", action="store_true", help="dump packets, never reply")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    tag = args.gamertag.encode("ascii", "replace")
    if args.mode == "server":
        run_server(args.host or "0.0.0.0", args.port, args.heartbeat, args.verbose,
                   args.capture, args.reply_opcode, tag)
    else:
        run_client(args.host or "127.0.0.1", args.port, args.heartbeat, args.verbose,
                   args.capture, args.reply_opcode, tag)


if __name__ == "__main__":
    main()
