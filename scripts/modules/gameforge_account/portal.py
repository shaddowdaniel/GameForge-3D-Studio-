# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Client for the atomgameforge.com account portal (Supabase edge functions).

The Studio signs in with the portal's device authorization grant (RFC 8628), the same flow the
AtomLauncher and the 155dll use: ``device-code/start`` gives a short user code, the person approves
it on ``atomgameforge.com/link``, and ``device-code/poll`` hands back a ``session_token`` once.
That token goes in ``x-session-token`` for ``entitlements`` (plan and tools) and
``heartbeat-session`` (keeps the 8 hour session alive).

Pure Python standard library, no ``bpy``, so it can be tested outside of the Studio.
"""

__all__ = (
    "Config",
    "PortalError",
    "NetworkError",
    "SessionInvalid",
    "DeviceCode",
    "Portal",
    "STUDIO_TOOL_SLUG",
    "studio_included",
)

import json
import os
import ssl
import urllib.error
import urllib.request
from dataclasses import dataclass

# The Studio's slug in the portal's `tools` catalog (migration 20260723204543 of the portal).
STUDIO_TOOL_SLUG = "estudio"

# Public values: the same project URL and publishable (anon) key the atomgameforge.com website ships
# to every browser. They only identify the project; access is decided by the user's session.
_DEFAULT_SUPABASE_URL = "https://cbwkbwaicqrqnjoopcju.supabase.co"
_DEFAULT_ANON_KEY = (
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
    "eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImNid2tid2FpY3FycW5qb29wY2p1Iiwicm9sZSI6ImFub24i"
    "LCJpYXQiOjE3Nzk4NjA1OTUsImV4cCI6MjA5NTQzNjU5NX0."
    "Kj-SzvWHw57gtOUFrMY1NlffuwxIA8U1-H4e6vEcj6w"
)
_DEFAULT_PLANS_URL = "https://atomgameforge.com/planos"
_TIMEOUT_SEC = 15.0
_DOWNLOAD_TIMEOUT_SEC = 120.0


@dataclass(frozen=True)
class Config:
    supabase_url: str
    anon_key: str
    plans_url: str

    @classmethod
    def from_environment(cls):
        """Defaults, overridable per variable (testing against a staging portal, for example)."""
        env = os.environ.get
        return cls(
            supabase_url=env("GAMEFORGE_PORTAL_SUPABASE_URL", _DEFAULT_SUPABASE_URL).rstrip("/"),
            anon_key=env("GAMEFORGE_PORTAL_ANON_KEY", _DEFAULT_ANON_KEY),
            plans_url=env("GAMEFORGE_PORTAL_PLANS_URL", _DEFAULT_PLANS_URL),
        )


class PortalError(Exception):
    """The portal answered with an error the user should see (``code`` is the portal's own code)."""

    def __init__(self, message, *, status=0, code=None):
        super().__init__(message)
        self.status = status
        self.code = code


class NetworkError(PortalError):
    """The portal could not be reached (no connection, DNS, timeout, TLS, 5xx)."""


class SessionInvalid(PortalError):
    """The session token is expired, revoked, or its device was removed: sign in again."""


@dataclass(frozen=True)
class DeviceCode:
    device_code: str
    user_code: str
    verification_uri: str
    verification_uri_complete: str
    expires_in: int
    interval: int


def studio_included(entitlements):
    """
    True when the user's plan includes the GameForge Studio.

    Fail-closed: a missing or malformed ``tools`` list does not grant access (unlike PWOps, which
    predates the catalog and lets an absent list through).
    """
    tools = entitlements.get("tools") if isinstance(entitlements, dict) else None
    if not isinstance(tools, list):
        return False
    for tool in tools:
        if isinstance(tool, dict) and tool.get("slug") == STUDIO_TOOL_SLUG:
            return tool.get("included") is True
    return False


def _ssl_context():
    context = ssl.create_default_context()
    # Windows and macOS builds of Python read the system store; fall back to `certifi` where the
    # bundled OpenSSL has no default paths (some Linux builds), when it is available.
    if not context.get_ca_certs():
        try:
            import certifi
        except ImportError:
            pass
        else:
            context.load_verify_locations(certifi.where())
    return context


class Portal:
    """Thin, synchronous client. Every call raises ``PortalError`` (or a subclass) on failure."""

    def __init__(self, config=None, *, opener=None, app_version="", os_name=""):
        self.config = config or Config.from_environment()
        self._opener = opener or urllib.request.build_opener(
            urllib.request.HTTPSHandler(context=_ssl_context()))
        self._app_version = app_version
        self._os_name = os_name

    def _call(self, function, *, body=None, session_token=None, bearer=None):
        url = "{:s}/functions/v1/{:s}".format(self.config.supabase_url, function)
        headers = {
            "content-type": "application/json",
            "apikey": self.config.anon_key,
            "authorization": "Bearer " + (bearer or self.config.anon_key),
        }
        if session_token:
            headers["x-session-token"] = session_token
        data = json.dumps(body if body is not None else {}).encode("utf-8")
        request = urllib.request.Request(url, data=data, headers=headers, method="POST")
        try:
            with self._opener.open(request, timeout=_TIMEOUT_SEC) as response:
                payload = response.read()
                status = response.status
        except urllib.error.HTTPError as ex:
            status = ex.code
            payload = ex.read() or b""
        except (urllib.error.URLError, OSError, ssl.SSLError) as ex:
            raise NetworkError("Não foi possível conectar a atomgameforge.com ({:s})".format(str(ex))) from ex

        try:
            result = json.loads(payload.decode("utf-8")) if payload else {}
        except ValueError:
            result = {}
        if not isinstance(result, dict):
            result = {}

        if status >= 500:
            raise NetworkError(
                "atomgameforge.com respondeu {:d}".format(status), status=status, code=result.get("code"))
        if status in {401, 403} and session_token:
            raise SessionInvalid(
                result.get("error") or "sessão inválida", status=status, code=result.get("code"))
        if status >= 400:
            raise PortalError(
                result.get("error") or "erro {:d}".format(status), status=status, code=result.get("code"))
        return result

    # Device authorization grant.

    def device_code_start(self, fingerprint):
        result = self._call("device-code/start", body={
            "fingerprint": fingerprint,
            "app_version": self._app_version,
            "os": self._os_name,
            "client": STUDIO_TOOL_SLUG,
        })
        try:
            return DeviceCode(
                device_code=str(result["device_code"]),
                user_code=str(result["user_code"]),
                verification_uri=str(result["verification_uri"]),
                verification_uri_complete=str(result.get("verification_uri_complete") or result["verification_uri"]),
                expires_in=int(result.get("expires_in", 600)),
                interval=max(1, int(result.get("interval", 5))),
            )
        except (KeyError, TypeError, ValueError) as ex:
            raise PortalError("resposta inesperada do device-code/start") from ex

    def device_code_poll(self, device_code):
        """
        Returns ``("approved", session_token)``, ``("pending", interval)``, or raises:
        ``PortalError`` with code ``expired_token`` or ``access_denied``.
        """
        result = self._call("device-code/poll", body={"device_code": device_code})
        status = result.get("status")
        if status == "approved" and result.get("session_token"):
            return "approved", str(result["session_token"])
        if status == "authorization_pending":
            return "pending", max(1, int(result.get("interval", 5)))
        if status == "expired_token":
            raise PortalError("o código expirou", code="expired_token")
        raise PortalError("o acesso foi negado", code="access_denied")

    # Session.

    def entitlements(self, session_token):
        result = self._call("entitlements", session_token=session_token)
        if not result.get("user_id"):
            raise PortalError("entitlements respondeu sem user_id")
        return result

    def heartbeat(self, session_token):
        return self._call("heartbeat-session", session_token=session_token)

    # Plugins (`plugin-download`): data and add-ons published in the portal's /admin, gated by plan.

    def plugin_versions(self, slug, session_token):
        """``{"current_version": str, "versions": [{"version", "published_at", "changelog"}]}``."""
        return self._call("plugin-download", body={"plugin_id": slug, "list": True}, session_token=session_token)

    def plugin_download(self, slug, session_token, version=None):
        """
        A short lived signed URL for one version (the current one by default):
        ``{"version", "download_url", "file_hash", "signature", "expires_in"}``.
        """
        body = {"plugin_id": slug}
        if version:
            body["version"] = version
        result = self._call("plugin-download", body=body, session_token=session_token)
        if not result.get("download_url") or not result.get("file_hash") or not result.get("version"):
            raise PortalError("resposta inesperada do plugin-download")
        return result

    def fetch(self, url, *, max_bytes):
        """Download a signed storage URL (no portal headers), refusing anything larger than ``max_bytes``."""
        request = urllib.request.Request(url, method="GET")
        try:
            with self._opener.open(request, timeout=_DOWNLOAD_TIMEOUT_SEC) as response:
                data = response.read(max_bytes + 1)
        except urllib.error.HTTPError as ex:
            raise PortalError("download recusado ({:d})".format(ex.code), status=ex.code) from ex
        except (urllib.error.URLError, OSError, ssl.SSLError) as ex:
            raise NetworkError("Não foi possível baixar de atomgameforge.com ({:s})".format(str(ex))) from ex
        if len(data) > max_bytes:
            raise PortalError("arquivo maior que o esperado")
        return data
