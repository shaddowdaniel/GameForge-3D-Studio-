# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The engine of the "Agentes de IA" panel, without ``bpy``.

Each request is one run of the user's own Claude Code CLI (``claude -p``, stream-json output), resumed with
``--resume`` so the conversation carries on. Anthropic lets a product run the unmodified Claude Code with each
user signed in through Claude Code's own sign-in, so the Studio never reads or stores Claude credentials:
signing in, signing out and the status are ``claude auth`` commands.

A run sees two MCP servers and nothing else of the user's Claude Code setup (plugins, hooks, skills):

- ``blender``: MCP for Blender (the user's ``uvx mcp-for-blender``), talking to its add-on inside the Studio
  on ``STUDIO_PORT``, with the author's telemetry off (it sends prompts and viewport captures);
- ``gameforge_approval``: ``approval.py``, Claude Code's permission prompt tool, which asks the panel.
"""

__all__ = (
    "APPROVAL_TOOL",
    "STUDIO_PORT",
    "auth_status",
    "blender_mcp_addon",
    "command",
    "describe",
    "find_claude",
    "tool_label",
    "write_mcp_config",
)

import json
import os
import shutil
import subprocess

# The MCP for Blender add-on's socket in the Studio. Blender's own default is 9876: a Blender open at the same
# time keeps its connection, and the panel always drives the Studio.
STUDIO_PORT = 9886
APPROVAL_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "approval.py")
APPROVAL_TOOL = "mcp__gameforge_approval__ask"
# Tools that only look: they run without asking. Everything else goes through the panel's approval.
READ_ONLY_TOOLS = (
    "mcp__blender__get_scene_info",
    "mcp__blender__get_object_info",
    "mcp__blender__get_addon_status",
    "mcp__blender__look",
    "mcp__blender__search_assets",
)
_TOOL_LABELS = {
    "execute_blender_code": "Rodar código",
    "get_scene_info": "Ler a cena",
    "get_object_info": "Ler um objeto",
    "get_addon_status": "Conferir a conexão",
    "look": "Olhar a viewport",
    "search_assets": "Buscar modelos prontos",
    "import_asset": "Importar modelo pronto",
    "generate_3d": "Gerar modelo 3D",
}
_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def find_claude():
    """The Claude Code executable: on PATH or where Anthropic's installer puts it ("" when missing)."""
    for path in (shutil.which("claude") or "", os.path.join(os.path.expanduser("~"), ".local", "bin", "claude.exe")):
        if path and os.path.isfile(path):
            return path
    return ""


def auth_status(exe):
    """``claude auth status`` as a dict: ``loggedIn``, ``authMethod``, ``email``, ``subscriptionType``..."""
    try:
        done = subprocess.run([exe, "auth", "status"], stdin=subprocess.DEVNULL, capture_output=True, timeout=60,
                              creationflags=_NO_WINDOW)
        status = json.loads(done.stdout.decode("utf-8", "replace") or "{}")
        return status if isinstance(status, dict) else {"loggedIn": False}
    except (OSError, ValueError, subprocess.TimeoutExpired) as ex:
        return {"loggedIn": False, "error": str(ex)}


def blender_mcp_addon():
    """``(version, add-on file)`` of the MCP for Blender that ``uvx`` runs, or ``None`` without ``uv`` or the package.
    The Studio installs that same file, so the server and its add-on always match."""
    uvx = shutil.which("uvx")
    if not uvx:
        return None
    code = ("import importlib.metadata as m; from blender_mcp.addon_manager import get_bundled_addon_path as p; "
            "print(m.version('mcp-for-blender')); print(p())")
    for offline in (["--offline"], []):  # The cached copy answers at once; the first time it downloads.
        try:
            done = subprocess.run([uvx, *offline, "--from", "mcp-for-blender", "python", "-c", code],
                                  stdin=subprocess.DEVNULL, capture_output=True, timeout=300, creationflags=_NO_WINDOW)
        except (OSError, subprocess.TimeoutExpired):
            continue
        lines = done.stdout.decode("utf-8", "replace").strip().splitlines()
        if done.returncode == 0 and len(lines) >= 2 and os.path.isfile(lines[-1].strip()):
            return lines[-2].strip(), lines[-1].strip()
    return None


def write_mcp_config(path, *, python, approval_port, blender_version=""):
    """The run's MCP servers, written to ``path`` for ``--mcp-config``."""
    servers = {
        "gameforge_approval": {"command": python, "args": ["-I", APPROVAL_SCRIPT, str(approval_port)]},
    }
    uvx = shutil.which("uvx")
    if uvx and blender_version:
        servers["blender"] = {
            "command": uvx,
            "args": ["--from", "mcp-for-blender==" + blender_version, "mcp-for-blender"],
            "env": {"BLENDER_PORT": str(STUDIO_PORT), "DISABLE_TELEMETRY": "1"},
        }
    with open(path, "w", encoding="utf-8") as fh:
        json.dump({"mcpServers": servers}, fh, indent=1)


def command(exe, *, mcp_config, system, model="", session=""):
    """The ``claude`` command line of one request (the request itself goes through stdin)."""
    cmd = [
        exe, "-p", "--output-format", "stream-json", "--verbose",
        "--mcp-config", mcp_config, "--strict-mcp-config",
        # Only this run's settings: the user's own plugins, hooks and skills stay out of the Studio.
        "--setting-sources", "local", "--disable-slash-commands",
        # No terminal, file or web tools: the agent acts on the Studio through MCP only.
        "--tools", "",
        "--settings", json.dumps({"permissions": {"allow": list(READ_ONLY_TOOLS)}}),
        "--permission-prompt-tool", APPROVAL_TOOL,
        "--append-system-prompt", system,
    ]
    if model:
        cmd += ["--model", model]
    if session:
        cmd += ["--resume", session]
    return cmd


def tool_label(name):
    """A tool's name for people: "mcp__blender__look" -> "Olhar a viewport"."""
    short = name.rsplit("__", 1)[-1]
    return _TOOL_LABELS.get(short, short)


def _first_line(text, limit):
    line = next((ln.strip() for ln in str(text).splitlines() if ln.strip()), "")
    return line if len(line) <= limit else line[:limit - 1] + "…"


def _result_text(content):
    if isinstance(content, str):
        text = content
    else:
        parts = []
        for block in content or []:
            if block.get("type") == "text":
                parts.append(block.get("text", ""))
            elif block.get("type") == "image":
                parts.append("[imagem]")
        text = "\n".join(parts)
    try:  # MCP for Blender wraps its answers: {"result": "..."} or {"error": "..."}.
        data = json.loads(text)
    except ValueError:
        return text
    if isinstance(data, dict):
        for key in ("result", "error", "message"):
            if isinstance(data.get(key), str):
                return data[key]
    return text


def describe(event, *, show_cost=False):
    """The transcript lines of one stream-json event (none for the events that only carry state)."""
    kind = event.get("type")
    if kind == "assistant":
        lines = []
        for block in event.get("message", {}).get("content", []):
            if block.get("type") == "text" and block.get("text", "").strip():
                # The panel shows plain text: no Markdown bold or code marks.
                lines += block["text"].replace("**", "").replace("`", "").strip().splitlines()
            elif block.get("type") == "tool_use":
                lines.append("› " + tool_label(block.get("name", "")))
        return lines
    if kind == "user":
        content = event.get("message", {}).get("content")
        lines = []
        for block in content if isinstance(content, list) else []:
            if block.get("type") == "tool_result":
                text = _first_line(_result_text(block.get("content")), 160) or "ok"
                lines.append(("  ✗ " if block.get("is_error") else "  ↳ ") + text)
        return lines
    if kind == "result":
        if event.get("is_error"):
            return ["— erro: " + (_first_line(event.get("result") or event.get("subtype") or "", 200) or "sem detalhes")]
        line = "— pronto em {:.0f} s".format((event.get("duration_ms") or 0) / 1000.0)
        if show_cost and event.get("total_cost_usd"):
            line += " · US$ {:.4f}".format(event["total_cost_usd"])
        return [line]
    if kind == "system" and event.get("subtype") == "init":
        missing = [s.get("name", "?") for s in event.get("mcp_servers", []) if s.get("status") != "connected"]
        return ["aviso: sem conexão com " + ", ".join(missing)] if missing else []
    if kind == "rate_limit_event":
        if (event.get("rate_limit_info") or {}).get("status") == "rejected":
            return ["aviso: o limite de uso da sua conta Claude foi atingido"]
    return []
