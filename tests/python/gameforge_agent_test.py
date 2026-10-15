# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for ``gameforge_agent``, the engine of the "Agentes de IA" panel: the transcript lines of Claude Code's
stream-json events, the command line of a request, the MCP config and the approval server. No Claude, no
network.

Runs with plain Python, no Studio build needed:
  python3 tests/python/gameforge_agent_test.py
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts", "modules"))

import gameforge_agent as engine  # noqa: E402


def _assistant(*blocks):
    return {"type": "assistant", "message": {"role": "assistant", "content": list(blocks)}}


class DescribeTest(unittest.TestCase):

    def test_text_and_tool_steps(self):
        event = _assistant({"type": "text", "text": "Vou criar a **lâmina**.\nDepois confiro com `look`."},
                           {"type": "tool_use", "name": "mcp__blender__execute_blender_code", "input": {"code": "x"}},
                           {"type": "tool_use", "name": "mcp__blender__look", "input": {}})
        self.assertEqual(engine.describe(event),
                         ["Vou criar a lâmina.", "Depois confiro com look.", "› Rodar código", "› Olhar a viewport"])

    def test_tool_results(self):
        event = {"type": "user", "message": {"role": "user", "content": [
            {"type": "tool_result", "tool_use_id": "a", "content": [{"type": "text", "text": "\nCriado: Lâmina\nmais"}]},
            {"type": "tool_result", "tool_use_id": "b", "content": "NameError: x", "is_error": True},
            {"type": "tool_result", "tool_use_id": "c", "content": [{"type": "image", "source": {}}]},
            # MCP for Blender's own wrapping.
            {"type": "tool_result", "tool_use_id": "d", "content": [{"type": "text", "text": '{"result":"Scene \'Scene\' | 1 mesh\\n"}'}]},
            {"type": "tool_result", "tool_use_id": "e", "content": '{"error": "sem objeto"}', "is_error": True},
        ]}}
        self.assertEqual(engine.describe(event), ["  ↳ Criado: Lâmina", "  ✗ NameError: x", "  ↳ [imagem]",
                                                  "  ↳ Scene 'Scene' | 1 mesh", "  ✗ sem objeto"])
        # A plain user message (the request itself) is not repeated.
        self.assertEqual(engine.describe({"type": "user", "message": {"role": "user", "content": "oi"}}), [])

    def test_result(self):
        done = {"type": "result", "subtype": "success", "is_error": False, "duration_ms": 12400, "total_cost_usd": 0.0123}
        self.assertEqual(engine.describe(done), ["— pronto em 12 s"])
        self.assertEqual(engine.describe(done, show_cost=True), ["— pronto em 12 s · US$ 0.0123"])
        failed = {"type": "result", "subtype": "error_during_execution", "is_error": True, "result": "Login expired"}
        self.assertEqual(engine.describe(failed), ["— erro: Login expired"])

    def test_missing_mcp_server_and_limits(self):
        init = {"type": "system", "subtype": "init", "session_id": "s",
                "mcp_servers": [{"name": "blender", "status": "failed"}, {"name": "gameforge_approval", "status": "connected"}]}
        self.assertEqual(engine.describe(init), ["aviso: sem conexão com blender"])
        self.assertEqual(engine.describe({"type": "rate_limit_event", "rate_limit_info": {"status": "allowed"}}), [])
        self.assertTrue(engine.describe({"type": "rate_limit_event", "rate_limit_info": {"status": "rejected"}}))
        self.assertEqual(engine.describe({"type": "stream_event", "event": {}}), [])


class CommandTest(unittest.TestCase):

    def test_isolated_from_the_users_setup(self):
        cmd = engine.command("claude.exe", mcp_config="m.json", system="regras")
        self.assertEqual(cmd[:2], ["claude.exe", "-p"])
        for flag in ("--strict-mcp-config", "--disable-slash-commands"):
            self.assertIn(flag, cmd)
        self.assertEqual(cmd[cmd.index("--setting-sources") + 1], "local")
        self.assertEqual(cmd[cmd.index("--tools") + 1], "")
        self.assertEqual(cmd[cmd.index("--permission-prompt-tool") + 1], engine.APPROVAL_TOOL)
        allowed = json.loads(cmd[cmd.index("--settings") + 1])["permissions"]["allow"]
        self.assertIn("mcp__blender__look", allowed)
        self.assertNotIn("mcp__blender__execute_blender_code", allowed)
        self.assertNotIn("--resume", cmd)
        self.assertNotIn("--model", cmd)

    def test_resume_and_model(self):
        cmd = engine.command("claude.exe", mcp_config="m.json", system="", model="haiku", session="abc")
        self.assertEqual(cmd[cmd.index("--resume") + 1], "abc")
        self.assertEqual(cmd[cmd.index("--model") + 1], "haiku")


