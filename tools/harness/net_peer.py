"""The peer of the network's tests: TCP, TLS and HTTP servers that a test binary connects to.

`python3 -m tools.harness.net_peer <binary> [arguments]` makes certificates for this run with the openssl command,
starts each server on a port of its own on 127.0.0.1, and runs the binary with PR_NET_PEER set to a directory. The
directory holds `ports`, a line of a server's name and port for each server, and the trusted CA's certificate as
`ca.der`. The peer exits with the binary's status.

Each echo server echoes what it reads until the client asks it to end, by sending `<bye>` last, or ends itself. Then
the server writes how the exchange ended, `<eof:asked>`, `<eof:clean>`, or `<eof:ragged>` for a TLS client that closed
without close_notify, and ends cleanly itself, except where a server's purpose is to end otherwise.

The HTTP servers, over TCP and over TLS, answer the paths of Http.ROUTES as HTTP/1.1 and keep each connection open.
Each response says in X-Requests how many requests its connection has carried. /headers answers with the Origin and
Referer that the request sent, to any origin, and says in X-Host what its Host was. /cors answers with the CORS headers
that its query names, and redirects to the URL that its `to` names. A request to /cors that is not a preflight
receives its method and the last preflight's requested method and headers. /no-content-sent answers 204 with content
that it declares, and sends that content after a pause.

The WebSocket servers, over TCP and over TLS, answer the paths that serve_websocket describes. The ComfyUI server
fakes the endpoints that alchemy's client uses, and runs each prompt that names a sampler node at once (ComfyHttp).
"""

import base64
import gzip
import hashlib
import http.server
import json
import os
import random
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

PROTOCOLS = ["http/1.1"]


def openssl(directory, *arguments):
    subprocess.run(["openssl", *arguments], cwd=directory, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.PIPE)


