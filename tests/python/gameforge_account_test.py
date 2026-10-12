# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for ``gameforge_account`` (sign in with the atomgameforge.com account).

Runs with plain Python, no Studio build needed:
  python3 tests/python/gameforge_account_test.py
"""

import http.server
import json
import os
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts", "modules"))

from gameforge_account.portal import (  # noqa: E402
    Config,
    DeviceCode,
    NetworkError,
    Portal,
    PortalError,
    SessionInvalid,
    studio_included,
)
from gameforge_account.session import (  # noqa: E402
    ENTITLEMENTS_INTERVAL_SEC,
    HEARTBEAT_INTERVAL_SEC,
    NETWORK_GRACE_SEC,
    Session,
    State,
)
from gameforge_account.vault import Vault, _UserFileBackend  # noqa: E402


def entitlements(included=True, plan="pro"):
    return {
        "user_id": "11111111-2222-3333-4444-555555555555",
        "plan": {"code": plan, "name": plan.title()},
        "tools": [
            {"slug": "pwops", "included": True},
            {"slug": "estudio", "included": included},
        ],
    }


class FakeClock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


class MemoryVault:
    def __init__(self, token=None):
        self.token = token

    def load(self):
        return self.token

    def save(self, token):
        self.token = token

    def clear(self):
        self.token = None


class FakePortal:
    def __init__(self):
        self.polls = []
        self.ent = entitlements()
        self.ent_error = None
        self.heartbeat_error = None
        self.heartbeats = 0
        self.ent_calls = 0

    def device_code_start(self, fingerprint):
        self.fingerprint = fingerprint
        return DeviceCode("dev-secret", "ABCD-EFGH", "https://atomgameforge.com/link",
                          "https://atomgameforge.com/link?code=ABCD-EFGH", 600, 5)

    def device_code_poll(self, device_code):
        assert device_code == "dev-secret"
        result = self.polls.pop(0)
        if isinstance(result, Exception):
            raise result
        return result

    def entitlements(self, token):
        self.ent_calls += 1
        self.last_token = token
        if self.ent_error:
            raise self.ent_error
        return self.ent

    def heartbeat(self, token):
        self.heartbeats += 1
        if self.heartbeat_error:
            raise self.heartbeat_error
        return {"ok": True}


def make_session(portal, vault, clock):
    return Session(portal, vault, clock=clock, wall_clock=clock, fingerprint="fp", start_thread=False)


class StudioIncludedTest(unittest.TestCase):

    def test_included(self):
        self.assertTrue(studio_included(entitlements(True)))

    def test_not_included(self):
        self.assertFalse(studio_included(entitlements(False)))

    def test_fail_closed(self):
        self.assertFalse(studio_included({}))
        self.assertFalse(studio_included({"tools": None}))
        self.assertFalse(studio_included({"tools": [{"slug": "pwops", "included": True}]}))
        self.assertFalse(studio_included({"tools": [{"slug": "estudio", "included": "true"}]}))


class SessionLoginTest(unittest.TestCase):

    def setUp(self):
        self.portal = FakePortal()
        self.vault = MemoryVault()
        self.clock = FakeClock()
        self.session = make_session(self.portal, self.vault, self.clock)

    def test_starts_signed_out_without_token(self):
        self.assertIs(self.session.state.state, State.SIGNED_OUT)

    def test_device_code_flow(self):
        self.session.begin_login()
        self.session.step()
        snap = self.session.state
        self.assertIs(snap.state, State.WAITING_APPROVAL)
        self.assertEqual(snap.user_code, "ABCD-EFGH")
        self.assertIn("code=ABCD-EFGH", snap.verification_uri_complete)

        # Not due yet: no poll.
        self.session.step()
        self.assertEqual(self.portal.polls, [])

        self.portal.polls = [("pending", 5), ("approved", "tok-123")]
        self.clock.t += 5
        self.session.step()
        self.assertIs(self.session.state.state, State.WAITING_APPROVAL)
        self.clock.t += 5
        self.session.step()
        snap = self.session.state
        self.assertIs(snap.state, State.AUTHORIZED)
        self.assertEqual(snap.plan_code, "pro")
        self.assertEqual(self.vault.token, "tok-123")
        self.assertEqual(self.portal.last_token, "tok-123")

    def test_plan_without_studio(self):
        self.portal.ent = entitlements(included=False, plan="basic")
        self.session.begin_login()
        self.session.step()
        self.portal.polls = [("approved", "tok")]
        self.clock.t += 5
        self.session.step()
        snap = self.session.state
        self.assertIs(snap.state, State.NOT_INCLUDED)
        self.assertEqual(snap.plan_code, "basic")
        self.assertFalse(snap.authorized)

    def test_expired_code(self):
        self.session.begin_login()
        self.session.step()
        self.portal.polls = [PortalError("x", code="expired_token")]
        self.clock.t += 5
        self.session.step()
        self.assertIs(self.session.state.state, State.SIGNED_OUT)
        self.assertIn("expirou", self.session.state.message)

    def test_cancel(self):
        self.session.begin_login()
        self.session.step()
        self.session.cancel_login()
        self.assertIs(self.session.state.state, State.SIGNED_OUT)
        self.portal.polls = [("approved", "tok")]
        self.clock.t += 5
        self.session.step()
        self.assertIs(self.session.state.state, State.SIGNED_OUT)
        self.assertIsNone(self.vault.token)

    def test_network_error_while_polling_keeps_waiting(self):
        self.session.begin_login()
        self.session.step()
        self.portal.polls = [NetworkError("down"), ("approved", "tok")]
        self.clock.t += 5
        self.session.step()
        self.assertIs(self.session.state.state, State.WAITING_APPROVAL)
        self.clock.t += 10
        self.session.step()
        self.assertIs(self.session.state.state, State.AUTHORIZED)


class SessionSavedTokenTest(unittest.TestCase):

    def setUp(self):
        self.portal = FakePortal()
        self.vault = MemoryVault("saved")
        self.clock = FakeClock()
        self.session = make_session(self.portal, self.vault, self.clock)

    def test_saved_token_signs_in(self):
        self.assertIs(self.session.state.state, State.CHECKING)
        self.session.step()
        self.assertIs(self.session.state.state, State.AUTHORIZED)

    def test_revoked_token_signs_out_and_clears(self):
        self.portal.ent_error = SessionInvalid("revoked", status=401)
        self.session.step()
        self.assertIs(self.session.state.state, State.SIGNED_OUT)
        self.assertIsNone(self.vault.token)

    def test_offline_at_startup(self):
        self.portal.ent_error = NetworkError("down")
        self.session.step()
        self.assertIs(self.session.state.state, State.OFFLINE)
        self.assertEqual(self.vault.token, "saved")
        self.portal.ent_error = None
        self.session.retry()
        self.session.step()
        self.assertIs(self.session.state.state, State.AUTHORIZED)

    def test_heartbeat_and_recheck_schedule(self):
        self.session.step()
        self.clock.t += HEARTBEAT_INTERVAL_SEC
        self.session.step()
        self.assertEqual(self.portal.heartbeats, 1)
        calls = self.portal.ent_calls
        self.clock.t += ENTITLEMENTS_INTERVAL_SEC - HEARTBEAT_INTERVAL_SEC
        self.session.step()
        self.assertEqual(self.portal.ent_calls, calls + 1)

    def test_plan_downgrade_while_open(self):
        self.session.step()
        self.portal.ent = entitlements(included=False, plan="demo")
        self.clock.t += ENTITLEMENTS_INTERVAL_SEC
        self.session.step()
        self.assertIs(self.session.state.state, State.NOT_INCLUDED)

    def test_short_outage_keeps_authorized(self):
        self.session.step()
        self.portal.ent_error = NetworkError("down")
        self.clock.t += ENTITLEMENTS_INTERVAL_SEC
        self.session.step()
        self.assertIs(self.session.state.state, State.AUTHORIZED)
        self.clock.t += NETWORK_GRACE_SEC
        self.session.step()
        self.assertIs(self.session.state.state, State.OFFLINE)

    def test_sign_out(self):
        self.session.step()
        self.session.sign_out()
        self.assertIs(self.session.state.state, State.SIGNED_OUT)
        self.assertIsNone(self.vault.token)


class VaultTest(unittest.TestCase):

    def test_file_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            vault = Vault(os.path.join(tmp, "cfg"), backend=_UserFileBackend())
            self.assertIsNone(vault.load())
            vault.save("abc")
            self.assertEqual(vault.load(), "abc")
            if os.name == "posix":
                mode = os.stat(os.path.join(tmp, "cfg", "gameforge_session.bin")).st_mode & 0o777
                self.assertEqual(mode, 0o600)
            vault.clear()
            self.assertIsNone(vault.load())


class _Handler(http.server.BaseHTTPRequestHandler):
    routes = {}
    seen = []

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("content-length", 0)))
        _Handler.seen.append((self.path, {k.lower(): v for k, v in self.headers.items()}, json.loads(body or b"{}")))
        status, payload = _Handler.routes[self.path]
        data = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


class PortalHttpTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.server = http.server.HTTPServer(("127.0.0.1", 0), _Handler)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        url = "http://127.0.0.1:{:d}".format(cls.server.server_address[1])
        cls.portal = Portal(Config(url, "anon-key", "https://x/planos"), app_version="t", os_name="os")

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def setUp(self):
        _Handler.seen.clear()

    def test_start_and_poll(self):
        _Handler.routes["/functions/v1/device-code/start"] = (200, {
            "device_code": "d", "user_code": "WXYZ-2345",
            "verification_uri": "https://atomgameforge.com/link",
            "verification_uri_complete": "https://atomgameforge.com/link?code=WXYZ-2345",
            "expires_in": 600, "interval": 5})
        dc = self.portal.device_code_start("fp")
        self.assertEqual(dc.user_code, "WXYZ-2345")
        path, headers, body = _Handler.seen[0]
        self.assertEqual(body["client"], "estudio")
        self.assertEqual(headers["apikey"], "anon-key")

        _Handler.routes["/functions/v1/device-code/poll"] = (200, {"status": "authorization_pending", "interval": 5})
        self.assertEqual(self.portal.device_code_poll("d"), ("pending", 5))
        _Handler.routes["/functions/v1/device-code/poll"] = (200, {"status": "approved", "session_token": "tok"})
        self.assertEqual(self.portal.device_code_poll("d"), ("approved", "tok"))
        _Handler.routes["/functions/v1/device-code/poll"] = (200, {"status": "access_denied"})
        with self.assertRaises(PortalError) as cm:
            self.portal.device_code_poll("d")
        self.assertEqual(cm.exception.code, "access_denied")

    def test_entitlements_uses_session_header(self):
        _Handler.routes["/functions/v1/entitlements"] = (200, entitlements())
        self.assertTrue(studio_included(self.portal.entitlements("tok")))
        self.assertEqual(_Handler.seen[0][1]["x-session-token"], "tok")

    def test_errors(self):
        _Handler.routes["/functions/v1/entitlements"] = (401, {"error": "session expired", "code": "session_invalid"})
        with self.assertRaises(SessionInvalid):
            self.portal.entitlements("tok")
        _Handler.routes["/functions/v1/entitlements"] = (502, {})
        with self.assertRaises(NetworkError):
            self.portal.entitlements("tok")

    def test_unreachable(self):
        portal = Portal(Config("http://127.0.0.1:9", "k", ""), app_version="", os_name="")
        with self.assertRaises(NetworkError):
            portal.heartbeat("tok")


if __name__ == "__main__":
    unittest.main()
