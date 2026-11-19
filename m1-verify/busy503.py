#!/usr/bin/env python3
# busy503.py - check --max-clients 3 -> ERROR 503 + close (Member 1 scratch).
# Start the server first:  ./server 8888 --max-clients 3
import socket, struct, time

def hello(s):
    s.sendall(struct.pack("!HBBIII", 0x5753, 1, 1, 0, 0, 0))
    m, v, t, seq, pl, par = struct.unpack("!HBBIII", s.recv(16))
    return t, (s.recv(pl) if pl else b"")

keep = []
for i in range(3):
    s = socket.create_connection(("127.0.0.1", 8888))
    print("client", i + 1, "HELLO ->", hello(s))
    keep.append(s)

s = socket.create_connection(("127.0.0.1", 8888))
m, v, t, seq, pl, par = struct.unpack("!HBBIII", s.recv(16))
body = s.recv(pl) if pl else b""
print("client 4 -> type=0x%02X param=%d msg=%r" % (t, par, body))
print("then EOF:", s.recv(1) == b"")

for k in keep:
    k.close()
time.sleep(0.5)
s = socket.create_connection(("127.0.0.1", 8888))
print("after others left, HELLO ->", hello(s)[0])