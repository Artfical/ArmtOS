#!/usr/bin/env python3
"""A minimal WebSocket echo server (RFC 6455) for the emulator tests: stdlib only."""
import base64, hashlib, socket, struct, sys, threading

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

def recv_exact(c, n):
    b = b""
    while len(b) < n:
        d = c.recv(n - len(b))
        if not d:
            raise EOFError
        b += d
    return b

def handle(c):
    try:
        req = b""
        while b"\r\n\r\n" not in req:
            d = c.recv(4096)
            if not d:
                return
            req += d
        key = [l.split(b":", 1)[1].strip() for l in req.split(b"\r\n") if l.lower().startswith(b"sec-websocket-key")][0]
        acc = base64.b64encode(hashlib.sha1(key + GUID.encode()).digest())
        c.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + acc + b"\r\n\r\n")
        while True:
            h = recv_exact(c, 2)
            op, ln = h[0] & 0x0F, h[1] & 0x7F
            if ln == 126: ln = struct.unpack(">H", recv_exact(c, 2))[0]
            elif ln == 127: ln = struct.unpack(">Q", recv_exact(c, 8))[0]
            mask = recv_exact(c, 4) if h[1] & 0x80 else b"\0\0\0\0"
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(recv_exact(c, ln)))
            if op == 8:
                c.sendall(b"\x88\x00"); return
            if op == 9:
                c.sendall(b"\x8a" + bytes([len(data)]) + data); continue
            out = bytes([0x80 | op])
            out += bytes([len(data)]) if len(data) < 126 else b"\x7e" + struct.pack(">H", len(data))
            c.sendall(out + data)
    except Exception:
        pass
    finally:
        c.close()

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", int(sys.argv[1]) if len(sys.argv) > 1 else 18080))
s.listen(8)
while True:
    c, _ = s.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()
