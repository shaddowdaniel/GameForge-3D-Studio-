# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The Studio's account session: sign in, check the plan includes the Studio, keep the session alive.

Network calls run on a worker thread; the UI only reads ``state`` (a snapshot) and calls the
``begin_login``/``cancel_login``/``sign_out``/``retry`` methods, then redraws when ``revision`` changes.
"""

__all__ = (
    "State",
    "Snapshot",
    "Session",
)

import enum
import hashlib
import platform
import threading
import time
import uuid
from dataclasses import dataclass, field

from .portal import (
    NetworkError,
    PortalError,
    SessionInvalid,
    studio_included,
)

# How often the open Studio confirms the session is alive and the plan still includes it. The
# portal session lasts 8 hours from the last heartbeat.
HEARTBEAT_INTERVAL_SEC = 30 * 60
ENTITLEMENTS_INTERVAL_SEC = 60 * 60
# A network failure while the Studio is open does not lock the user out at once: retry for this
# long before asking to sign in again (the work in progress is never discarded).
NETWORK_GRACE_SEC = 15 * 60
RETRY_INTERVAL_SEC = 60


class State(enum.Enum):
    CHECKING = "checking"            # Validating a saved token.
    SIGNED_OUT = "signed_out"        # Needs to sign in.
    WAITING_APPROVAL = "waiting"     # Showing the code, waiting for approval on atomgameforge.com/link.
    AUTHORIZED = "authorized"        # Signed in and the plan includes the Studio.
    NOT_INCLUDED = "not_included"    # Signed in, but the plan does not include the Studio.
    OFFLINE = "offline"              # Could not reach the portal.


@dataclass(frozen=True)
class Snapshot:
    state: State
    revision: int
    message: str = ""
    user_code: str = ""
    verification_uri: str = ""
    verification_uri_complete: str = ""
    code_expires_at: float = 0.0
    plan_code: str = ""
    plan_name: str = ""
    user_id: str = ""
    entitlements: dict = field(default_factory=dict)

    @property
    def authorized(self):
        return self.state is State.AUTHORIZED


def machine_fingerprint():
    """Stable per machine and not personally identifying: the portal uses it to count devices."""
    node = "{:s}|{:012x}|{:s}".format(platform.node(), uuid.getnode(), platform.system())
    return "gfs-" + hashlib.sha256(node.encode("utf-8")).hexdigest()[:40]


class Session:

    def __init__(
            self, portal, vault, *,
            clock=time.monotonic, wall_clock=time.time, fingerprint=None, start_thread=True,
    ):
        self._portal = portal
        self._vault = vault
        self._clock = clock
        self._wall_clock = wall_clock
        self._fingerprint = fingerprint or machine_fingerprint()
        self._lock = threading.Lock()
        self._wake = threading.Event()
        self._stop = False
        self._token = None
        self._device = None
        self._next_poll = 0.0
        self._next_heartbeat = 0.0
        self._next_entitlements = 0.0
        self._offline_since = None
        self._snapshot = Snapshot(State.CHECKING, 0)
        self._thread = None
        self._token = self._vault.load()
        if not self._token:
            self._set(State.SIGNED_OUT)
        if start_thread:
            self._thread = threading.Thread(target=self._run, name="gameforge-account", daemon=True)
            self._thread.start()

    # Public API (any thread).

    @property
    def state(self):
        return self._snapshot

    def begin_login(self):
        with self._lock:
            if self._snapshot.state in {State.WAITING_APPROVAL, State.AUTHORIZED}:
                return
            self._device = None
            self._set_locked(State.WAITING_APPROVAL, message="Gerando o código...")
        self._wake.set()

    def cancel_login(self):
        with self._lock:
            self._device = None
            if self._snapshot.state is State.WAITING_APPROVAL:
                self._set_locked(State.SIGNED_OUT)

    def sign_out(self):
        with self._lock:
            self._token = None
            self._device = None
            self._vault.clear()
            self._set_locked(State.SIGNED_OUT)

    def retry(self):
        with self._lock:
            if self._token:
                self._next_entitlements = 0.0
                self._set_locked(State.CHECKING, message="Verificando sua conta...", keep_account=True)
            else:
                self._set_locked(State.SIGNED_OUT)
        self._wake.set()

    def shutdown(self):
        self._stop = True
        self._wake.set()

    def step(self):
        """One pass of the worker: does whatever is due now. Returns seconds until the next due work."""
        now = self._clock()
        state = self._snapshot.state
        if state is State.WAITING_APPROVAL:
            return self._step_login(now)
        if state is State.CHECKING and self._token:
            return self._check_entitlements(now)
        if state in {State.AUTHORIZED, State.NOT_INCLUDED, State.OFFLINE} and self._token:
            if now >= self._next_entitlements:
                return self._check_entitlements(now)
            if state is State.AUTHORIZED and now >= self._next_heartbeat:
                return self._heartbeat(now)
            return max(0.0, min(self._next_entitlements, self._next_heartbeat) - now)
        return 3600.0

    # Worker.

    def _run(self):
        while not self._stop:
            try:
                delay = self.step()
            except Exception as ex:  # Never let the worker die: report and retry later.
                self._set(State.OFFLINE, message="Erro inesperado: {:s}".format(str(ex)), keep_account=True)
                delay = RETRY_INTERVAL_SEC
            self._wake.wait(timeout=max(0.2, delay))
            self._wake.clear()

    def _step_login(self, now):
        device = self._device
        if device is None:
            try:
                device = self._portal.device_code_start(self._fingerprint)
            except PortalError as ex:
                self._set(State.OFFLINE if isinstance(ex, NetworkError) else State.SIGNED_OUT, message=str(ex))
                return RETRY_INTERVAL_SEC
            with self._lock:
                if self._snapshot.state is not State.WAITING_APPROVAL:
                    return 3600.0  # Cancelled meanwhile.
                self._device = device
                self._next_poll = now + device.interval
                self._set_locked(
                    State.WAITING_APPROVAL,
                    user_code=device.user_code,
                    verification_uri=device.verification_uri,
                    verification_uri_complete=device.verification_uri_complete,
                    code_expires_at=self._wall_clock() + device.expires_in,
                )
            return device.interval

        if now < self._next_poll:
            return self._next_poll - now
        try:
            status, value = self._portal.device_code_poll(device.device_code)
        except NetworkError:
            self._next_poll = now + device.interval * 2
            return device.interval * 2
        except PortalError as ex:
            if ex.code == "expired_token":
                message = "O código expirou. Gere um novo."
            else:
                message = "O acesso foi negado no site."
            with self._lock:
                if self._device is device:
                    self._device = None
                    self._set_locked(State.SIGNED_OUT, message=message)
            return 3600.0
        if status == "pending":
            self._next_poll = now + value
            return value
        with self._lock:
            if self._device is not device:
                return 3600.0
            self._device = None
            self._token = value
            self._set_locked(State.CHECKING, message="Verificando seu plano...")
        try:
            self._vault.save(value)
        except Exception as ex:
            print("GameForge: não foi possível guardar a sessão:", ex)
        return self._check_entitlements(self._clock())

    def _check_entitlements(self, now):
        token = self._token
        try:
            ent = self._portal.entitlements(token)
        except SessionInvalid:
            with self._lock:
                if self._token == token:
                    self._token = None
                    self._vault.clear()
                    self._set_locked(State.SIGNED_OUT, message="Sua sessão terminou. Entre de novo.")
            return 3600.0
        except NetworkError as ex:
            return self._network_failure(now, str(ex))
        except PortalError as ex:
            return self._network_failure(now, str(ex))

        self._offline_since = None
        self._next_entitlements = now + ENTITLEMENTS_INTERVAL_SEC
        if self._snapshot.state is not State.AUTHORIZED:
            self._next_heartbeat = now + HEARTBEAT_INTERVAL_SEC
        plan = ent.get("plan") if isinstance(ent.get("plan"), dict) else {}
        fields = dict(
            plan_code=str(plan.get("code") or ""),
            plan_name=str(plan.get("name") or ""),
            user_id=str(ent.get("user_id") or ""),
            entitlements=ent,
        )
        with self._lock:
            if self._token != token:
                return 0.0
            if studio_included(ent):
                self._set_locked(State.AUTHORIZED, **fields)
            else:
                message = "Seu plano não inclui o GameForge Studio."
                self._set_locked(State.NOT_INCLUDED, message=message, **fields)
        return min(ENTITLEMENTS_INTERVAL_SEC, HEARTBEAT_INTERVAL_SEC)

    def _heartbeat(self, now):
        token = self._token
        try:
            self._portal.heartbeat(token)
        except SessionInvalid:
            # Revoked, or the device was removed in the portal: confirm through entitlements.
            self._next_entitlements = now
            return 0.0
        except PortalError as ex:
            return self._network_failure(now, str(ex))
        self._next_heartbeat = now + HEARTBEAT_INTERVAL_SEC
        return max(0.0, min(self._next_entitlements, self._next_heartbeat) - now)

    def _network_failure(self, now, message):
        state = self._snapshot.state
        if self._offline_since is None:
            self._offline_since = now
        # While the Studio is already open and authorized, keep working through short outages.
        if state is State.AUTHORIZED and now - self._offline_since < NETWORK_GRACE_SEC:
            self._next_entitlements = now + RETRY_INTERVAL_SEC
            self._next_heartbeat = now + RETRY_INTERVAL_SEC
            return RETRY_INTERVAL_SEC
        self._next_entitlements = now + RETRY_INTERVAL_SEC
        self._set(State.OFFLINE, message=message, keep_account=True)
        return RETRY_INTERVAL_SEC

    # State.

    def _set(self, state, **kw):
        with self._lock:
            self._set_locked(state, **kw)

    def _set_locked(self, state, *, keep_account=False, **kw):
        prev = self._snapshot
        if keep_account:
            for key in ("plan_code", "plan_name", "user_id", "entitlements"):
                kw.setdefault(key, getattr(prev, key))
        self._snapshot = Snapshot(state, prev.revision + 1, **kw)
