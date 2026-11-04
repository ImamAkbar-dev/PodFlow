import socket, struct, time

s = socket.create_connection(("127.0.0.1", 8888))
name = b"sample_5m.wav"
s.sendall(struct.pack("!HBBIII", 0x5753, 1, 0x04, 1, len(name), 0) + name)
print("PLAY terkirim, klien tidur 60 detik tanpa membaca...")
time.sleep(60)
