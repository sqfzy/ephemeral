"""Minimal RFC 6455 WebSocket server helpers (pure stdlib).

Only the pieces the bench mocks need:
  - accept_handshake(conn): read the HTTP upgrade request and reply 101.
  - decode_frame(conn):     read one client->server frame (always masked).
  - encode_frame(op, pl):   encode one server->client frame (never masked).

These are *not* a general-purpose implementation. No fragmentation,
no extensions, no permessage-deflate. That's intentional — the bench
client uses the same framing subset.
"""

import base64
import hashlib
import socket
import struct
from typing import Optional, Tuple

OPCODE_CONTINUATION = 0x0
OPCODE_TEXT = 0x1
OPCODE_BINARY = 0x2
OPCODE_CLOSE = 0x8
OPCODE_PING = 0x9
OPCODE_PONG = 0xA

_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def _recv_exact(conn: socket.socket, n: int) -> Optional[bytes]:
    """Read exactly n bytes or return None on EOF."""
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def compute_accept(key: str) -> str:
    """RFC 6455 §4.2.2: Sec-WebSocket-Accept = base64(sha1(key + GUID))."""
    h = hashlib.sha1((key + _GUID).encode("ascii")).digest()
    return base64.b64encode(h).decode("ascii")


def accept_handshake(conn: socket.socket) -> None:
    """Read HTTP upgrade request, send 101 Switching Protocols response.

    Raises ValueError if the request is not a well-formed WS upgrade.
    """
    # Read until end of HTTP headers. Requests are tiny; 8 KiB is plenty.
    buf = bytearray()
    while b"\r\n\r\n" not in buf:
        chunk = conn.recv(4096)
        if not chunk:
            raise ValueError("peer closed during handshake")
        buf.extend(chunk)
        if len(buf) > 65536:
            raise ValueError("handshake request too large")

    header_text = buf.split(b"\r\n\r\n", 1)[0].decode("iso-8859-1")
    lines = header_text.split("\r\n")
    key: Optional[str] = None
    for line in lines[1:]:
        name, sep, value = line.partition(":")
        if sep and name.strip().lower() == "sec-websocket-key":
            key = value.strip()
            break
    if key is None:
        raise ValueError("missing Sec-WebSocket-Key header")

    accept = compute_accept(key)
    response = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n"
        "\r\n"
    )
    conn.sendall(response.encode("ascii"))


def decode_frame(conn: socket.socket) -> Optional[Tuple[int, bytes]]:
    """Read one WebSocket frame from conn.

    Returns (opcode, payload) or None on EOF. Client->server frames
    are required to be masked per RFC 6455 §5.3; we enforce that.
    """
    header = _recv_exact(conn, 2)
    if header is None:
        return None
    b1, b2 = header[0], header[1]
    opcode = b1 & 0x0F
    masked = (b2 & 0x80) != 0
    length = b2 & 0x7F

    if length == 126:
        ext = _recv_exact(conn, 2)
        if ext is None:
            return None
        length = struct.unpack("!H", ext)[0]
    elif length == 127:
        ext = _recv_exact(conn, 8)
        if ext is None:
            return None
        length = struct.unpack("!Q", ext)[0]

    if not masked:
        # RFC 6455: client MUST mask. Bail rather than silently accepting.
        raise ValueError("client frame missing mask bit")

    mask = _recv_exact(conn, 4)
    if mask is None:
        return None
    payload = _recv_exact(conn, length) if length else b""
    if payload is None:
        return None

    if length:
        unmasked = bytearray(length)
        for i in range(length):
            unmasked[i] = payload[i] ^ mask[i & 3]
        payload = bytes(unmasked)
    return opcode, payload


def encode_frame(opcode: int, payload: bytes) -> bytes:
    """Encode a server->client WS frame. FIN=1, never masked."""
    n = len(payload)
    header = bytearray()
    header.append(0x80 | (opcode & 0x0F))
    if n < 126:
        header.append(n)
    elif n < (1 << 16):
        header.append(126)
        header += struct.pack("!H", n)
    else:
        header.append(127)
        header += struct.pack("!Q", n)
    return bytes(header) + payload