def make_ca(directory, name, subject):
    openssl(directory, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
            "-keyout", name + ".key", "-out", name + ".pem", "-days", "30", "-subj", "/CN=" + subject,
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")


def make_leaf(directory, name, ca, serial, start=None, end=None):
    """Makes a server certificate for localhost and 127.0.0.1, issued by `ca`. With `start` and `end`, in the form
    YYYYMMDDHHMMSSZ, it is valid only between them, which `openssl ca` can backdate."""
    with open(os.path.join(directory, name + ".ext"), "w") as out:
        out.write("basicConstraints=critical,CA:FALSE\n"
                  "keyUsage=critical,digitalSignature\n"
                  "extendedKeyUsage=serverAuth\n"
                  "subjectAltName=DNS:localhost,IP:127.0.0.1\n")
    openssl(directory, "req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
            "-keyout", name + ".key", "-out", name + ".csr", "-subj", "/CN=localhost")
    if start is None:
        openssl(directory, "x509", "-req", "-in", name + ".csr", "-CA", ca + ".pem", "-CAkey", ca + ".key",
                "-set_serial", str(serial), "-days", "30", "-sha256",
                "-extfile", name + ".ext", "-out", name + ".pem")
        return
    database = os.path.join(directory, name + ".db")
    os.makedirs(database, exist_ok=True)
    open(os.path.join(database, "index.txt"), "w").close()
    with open(os.path.join(database, "serial"), "w") as out:
        out.write("%04X\n" % serial)
    # OpenSSL's configuration reads a backslash as an escape, so the path is written with forward slashes, which Windows
    # also accepts.
    with open(os.path.join(directory, name + ".cnf"), "w") as out:
        out.write("[ca]\ndefault_ca = peer\n"
                  "[peer]\n"
                  f"dir = {database.replace(os.sep, '/')}\n"
                  "database = $dir/index.txt\n"
                  "serial = $dir/serial\n"
                  "new_certs_dir = $dir\n"
                  "default_md = sha256\n"
                  "policy = anything\n"
                  "unique_subject = no\n"
                  "[anything]\ncommonName = supplied\n")
    openssl(directory, "ca", "-batch", "-notext", "-config", name + ".cnf", "-cert", ca + ".pem",
            "-keyfile", ca + ".key", "-in", name + ".csr", "-startdate", start, "-enddate", end,
            "-extfile", name + ".ext", "-out", name + ".pem")


def tls_context(directory, leaf):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(os.path.join(directory, leaf + ".pem"), os.path.join(directory, leaf + ".key"))
    context.set_alpn_protocols(PROTOCOLS)
    return context


def echo(connection):
    """Echoes until the client asks the server to end or ends itself, and returns how the exchange ended."""
    tail = b""
    while True:
        try:
            data = connection.recv(65536)
        except ssl.SSLEOFError:
            return b"<eof:ragged>"
        if not data:
            return b"<eof:clean>"
        connection.sendall(data)
        tail = (tail + data)[-5:]
        if tail == b"<bye>":
            return b"<eof:asked>"


def serve_tcp(connection):
    connection.sendall(echo(connection))
    connection.shutdown(socket.SHUT_WR)


def serve_tls(connection):
    connection.sendall(echo(connection))
    connection.unwrap()


def serve_truncating(connection):
    """Sends a few bytes and closes TCP without close_notify."""
    connection.sendall(b"bye")
    # SSLSocket.shutdown drops the TLS state and ends the socket's sending side, without close_notify. The socket then
    # reads what the client sends until it ends, so that closing it sends no reset.
    connection.shutdown(socket.SHUT_WR)
    while connection.recv(65536):
        pass


LARGE = b"".join(b"line %d\n" % i for i in range(400000))


class Http(http.server.BaseHTTPRequestHandler):
    """Answers the HTTP tests' requests on one connection, which it keeps open between them."""

    protocol_version = "HTTP/1.1"

    # Each route's answer: its status, its headers, and its body or the chunks of a chunked body.
    ROUTES = {
        "/hello": (200, {"Content-Type": "text/plain"}, b"hello"),
        "/chunked": (200, {"Transfer-Encoding": "chunked"}, [b"chu", b"nked", b" body"]),
        "/gzip": (200, {"Content-Encoding": "gzip"}, gzip.compress(b"compressed " * 1000)),
        "/large": (200, {}, LARGE),
        "/missing": (404, {"Content-Type": "text/plain"}, b"missing"),
        "/not-modified": (304, {"ETag": '"same"'}, b""),
        "/redirect": (302, {"Location": "/hello"}, b""),
        "/see-other": (303, {"Location": "/hello"}, b""),
        "/loop": (302, {"Location": "/loop"}, b""),
    }

    def setup(self):
        super().setup()
        self.served = 0

    def log_message(self, *arguments):
        pass

    # The cache's routes, each of which counts the times that the server answered it, in X-Served and in its body.
    # /cache/variant varies by X-Variant, whose value its body also names.
    CACHE_ROUTES = {
        "/cache/fresh": {"Cache-Control": "max-age=60"},
        "/cache/validated": {"Cache-Control": "no-cache", "ETag": '"v1"'},
        "/cache/no-store": {"Cache-Control": "no-store, max-age=60"},
        "/cache/variant": {"Cache-Control": "max-age=60", "Vary": "X-Variant"},
        "/cache/heuristic": {"Last-Modified": "Mon, 01 Jan 2024 00:00:00 GMT"},
        "/cache/expired": {"Expires": "Mon, 01 Jan 2024 00:00:00 GMT"},
    }
    cache_served = {}
    cache_lock = threading.Lock()

    # The method and headers that the last CORS preflight asked for, which /cors reports.
    preflight = "none"

    # The CORS response headers that /cors sends, by the name of the query parameter that gives each.
    CORS_HEADERS = {
        "origin": "Access-Control-Allow-Origin",
        "credentials": "Access-Control-Allow-Credentials",
        "methods": "Access-Control-Allow-Methods",
        "headers": "Access-Control-Allow-Headers",
    }

    def answer_cors(self, query):
        """Answers /cors: a preflight with no body, a redirect to `to`, or the method and the last preflight."""
        parameters = urllib.parse.parse_qs(query)
        if self.command == "OPTIONS":
            requested = self.headers.get("Access-Control-Request-Headers") or ""
            Http.preflight = "%s;%s" % (self.headers.get("Access-Control-Request-Method"), requested)
        target = parameters.get("to", [None])[0]
        if self.command == "OPTIONS":
            body = b""
        else:
            body = ("%s %s" % (self.command, Http.preflight)).encode()
            Http.preflight = "none"
        self.send_response(302 if target is not None and self.command != "OPTIONS" else 200)
        for name, header in self.CORS_HEADERS.items():
            for value in parameters.get(name, []):
                self.send_header(header, value)
        if target is not None and self.command != "OPTIONS":
            self.send_header("Location", target)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def answer_cached(self, body_wanted):
        with self.cache_lock:
            served = self.cache_served.get(self.path, 0) + 1
            self.cache_served[self.path] = served
        headers = self.CACHE_ROUTES[self.path]
        if headers.get("ETag") is not None and self.headers.get("If-None-Match") == headers["ETag"]:
            self.send_response(304)
            self.send_header("X-Served", str(served))
            self.send_header("ETag", headers["ETag"])
            self.end_headers()
            return
        body = b"%s %d" % (self.path.encode(), served)
        if self.path == "/cache/variant":
            body = b"%s %s %d" % (self.path.encode(), (self.headers.get("X-Variant") or "").encode(), served)
        self.send_response(200)
        self.send_header("X-Served", str(served))
        for name, value in headers.items():
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body_wanted:
            self.wfile.write(body)

    def answer(self, body_wanted):
        if self.path in self.CACHE_ROUTES:
            self.answer_cached(body_wanted)
            return
        path, _, query = self.path.partition("?")
        if path == "/cors":
            self.answer_cors(query)
            return
        if path == "/headers":
            body = ("Origin: %s\nReferer: %s\n" % (self.headers.get("Origin", "-"),
                                                   self.headers.get("Referer", "-"))).encode()
            self.send_response(200)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("X-Host", self.headers.get("Host", "-"))
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.served += 1
        if self.path == "/close":
            # A body that ends when the connection closes, as HTTP/1.0 framed it.
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            if body_wanted:
                self.wfile.write(b"closed")
            self.close_connection = True
            return
        if self.path == "/no-content-sent":
            # A 204 that declares content and sends it after its head, as a careless server does. The content arrives
            # once the client has taken the response as done.
            self.send_response(204)
            self.send_header("X-Requests", str(self.served))
            self.send_header("Content-Length", "5")
            self.end_headers()
            self.wfile.flush()
            time.sleep(0.1)
            self.wfile.write(b"stray")
            return
        status, headers, body = self.ROUTES.get(self.path, (404, {}, b""))
        self.send_response(status)
        self.send_header("X-Requests", str(self.served))
        for name, value in headers.items():
            self.send_header(name, value)
        if isinstance(body, list):
            self.end_headers()
            if body_wanted:
                for chunk in body:
                    self.wfile.write(b"%x\r\n%s\r\n" % (len(chunk), chunk))
                self.wfile.write(b"0\r\n\r\n")
            return
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body_wanted:
            self.wfile.write(body)

    def do_GET(self):
        self.answer(True)

    def do_HEAD(self):
        self.answer(False)

    def do_OPTIONS(self):
        self.answer(True)

    def do_PUT(self):
        length = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(length)
        self.answer(True)

    def do_POST(self):
        """Echoes the body posted to /echo, and answers any other path as GET does."""
        length = int(self.headers.get("Content-Length", "0"))
        received = self.rfile.read(length)
        if self.path != "/echo":
            self.answer(True)
            return
        self.served += 1
        self.send_response(200)
        self.send_header("X-Requests", str(self.served))
        self.send_header("Content-Length", str(len(received)))
        self.end_headers()
        self.wfile.write(received)


def serve_http(connection):
    Http(connection, connection.getpeername(), None)


WEBSOCKET_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# The subprotocols that the WebSocket server speaks. It chooses the first that the client offers.
WEBSOCKET_PROTOCOLS = ["chat", "superchat"]


def read_exactly(connection, size):
    data = b""
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data


def ws_frame(opcode, payload, fin=True):
    """Returns an unmasked frame, as a server sends it."""
    head = bytes([(0x80 if fin else 0) | opcode])
    if len(payload) < 126:
        head += bytes([len(payload)])
    elif len(payload) < 65536:
        head += bytes([126]) + len(payload).to_bytes(2, "big")
    else:
        head += bytes([127]) + len(payload).to_bytes(8, "big")
    return head + payload


def ws_read_frame(connection):
    """Reads a frame from the client, which must be masked, and returns its fin bit, opcode and unmasked payload."""
    first, second = read_exactly(connection, 2)
    if not second & 0x80:
        raise ValueError("an unmasked frame from the client")
    size = second & 0x7F
    if size == 126:
        size = int.from_bytes(read_exactly(connection, 2), "big")
    elif size == 127:
        size = int.from_bytes(read_exactly(connection, 8), "big")
    key = read_exactly(connection, 4)
    payload = bytes(b ^ key[i % 4] for i, b in enumerate(read_exactly(connection, size)))
    return bool(first & 0x80), first & 0x0F, payload


def ws_echo(connection):
    """Echoes each message until the client closes, and answers its Close with the same code."""
    message, opcode = b"", 0
    while True:
        fin, frame_opcode, payload = ws_read_frame(connection)
        if frame_opcode == 0x8:
            connection.sendall(ws_frame(0x8, payload[:2]))
            return
        if frame_opcode == 0x9:
            connection.sendall(ws_frame(0xA, payload))
            continue
        if frame_opcode in (0x1, 0x2):
            message, opcode = b"", frame_opcode
        message += payload
        if fin:
            connection.sendall(ws_frame(opcode, message))


def ws_await_close(connection):
    """Reads frames until the client's Close."""
    while ws_read_frame(connection)[1] != 0x8:
        pass


def serve_websocket(connection):
    """Answers a WebSocket handshake by the request's path, then plays that path's part (rfc 6455).

    /echo echoes each message. /close closes with code 4000 and the reason "bye" at once. /fragmented sends a text
    message in three fragments with a ping between them, and then a binary message of 70000 bytes. /abort ends the
    connection without a Close frame. /reject answers 403, /bad-accept answers 101 with a wrong Sec-WebSocket-Accept,
    and /bad-frame sends a frame with a reserved bit set."""
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = connection.recv(4096)
        if not chunk:
            return
        head += chunk
    lines = head.split(b"\r\n\r\n")[0].decode("latin-1").split("\r\n")
    path = lines[0].split(" ")[1]
    fields = {}
    for line in lines[1:]:
        name, _, value = line.partition(":")
        fields[name.strip().lower()] = value.strip()
    if path == "/reject" or fields.get("upgrade", "").lower() != "websocket":
        connection.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
        return
    key = fields["sec-websocket-key"].encode()
    accept = base64.b64encode(hashlib.sha1(key + WEBSOCKET_GUID).digest()).decode()
    if path == "/bad-accept":
        accept = base64.b64encode(b"\0" * 20).decode()
    response = ("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n")
    offered = [p.strip() for p in fields.get("sec-websocket-protocol", "").split(",") if p.strip()]
    chosen = next((p for p in offered if p in WEBSOCKET_PROTOCOLS), None)
    if chosen:
        response += f"Sec-WebSocket-Protocol: {chosen}\r\n"
    connection.sendall((response + "\r\n").encode())
    try:
        if path == "/echo":
            ws_echo(connection)
        elif path == "/close":
            connection.sendall(ws_frame(0x8, (4000).to_bytes(2, "big") + b"bye"))
            ws_await_close(connection)
        elif path == "/fragmented":
            connection.sendall(ws_frame(0x1, b"hel", fin=False) + ws_frame(0x9, b"ping")
                               + ws_frame(0x0, b"lo wo", fin=False) + ws_frame(0x0, b"rld"))
            connection.sendall(ws_frame(0x2, bytes(i % 251 for i in range(70000))))
            ws_echo(connection)
        elif path == "/bad-frame":
            connection.sendall(bytes([0xC1, 0]))
            ws_await_close(connection)
        # /abort, and any other path, ends the connection here.
    except (EOFError, ValueError):
        return


# A PNG file of one pixel, which the fake ComfyUI server's runs save.
COMFY_IMAGE = bytes.fromhex(
    "89504e470d0a1a0a0000000d4948445200000001000000010806000000"
    "1f15c4890000000d49444154789c63f8cfc0f01f0005000201a7c8ab1b0000000049454e44ae426082")


class Comfy:
    """The state that the fake ComfyUI server's connections share: the WebSocket of each client id, the images that
    were uploaded, and the prompts that were submitted."""
    lock = threading.Lock()
    sockets = {}
    uploads = {}
    prompts = 0


def comfy_send(client_id, frame):
    with Comfy.lock:
        connection = Comfy.sockets.get(client_id)
    if connection is not None:
        try:
            connection.sendall(frame)
        except OSError:
            pass


def comfy_run(client_id, prompt_id):
    """Runs a prompt as a server reports a run to its client: progress, a preview, the image that its save node wrote,
    and the end of the prompt."""
    def message(kind, data):
        data["prompt_id"] = prompt_id
        return ws_frame(0x1, json.dumps({"type": kind, "data": data}).encode())
    time.sleep(0.05)
    comfy_send(client_id, message("execution_start", {}))
    comfy_send(client_id, message("executing", {"node": "sampler"}))
    comfy_send(client_id, message("progress", {"value": 1, "max": 2, "node": "sampler"}))
    comfy_send(client_id, ws_frame(0x2, b"\0\0\0\1\0\0\0\2" + COMFY_IMAGE))
    comfy_send(client_id, message("progress", {"value": 2, "max": 2, "node": "sampler"}))
    comfy_send(client_id, message("executed", {"node": "save", "output": {"images": [
        {"filename": "Alchemy_00001_.png", "subfolder": "", "type": "output"}]}}))
    comfy_send(client_id, message("executing", {"node": None}))


class ComfyHttp(http.server.BaseHTTPRequestHandler):
    """Answers the REST endpoints that alchemy's ComfyUI client uses, and upgrades /ws to the event stream."""

    protocol_version = "HTTP/1.1"

    OBJECT_INFO = {
        "CheckpointLoaderSimple": {"input": {"required": {"ckpt_name": [["sd_xl_base_1.0.safetensors"], {}]}}},
        "LoraLoader": {"input": {"required": {"lora_name": [["detail.safetensors"], {}]}}},
        "KSampler": {"input": {"required": {
            "sampler_name": ["COMBO", {"options": ["euler", "dpmpp_2m"]}],
            "scheduler": [["normal", "karras"], {}]}}},
        "UpscaleModelLoader": {"input": {"required": {"model_name": [["RealESRGAN_x4plus.pth"], {}]}}},
    }

    def log_message(self, *arguments):
        pass

    def reply(self, status, body, content_type="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(url.query)
        if url.path == "/ws":
            self.upgrade(query["clientId"][0])
        elif url.path == "/object_info":
            self.reply(200, self.OBJECT_INFO)
        elif url.path == "/embeddings":
            self.reply(200, ["easynegative"])
        elif url.path == "/internal/folder_paths":
            self.reply(200, {"checkpoints": ["/models/checkpoints", "/more/checkpoints"]})
        elif url.path == "/view" and query.get("filename") == ["Alchemy_00001_.png"]:
            self.reply(200, COMFY_IMAGE, "image/png")
        else:
            self.reply(404, {"error": "not found"})

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if self.path == "/prompt":
            request = json.loads(body)
            if "sampler" not in request.get("prompt", {}):
                self.reply(400, {"error": "no sampler"})
                return
            with Comfy.lock:
                Comfy.prompts += 1
                prompt_id = "prompt-%d" % Comfy.prompts
            self.reply(200, {"prompt_id": prompt_id, "number": Comfy.prompts})
            threading.Thread(target=comfy_run, args=(request["client_id"], prompt_id), daemon=True).start()
        elif self.path == "/upload/image":
            boundary = self.headers["Content-Type"].split("boundary=")[1].encode()
            part = body.split(b"--" + boundary)[1]
            head, _, content = part.partition(b"\r\n\r\n")
            name = head.split(b'filename="')[1].split(b'"')[0].decode()
            with Comfy.lock:
                Comfy.uploads[name] = content[:-2]
            self.reply(200, {"name": name, "subfolder": "", "type": "input"})
        elif self.path == "/interrupt":
            self.reply(200, {})
        else:
            self.reply(404, {"error": "not found"})

    def upgrade(self, client_id):
        key = self.headers["Sec-WebSocket-Key"].encode()
        accept = base64.b64encode(hashlib.sha1(key + WEBSOCKET_GUID).digest()).decode()
        self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
        self.wfile.flush()
        connection = self.connection
        with Comfy.lock:
            Comfy.sockets[client_id] = connection
        status = {"type": "status", "data": {"status": {"exec_info": {"queue_remaining": 0}}, "sid": client_id}}
        connection.sendall(ws_frame(0x1, json.dumps(status).encode()))
        try:
            ws_echo(connection)
        except (EOFError, ValueError, OSError):
            pass
        with Comfy.lock:
            Comfy.sockets.pop(client_id, None)
        self.close_connection = True


def serve_comfy(connection):
    ComfyHttp(connection, connection.getpeername(), None)


def listen():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", 0))
    server.listen(16)
    return server


def closed_port():
    """Returns a port that nothing listens on. The port is below the ranges from which systems assign ports to the
    sockets that ask for any (32768 and up on Linux, 49152 and up on macOS and Windows), so that no server of another
    run, and no outgoing connection, takes it after it is chosen."""
    for port in random.sample(range(10000, 32768), 64):
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            probe.bind(("127.0.0.1", port))
            return port
        except OSError:
            continue
        finally:
            probe.close()
    sys.exit("no free port below the assigned ranges")


def accept(server, context, handle):
    """Accepts connections on `server` for as long as the peer runs, each on a thread of its own."""
    def run(raw):
        try:
            with raw:
                if context is None:
                    handle(raw)
                    return
                with context.wrap_socket(raw, server_side=True, suppress_ragged_eofs=False) as connection:
                    handle(connection)
        except (OSError, ssl.SSLError):
            # A client that rejects the server's certificate aborts the handshake, as the tests intend.
            pass

    while True:
        raw, _ = server.accept()
        threading.Thread(target=run, args=(raw,), daemon=True).start()


def main():
    if len(sys.argv) < 2:
        print("usage: net_peer.py <binary> [arguments]", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="pr_net_peer") as directory:
        make_ca(directory, "ca", "Prax Test CA")
        make_ca(directory, "other_ca", "Prax Untrusted CA")
        make_leaf(directory, "leaf", "ca", 2)
        make_leaf(directory, "expired", "ca", 3, start="20200101000000Z", end="20200201000000Z")
        make_leaf(directory, "untrusted", "other_ca", 4)
        openssl(directory, "x509", "-in", "ca.pem", "-outform", "DER", "-out", "ca.der")

        servers = {
            "tcp_echo": (None, serve_tcp),
            "tls_echo": (tls_context(directory, "leaf"), serve_tls),
            "tls_truncating": (tls_context(directory, "leaf"), serve_truncating),
            "tls_expired": (tls_context(directory, "expired"), serve_tls),
            "tls_untrusted": (tls_context(directory, "untrusted"), serve_tls),
            "http": (None, serve_http),
            "https": (tls_context(directory, "leaf"), serve_http),
            "ws": (None, serve_websocket),
            "wss": (tls_context(directory, "leaf"), serve_websocket),
            "comfy": (None, serve_comfy),
        }
        ports = []
        for name, (context, handle) in servers.items():
            server = listen()
            ports.append((name, server.getsockname()[1]))
            threading.Thread(target=accept, args=(server, context, handle), daemon=True).start()
        ports.append(("closed", closed_port()))
        with open(os.path.join(directory, "ports"), "w") as out:
            for name, port in ports:
                out.write(f"{name} {port}\n")

        environment = dict(os.environ, PR_NET_PEER=directory)
        return subprocess.run(sys.argv[1:], env=environment).returncode


if __name__ == "__main__":
    sys.exit(main())
