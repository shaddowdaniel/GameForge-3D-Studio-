# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Client for the Studio's port of PWOps (``pwops.atomgameforge.com/api/studio/*``).

The character preview reads the characters saved in the user's account and the assembled look of
one of them (body, equipment, weapons, wings, face), as model paths inside the game's ``.pck``
packages. The models themselves are read from the local client by the native Angelica importer.

Authentication is the account session of the Studio (``x-session-token``); the plan must include
the Studio. Pure Python standard library, no ``bpy``.
"""

__all__ = (
    "Character",
    "PWOps",
    "PWOpsError",
    "parse_try_on",
)

import json
import os
import ssl
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass

from .portal import (
    NetworkError,
    PortalError,
    SessionInvalid,
    _ssl_context,
)

_DEFAULT_URL = "https://pwops.atomgameforge.com"
_TIMEOUT_SEC = 20.0
# The port accepts at most this many items to try on at once.
MAX_TRY_ON = 8


class PWOpsError(PortalError):
    """PWOps refused the request (``code`` is the port's own code, for example ``plano_sem_estudio``)."""


@dataclass(frozen=True)
class Character:
    account_id: str
    server_group: str
    role_id: int
    name: str
    class_name: str
    level: int
    occupation: int
    gender: int

    @property
    def label(self):
        server = " · " + self.server_group if self.server_group else ""
        return "{:s} (nv. {:d}{:s})".format(self.name or str(self.role_id), self.level, server)

    @classmethod
    def from_json(cls, item):
        def as_int(key):
            try:
                return int(item.get(key) or 0)
            except (TypeError, ValueError):
                return 0
        return cls(
            account_id=str(item.get("accountId") or ""),
            server_group=str(item.get("serverGroup") or ""),
            role_id=as_int("roleid"),
            name=str(item.get("name") or ""),
            class_name=str(item.get("class") or ""),
            level=as_int("level"),
            occupation=as_int("occupation"),
            gender=as_int("gender"),
        )


def parse_try_on(text):
    """Item ids typed by the user (``"1234, 5678"``): integers only, at most #MAX_TRY_ON."""
    ids = []
    for word in str(text or "").replace(";", ",").replace(" ", ",").split(","):
        word = word.strip()
        if word.isdigit() and int(word) > 0 and int(word) not in ids:
            ids.append(int(word))
    return ids[:MAX_TRY_ON]


class PWOps:

    def __init__(self, base_url=None, *, opener=None):
        self.base_url = (base_url or os.environ.get("GAMEFORGE_PWOPS_URL", _DEFAULT_URL)).rstrip("/")
        self._opener = opener or urllib.request.build_opener(
            urllib.request.HTTPSHandler(context=_ssl_context()))

    def _get(self, path, session_token, query=None):
        if not session_token:
            raise SessionInvalid("entre com a conta atomgameforge.com", code="no_token")
        url = self.base_url + path
        if query:
            url += "?" + urllib.parse.urlencode(query)
        request = urllib.request.Request(url, headers={
            "accept": "application/json",
            "x-session-token": session_token,
        }, method="GET")
        try:
            with self._opener.open(request, timeout=_TIMEOUT_SEC) as response:
                payload = response.read()
                status = response.status
        except urllib.error.HTTPError as ex:
            status = ex.code
            payload = ex.read() or b""
        except (urllib.error.URLError, OSError, ssl.SSLError) as ex:
            raise NetworkError("Não foi possível conectar ao PWOps ({:s})".format(str(ex))) from ex

        try:
            result = json.loads(payload.decode("utf-8")) if payload else {}
        except ValueError:
            result = {}
        if not isinstance(result, dict):
            result = {}
        code = result.get("code")
        message = result.get("error") or "erro {:d}".format(status)

        if status == 401:
            raise SessionInvalid(message, status=status, code=code)
        if status >= 500 or status == 429:
            raise NetworkError("PWOps: " + message, status=status, code=code)
        if status >= 400 or result.get("ok") is False:
            raise PWOpsError(message, status=status, code=code)
        return result

    def characters(self, session_token):
        """The characters saved in the account (every linked game account)."""
        result = self._get("/api/studio/characters", session_token)
        items = result.get("characters")
        if not isinstance(items, list):
            raise PWOpsError("resposta inesperada de /api/studio/characters")
        return [Character.from_json(item) for item in items if isinstance(item, dict)]

    def appearance(self, session_token, role_id, *, try_on=(), fashion=None):
        """
        The assembled look of a character, as JSON text for the native importer
        (``bpy.ops.wm.angelica_character_import(spec_json=...)``).

        ``try_on``: item ids to wear only in this answer. ``fashion``: force fashion mode on or off.
        """
        query = {"roleId": str(int(role_id))}
        ids = parse_try_on(",".join(str(i) for i in try_on))
        if ids:
            query["provando"] = ",".join(str(i) for i in ids)
        if fashion is not None:
            query["fashion"] = "1" if fashion else "0"
        result = self._get("/api/studio/appearance", session_token, query)
        character = result.get("character")
        if not isinstance(character, dict) or not character.get("smdPath"):
            raise PWOpsError("o PWOps não devolveu o modelo deste personagem")
        return json.dumps({"character": character}, ensure_ascii=False)
