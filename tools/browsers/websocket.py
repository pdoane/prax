"""A minimal WebSocket client (RFC 6455) in the standard library, for driving a browser over WebDriver BiDi.

It connects to a `ws://` URL without TLS, sends text messages in masked frames, as a client must, and receives text
messages, reassembling fragmented ones. It answers a ping with a pong and treats a close frame as the end of the
connection. Binary messages and extensions are not supported, because a browser's BiDi endpoint sends neither.

Usage:
    connection = websocket.connect("ws://127.0.0.1:9222/session")
    connection.send('{"id": 1, "method": "session.status", "params": {}}')
    reply = connection.receive()
    connection.close()
"""

import base64
import hashlib
import os
import socket
import struct
import urllib.parse

# The GUID that the server appends to the key before hashing it for its handshake answer, from RFC 6455 §1.3.
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# Opcodes of the frames that the client reads or writes.
CONTINUATION, TEXT, BINARY, CLOSE, PING, PONG = 0x0, 0x1, 0x2, 0x8, 0x9, 0xA


class Error(Exception):
    """A handshake that the server refused, or a connection that closed or broke the protocol."""


class Connection:
    """An open connection over `sock`, whose handshake has completed."""

    def __init__(self, sock):
        self.sock = sock
        self.buffer = b""

    def send(self, text):
        """Sends `text` as one masked text frame."""
        self.write_frame(TEXT, text.encode("utf-8"))

    def receive(self):
        """Returns the next text message, reassembled from its frames. Raises Error once the server closes."""
        message = b""
        while True:
            final, opcode, payload = self.read_frame()
            if opcode == PING:
                self.write_frame(PONG, payload)
            elif opcode == CLOSE:
                raise Error("the server closed the connection")
            elif opcode in (TEXT, CONTINUATION):
                message += payload
                if final:
                    return message.decode("utf-8")
            elif opcode != PONG:
                raise Error("an unexpected frame of opcode %d" % opcode)

    def close(self):
        """Sends a close frame and closes the socket, without waiting for the server's answer."""
        try:
            self.write_frame(CLOSE, struct.pack("!H", 1000))
        except OSError:
            pass
        self.sock.close()

    def write_frame(self, opcode, payload):
        header = bytes([0x80 | opcode])
        if len(payload) < 126:
            header += bytes([0x80 | len(payload)])
        elif len(payload) < 1 << 16:
            header += bytes([0x80 | 126]) + struct.pack("!H", len(payload))
        else:
            header += bytes([0x80 | 127]) + struct.pack("!Q", len(payload))
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def read_frame(self):
        """Returns the next frame as (final, opcode, payload). A server's frames are not masked, but a masked frame is
        unmasked."""
        first, second = self.read(2)
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", self.read(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self.read(8))[0]
        mask = self.read(4) if second & 0x80 else None
        payload = self.read(length)
        if mask is not None:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return bool(first & 0x80), first & 0x0F, payload

    def read(self, count):
        """Returns the next `count` bytes from the socket."""
        while len(self.buffer) < count:
            chunk = self.sock.recv(max(65536, count - len(self.buffer)))
            if not chunk:
                raise Error("the connection closed")
            self.buffer += chunk
        data, self.buffer = self.buffer[:count], self.buffer[count:]
        return data


def connect(url, timeout=60):
    """Opens a connection to `url`, a `ws://` URL, and completes the handshake. Each read and write waits at most
    `timeout` seconds."""
    parts = urllib.parse.urlsplit(url)
    if parts.scheme != "ws":
        raise Error("not a ws:// URL: %s" % url)
    sock = socket.create_connection((parts.hostname, parts.port or 80), timeout)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = ("GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n") % (parts.path or "/", parts.netloc, key)
    sock.sendall(request.encode("ascii"))
    connection = Connection(sock)
    response = b""
    while b"\r\n\r\n" not in response:
        chunk = sock.recv(4096)
        if not chunk:
            raise Error("the connection closed during the handshake")
        response += chunk
    head, connection.buffer = response.split(b"\r\n\r\n", 1)
    lines = head.decode("latin-1").split("\r\n")
    if lines[0].split(" ")[1:2] != ["101"]:
        raise Error("the server refused the handshake: %s" % lines[0])
    headers = {k.strip().lower(): v.strip() for k, _, v in (line.partition(":") for line in lines[1:])}
    accept = base64.b64encode(hashlib.sha1((key + GUID).encode("ascii")).digest()).decode("ascii")
    if headers.get("sec-websocket-accept") != accept:
        raise Error("the server's handshake answer does not match its key")
    return connection
