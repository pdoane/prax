"""A debug adapter for the editor's tests, over standard input and output: it stops at the first breakpoint that it is
given, steps a line at a time, counts its steps in a variable, and exits when it continues."""

import json
import sys

seq = 0


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
    global seq
    seq += 1
    message["seq"] = seq
    body = json.dumps(message).encode()
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    sys.stdout.buffer.flush()


def respond(request, body=None):
    send({"type": "response", "request_seq": request["seq"], "success": True, "command": request["command"],
          "body": body or {}})


def event(name, body=None):
    send({"type": "event", "event": name, "body": body or {}})


def main():
    stream = sys.stdin.buffer
    breakpoints = {}
    place = None
    steps = 0
    while True:
        message = read_message(stream)
        if message is None:
            return
        command = message.get("command")
        arguments = message.get("arguments") or {}
        if command == "initialize":
            respond(message, {"supportsConfigurationDoneRequest": True})
        elif command == "launch":
            respond(message)
            event("initialized")
        elif command == "setBreakpoints":
            lines = [b["line"] for b in arguments.get("breakpoints", [])]
            breakpoints[arguments["source"]["path"]] = lines
            respond(message, {"breakpoints": [{"verified": True, "line": line} for line in lines]})
        elif command == "configurationDone":
            respond(message)
            event("output", {"category": "stdout", "output": "hello\n"})
            for path, lines in breakpoints.items():
                if lines:
                    place = (path, lines[0])
                    event("stopped", {"reason": "breakpoint", "threadId": 1})
                    break
        elif command == "threads":
            respond(message, {"threads": [{"id": 1, "name": "main"}]})
        elif command == "stackTrace":
            path, line = place
            respond(message, {"stackFrames": [
                {"id": 1, "name": "main", "line": line, "column": 1, "source": {"path": path}},
                {"id": 2, "name": "start", "line": 1, "column": 1}]})
        elif command == "scopes":
            respond(message, {"scopes": [{"name": "Locals", "variablesReference": 10}]})
        elif command == "variables":
            respond(message, {"variables": [{"name": "steps", "value": str(steps), "variablesReference": 0}]})
        elif command == "next":
            respond(message)
            steps += 1
            place = (place[0], place[1] + 1)
            event("stopped", {"reason": "step", "threadId": 1})
        elif command == "continue":
            respond(message, {"allThreadsContinued": True})
            event("output", {"category": "stdout", "output": "bye\n"})
            event("exited", {"exitCode": 0})
            event("terminated")
        elif command == "disconnect":
            respond(message)
            return
        else:
            respond(message)


main()
