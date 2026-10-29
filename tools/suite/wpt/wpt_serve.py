"""WPT's server (wptserve), run from a checkout for the engine's runs of its tests, as wptrunner runs it for a browser.

The server serves the checkout with its own headers, substitutions and handlers, and with the vendor's files in place
of the checkout's: the reporter and the testdriver vendor file (tools/suite/wpt). Its ports are the ones that the system
gives it, so that runs in several worktrees do not collide, and the run learns them from the server. Its subdomain
check is off: the engine reaches WPT's hosts by its own host rules, which point them at the loopback.

Server(checkout, aliases) starts the server and waits until it answers. `aliases` maps a URL path that ends in '/' to
a directory that the server serves there too, with the checkout's handlers. Its `ports` are wptserve's ports by scheme,
and stop() ends it. Run as a script, `wpt_serve.py <checkout> [<aliases as JSON>]` is the server itself, which prints
one line `WPT-SERVE <json>` with its ports once they are known, and runs until its stdin ends.
`wpt_serve.py --around <checkout> <command>...` starts the server, runs the command with `--wpt-server <http port>
<https port>` added to its arguments, and stops the server once the command ends, with the command's status.

wptserve needs a Python that still has http.server.CGIHTTPRequestHandler, which 3.15 removed, so the server runs in
the newest Python on the path that has it (PYTHONS), or else in the Python that runs this module if it has it, for a
path that names Python without its version. It writes no bytecode, since the checkout is shared.
"""

import http.server
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

SUPPORT = os.path.dirname(os.path.abspath(__file__))

# The interpreters that can run wptserve, newest first.
PYTHONS = ["python3.14", "python3.13", "python3.12", "python3.11", "python3.10"]

MARKER = "WPT-SERVE "

# How long the server may take to start, in seconds.
START_TIMEOUT = 60


def python():
    """Returns the interpreter that runs the server, or None if neither the path nor this one can."""
    for name in PYTHONS:
        path = shutil.which(name)
        if path is not None:
            return path
    if hasattr(http.server, "CGIHTTPRequestHandler"):
        return sys.executable
    return None


class Server:
    """A running server: its process and its ports by scheme, as wptserve's config gives them."""

    def __init__(self, checkout, aliases=None, log=subprocess.DEVNULL):
        interpreter = python()
        if interpreter is None:
            sys.exit("WPT's server needs one of %s on the path, or a Python before 3.15 to run the tools"
                     % ", ".join(PYTHONS))
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        command = [interpreter, os.path.abspath(__file__), checkout, json.dumps(aliases or {})]
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True,
                                        env=environment)
        self.ports = None
        for line in self.process.stdout:
            if line.startswith(MARKER):
                self.ports = json.loads(line[len(MARKER):])["ports"]
                break
        if self.ports is None:
            self.stop()
            sys.exit("WPT's server did not start")
        # The ports are known before the servers listen on them.
        deadline = time.monotonic() + START_TIMEOUT
        for port in self.ports["http"] + self.ports["https"]:
            while not answers(port):
                if time.monotonic() > deadline or self.process.poll() is not None:
                    self.stop()
                    sys.exit("WPT's server did not start")
                time.sleep(0.05)

    def stop(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()


def answers(port):
    """Returns true if a server listens on `port` of the loopback."""
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=1):
            return True
    except OSError:
        return False


def vendor_routes(logger, aliases, config):
    """Returns the checkout's routes, with the vendor's files in place of the checkout's, as wptrunner adds its own
    (wptrunner/environment.py)."""
    from tools.serve import serve
    builder = serve.get_route_builder(logger, aliases, config)
    for name, route in (("report.js", "/resources/testharnessreport.js"),
                        ("testdriver-vendor.js", "/resources/testdriver-vendor.js")):
        builder.add_static(os.path.join(SUPPORT, name), None, "text/javascript;charset=utf8", route,
                           headers={"Cache-Control": "max-age=3600"})
    return builder


def serve_responses_whole():
    """Makes wptserve give each response whose content it knows a Content-Length. wptserve streams a static file, and a
    Python handler's content is a list, so either response goes without a Content-Length, and the server closes the
    connection after it. With the content whole, the response has its length and the connection carries the next
    request, as a browser's does. A run makes tens of thousands of requests, a page that polls a handler makes thousands
    on its own, and a connection for each exhausts the machine's ports while the closed connections wait out TIME_WAIT,
    which fails the connections of every run on the machine. Each of the server's processes calls it, since each imports
    wptserve for itself."""
    from wptserve import handlers, response
    read = handlers.FileHandler.get_data

    def get_data(self, response, path, byte_ranges):
        data = read(self, response, path, byte_ranges)
        if hasattr(data, "read"):
            with data:
                return data.read()
        return data

    handlers.FileHandler.get_data = get_data
    write = response.Response.write

    def write_whole(self):
        # Content of strings alone is known before it is written. Content with a function or a file streams, as the
        # handler asks. Only the response that wptserve writes whole is joined: a handler that writes its own head
        # streams what follows it, and its response goes without a length.
        content = self.content
        if isinstance(content, (list, tuple)) and all(isinstance(item, (bytes, str)) for item in content):
            self.content = b"".join(item.encode(self.encoding) if isinstance(item, str) else item for item in content)
        write(self)

    response.Response.write = write_whole


def main():
    checkout = os.path.abspath(sys.argv[1])
    aliases = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
    sys.path.insert(0, checkout)
    from tools import localpaths  # noqa: F401
    from tools.serve import serve
    serve_responses_whole()
    from wptserve import stash
    import multiprocessing
    import uuid

    logger = serve.get_logger("warning", None)
    ports = {scheme: ["auto"] * (2 if scheme in ("http", "https") else 1)
             for scheme in ("http", "https", "http-local", "https-local", "http-public", "https-public", "ws", "wss")}
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        json.dump({"server_host": "127.0.0.1", "check_subdomains": False, "ports": ports,
                   "aliases": [{"url-path": path, "local-dir": os.path.abspath(directory)}
                               for path, directory in aliases.items()]}, f)
        overrides = f.name
    try:
        with serve.build_config(logger, os.path.join(checkout, "config.json"), config_path=overrides) as config:
            routes = vendor_routes(logger, config.aliases, config).get_routes()
            mp_context = multiprocessing.get_context("spawn")
            with stash.StashServer((config.server_host, serve.get_port("")), authkey=str(uuid.uuid4())):
                servers = serve.start(logger, config, routes, mp_context, None, h2=False)
                print(MARKER + json.dumps({"ports": dict(config.ports)}), flush=True)
                sys.stdin.read()
                for server in serve.iter_servers(servers):
                    server.request_shutdown()
                for server in serve.iter_servers(servers):
                    server.wait(timeout=1)
    finally:
        os.unlink(overrides)


def around():
    """Runs the command that follows the checkout with the server's ports, and returns its status."""
    server = Server(sys.argv[2])
    try:
        command = sys.argv[3:] + ["--wpt-server", str(server.ports["http"][0]), str(server.ports["https"][0])]
        return subprocess.call(command)
    finally:
        server.stop()


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--around":
        sys.exit(around())
    main()
elif __name__ == "__mp_main__":
    # A server process that multiprocessing spawned, with the path of the process that spawned it.
    serve_responses_whole()