class McpConfigTest(unittest.TestCase):

    def test_servers(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "mcp.json")
            engine.write_mcp_config(path, python="py.exe", approval_port=4321, blender_version="2.1.9")
            with open(path, encoding="utf-8") as fh:
                servers = json.load(fh)["mcpServers"]
        approval = servers["gameforge_approval"]
        self.assertEqual(approval["command"], "py.exe")
        self.assertEqual(approval["args"], ["-I", engine.APPROVAL_SCRIPT, "4321"])
        if shutil.which("uvx"):
            blender = servers["blender"]
            self.assertIn("mcp-for-blender==2.1.9", blender["args"])
            self.assertEqual(blender["env"], {"BLENDER_PORT": str(engine.STUDIO_PORT), "DISABLE_TELEMETRY": "1"})
        else:
            self.assertNotIn("blender", servers)

    def test_without_mcp_for_blender(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "mcp.json")
            engine.write_mcp_config(path, python="py.exe", approval_port=1)
            with open(path, encoding="utf-8") as fh:
                self.assertEqual(list(json.load(fh)["mcpServers"]), ["gameforge_approval"])


class ApprovalServerTest(unittest.TestCase):
    """approval.py speaks MCP on stdio and asks the panel's socket."""

    def _panel(self, answer):
        """A stand-in for the panel: accepts one request and answers it."""
        server = socket.socket()
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        seen = []

        def serve():
            conn, _address = server.accept()
            data = b""
            while not data.endswith(b"\n"):
                data += conn.recv(65536)
            seen.append(json.loads(data))
            conn.sendall((json.dumps(answer) + "\n").encode("utf-8"))
            conn.close()
            server.close()

        threading.Thread(target=serve, daemon=True).start()
        return server.getsockname()[1], seen

    def _call(self, port, messages):
        proc = subprocess.run([sys.executable, "-I", engine.APPROVAL_SCRIPT, str(port)],
                              # Raw UTF-8, as Claude Code writes it (no \u escapes).
                              input="".join(json.dumps(m, ensure_ascii=False) + "\n" for m in messages).encode("utf-8"),
                              capture_output=True, timeout=60)
        return [json.loads(line) for line in proc.stdout.decode("utf-8").splitlines()]

    def test_round_trip(self):
        code = "print('青龙白大.ecm, ação')"  # Game paths and accents must cross the pipes intact.
        port, seen = self._panel({"behavior": "allow", "updatedInput": {"code": code}})
        request = {"tool_name": "mcp__blender__execute_blender_code", "input": {"code": code}, "tool_use_id": "t1"}
        replies = self._call(port, [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "ask", "arguments": request}},
        ])
        self.assertEqual([r["id"] for r in replies], [1, 2, 3])
        self.assertEqual(replies[0]["result"]["protocolVersion"], "2025-06-18")
        self.assertEqual([t["name"] for t in replies[1]["result"]["tools"]], ["ask"])
        answer = json.loads(replies[2]["result"]["content"][0]["text"])
        self.assertEqual(answer, {"behavior": "allow", "updatedInput": {"code": code}})
        self.assertEqual(seen, [request])

    def test_panel_gone_denies(self):
        free = socket.socket()
        free.bind(("127.0.0.1", 0))
        port = free.getsockname()[1]
        free.close()  # Nothing listens there now.
        replies = self._call(port, [{"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                     "params": {"name": "ask", "arguments": {"tool_name": "x", "input": {}}}}])
        self.assertEqual(json.loads(replies[0]["result"]["content"][0]["text"])["behavior"], "deny")


if __name__ == "__main__":
    unittest.main()
