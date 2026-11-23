#!/usr/bin/env python3
# ctl.py - raw-socket control check of the state machine (Member 1 scratch).
# Usage: python3 ~/m1-verify/ctl.py   (server already running on 127.0.0.1:8888)
import socket, struct, time

def fr(t, seq=0, param=0, pl=b""):
    return struct.pack("!HBBIII", 0x5753, 1, t, seq, len(pl), param) + pl

def rx(s):
    h = b""
    while len(h) < 16:
        d = s.recv(16 - len(h))
        if not d:
            return None
        h += d
    m, v, t, seq, pl, par = struct.unpack("!HBBIII", h)
    b = b""
    while len(b) < pl:
        b += s.recv(pl - len(b))
    return t, seq, par, b

def until(s, typ, maxn=400):
    """Skip frames (stale DATA) until one of type typ arrives."""
    for _ in range(maxn):
        r = rx(s)
        if r is None:
            return None
        if r[0] == typ:
            return r
    return None

ACK, ERR, DATA = 10, 0xFF, 8
s = socket.create_connection(("127.0.0.1", 8888))
s.settimeout(10)
s.sendall(fr(1))
assert rx(s)[0] == 2                                   # WELCOME
log = []
s.sendall(fr(4, 0, 0, b"sample_30s.wav")); r = until(s, ACK, 10); log.append(("PLAY -> ACK", r[2]))
time.sleep(1.0)
s.sendall(fr(5));           r = until(s, ACK, 400);    log.append(("PAUSE -> ACK next seq", r[2]))
s.sendall(fr(5));           r = until(s, ERR, 400);    log.append(("PAUSE again -> ERROR", r[2]))
s.sendall(fr(6, 0, 300));   r = until(s, ACK, 400);    log.append(("SEEK 300 (paused) -> ACK", r[2]))
s.sendall(fr(4));           r = until(s, ACK, 400);    log.append(("resume -> ACK next seq", r[2]))
d = until(s, DATA, 8);                                  log.append(("first DATA seq after resume", d[1]))
s.sendall(fr(6, 0, 999999)); r = until(s, ERR, 400);   log.append(("SEEK 999999 -> ERROR", r[2]))
time.sleep(0.5)
s.sendall(fr(7));           r = until(s, ACK, 400);    log.append(("STOP -> ACK last seq", r[2]))
s.sendall(fr(7));           r = until(s, ERR, 400);    log.append(("STOP in IDLE -> ERROR", r[2]))
for name, val in log:
    print(f"{name:32s} {val}")