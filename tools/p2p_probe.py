#!/usr/bin/env python3
"""Host simulator + live guest-memory probe for The Bigs 2 session debugging.

Acts as the P2P host (replies opcode 10 to the client's participant table and
sends heartbeats) and, while connected, periodically dumps the game's OP-session
state from /proc/<pid>/mem. Run it in the foreground; launch the game and press
Search while it listens.
"""
import argparse
import glob
import os
import select
import socket
import struct
import threading
import time

BASE = 0x100000000


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


def build_packet(opcode, payload=b""):
    pkt = struct.pack(">IB7s", opcode, len(payload), b"\x00" * 7) + payload
    return struct.pack(">I", 4 + len(pkt)) + pkt


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        c = sock.recv(n - len(buf))
        if not c:
            return None
        buf.extend(c)
    return bytes(buf)


def find_pid():
    best = None
    for p in glob.glob("/proc/[0-9]*/comm"):
        try:
            if open(p).read().strip() == "thebigs2":
                pid = int(p.split("/")[2])
                best = pid
        except OSError:
            pass
    return best


def dump_mem(stop):
    while not stop.is_set():
        pid = find_pid()
        if pid:
            try:
                mem = open(f"/proc/{pid}/mem", "rb", 0)

                def u32(a):
                    mem.seek(BASE + a)
                    b = mem.read(4)
                    return struct.unpack(">I", b)[0] if len(b) == 4 else None

                s = u32(0x825F5144)
                if s:
                    state = u32(s + 0xC0)
                    mode = u32(s + 0x5C)
                    arm = u32(s + 0x58)
                    term = u32(s + 0x18)
                    part = u32(s + 0x4C)
                    psize = u32(part + 8) if part else None
                    transport = u32(s + 0x60)
                    log(f"MEM session=0x{s:08X} state={state} mode={mode} arm={arm} "
                        f"term24={term & 0xFF} partlist=0x{part:08X} size={psize} "
                        f"transport=0x{transport:08X}")
                mem.close()
            except (OSError, ValueError):
                pass
        stop.wait(2.0)


def serve(sock, stop):
    data = b""
    last_hb = time.time()
    sock.setblocking(False)
    while not stop.is_set():
        now = time.time()
        if now - last_hb >= 3.0:
            try:
                sock.sendall(build_packet(11))
                last_hb = now
            except OSError:
                break
        r, _, x = select.select([sock], [], [sock], 0.1)
        if x:
            break
        if not r:
            continue
        try:
            c = sock.recv(4096)
        except BlockingIOError:
            continue
        if not c:
            log("peer closed")
            break
        data += c
        while len(data) >= 4:
            total = struct.unpack(">I", data[:4])[0]
            if total < 4 or total > 65536:
                data = b""
                break
            if len(data) < total:
                break
            body = data[4:total]
            data = data[total:]
            op = struct.unpack(">I", body[:4])[0]
            log(f"RX opcode {op} total={total}")
            if op == 8:
                log("TX opcode 10 (208 zero bytes)")
                sock.sendall(build_packet(10, b"\x00" * 208))
    stop.set()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=51000)
    ap.add_argument("--seconds", type=float, default=120)
    a = ap.parse_args()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", a.port))
    srv.listen(1)
    log(f"listening on 127.0.0.1:{a.port}")
    stop = threading.Event()
    threading.Thread(target=dump_mem, args=(stop,), daemon=True).start()
    end = time.time() + a.seconds
    srv.settimeout(1.0)
    while time.time() < end and not stop.is_set():
        try:
            c, addr = srv.accept()
        except socket.timeout:
            continue
        log(f"accepted {addr[0]}:{addr[1]}")
        serve(c, stop)
        c.close()
    stop.set()
    srv.close()
    log("done")


if __name__ == "__main__":
    main()
