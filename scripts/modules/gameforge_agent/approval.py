# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Claude Code's permission prompt tool for the "Agentes de IA" panel: a tiny MCP server (stdio, one JSON-RPC
message per line) with one tool, ``ask``. Each call goes to the panel through a local socket
(127.0.0.1:<port>), waits for "Executar" or "Recusar" there and answers Claude Code with
``{"behavior": "allow", "updatedInput": ...}`` or ``{"behavior": "deny", "message": ...}``. Nothing is approved
here. Standard library only, run by the Studio's own Python::

    python -I approval.py <port>
"""

import json
import socket
import sys

TOOL = {
    "name": "ask",
    "description": "Asks the user, in the Studio's \"Agentes de IA\" panel, to allow or deny a tool call.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "tool_name": {"type": "string"},
            "input": {"type": "object"},
            "tool_use_id": {"type": "string"},
        },
        "required": ["tool_name", "input"],
    },
}


def ask(arguments, port):
    """The panel's answer for one permission request (a JSON text), or a denial when the panel is gone."""
    try:
        with socket.create_connection(("127.0.0.1", port)) as conn:
            conn.sendall((json.dumps(arguments, ensure_ascii=False) + "\n").encode("utf-8"))
            data = b""
            while not data.endswith(b"\n"):
                part = conn.recv(65536)
                if not part:
                    break
                data += part
        answer = data.decode("utf-8").strip()
    except OSError as ex:
        answer = ""
        reason = "o painel do Studio não respondeu: {:s}".format(str(ex))
    else:
        reason = "o painel do Studio fechou sem responder"
    return answer or json.dumps({"behavior": "deny", "message": reason}, ensure_ascii=False)


def reply(request_id, result=None, error=None):
    message = {"jsonrpc": "2.0", "id": request_id}
    if error is not None:
        message["error"] = error
    else:
        message["result"] = result
    sys.stdout.write(json.dumps(message, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def main():
    port = int(sys.argv[1])
    # Claude Code speaks UTF-8; on Windows a pipe defaults to the ANSI code page, which would garble any
    # accent or game path (青龙白大.ecm) in a tool's input on its way to the panel and back.
    sys.stdin.reconfigure(encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8")
    for line in sys.stdin:
        if not line.strip():
            continue
        request = json.loads(line)
        method, request_id = request.get("method"), request.get("id")
        if request_id is None:  # A notification (notifications/initialized...).
            continue
        if method == "initialize":
            version = request.get("params", {}).get("protocolVersion", "2024-11-05")
            reply(request_id, {"protocolVersion": version, "capabilities": {"tools": {}},
                               "serverInfo": {"name": "gameforge_approval", "version": "1.0"}})
        elif method == "tools/list":
            reply(request_id, {"tools": [TOOL]})
        elif method == "tools/call":
            text = ask(request.get("params", {}).get("arguments", {}), port)
            reply(request_id, {"content": [{"type": "text", "text": text}]})
        elif method == "ping":
            reply(request_id, {})
        else:
            reply(request_id, error={"code": -32601, "message": "unknown method: {:s}".format(str(method))})


if __name__ == "__main__":
    main()
