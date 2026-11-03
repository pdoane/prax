"""A language server for the editor's tests, over standard input and output: it counts positions in UTF-16, reports
an error over the first three characters of each document that opens or changes, and answers hovers and definitions
with fixed results."""

import json
import sys


def read_message(stream):
    """Returns the next message, or None at the end of the input."""
    length = None
    while True:
        line = stream.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            break
        name, _, value = line.partition(b":")
        if name.strip().lower() == b"content-length":
            length = int(value.strip())
    return json.loads(stream.read(length))


def send(message):
    body = json.dumps(message).encode()
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    sys.stdout.buffer.flush()


def publish(uri, version):
    send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics", "params": {
        "uri": uri, "version": version, "diagnostics": [{
            "range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 3}},
            "severity": 1, "source": "fake", "message": "bad start"}]}})


def main():
    stream = sys.stdin.buffer
    while True:
        message = read_message(stream)
        if message is None:
            return
        method = message.get("method")
        params = message.get("params") or {}
        if method == "initialize":
            send({"jsonrpc": "2.0", "id": message["id"], "result": {
                "capabilities": {"textDocumentSync": 2, "hoverProvider": True, "definitionProvider": True},
                "serverInfo": {"name": "fake"}}})
        elif method == "textDocument/didOpen":
            publish(params["textDocument"]["uri"], params["textDocument"]["version"])
        elif method == "textDocument/didChange":
            publish(params["textDocument"]["uri"], params["textDocument"]["version"])
        elif method == "textDocument/hover":
            position = params["position"]
            text = "hover at %d:%d" % (position["line"], position["character"])
            send({"jsonrpc": "2.0", "id": message["id"], "result": {"contents": {"kind": "plaintext", "value": text}}})
        elif method == "textDocument/definition":
            send({"jsonrpc": "2.0", "id": message["id"], "result": {
                "uri": params["textDocument"]["uri"],
                "range": {"start": {"line": 1, "character": 4}, "end": {"line": 1, "character": 5}}}})
        elif method == "shutdown":
            send({"jsonrpc": "2.0", "id": message["id"], "result": None})
        elif method == "exit":
            return
        elif "id" in message:
            send({"jsonrpc": "2.0", "id": message["id"], "result": None})


main()
