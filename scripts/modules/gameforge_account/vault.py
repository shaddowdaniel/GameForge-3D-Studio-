# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Keeps the portal session token between runs, protected by the operating system:

- Windows: DPAPI (``CryptProtectData``, bound to the Windows user), stored in the config folder.
- macOS: the login Keychain (``security`` command line tool).
- Linux: the Secret Service (``secret-tool``) when present, otherwise a file only the user can read.

A missing or unreadable secret is not an error: the user simply signs in again.
"""

__all__ = (
    "Vault",
)

import os
import shutil
import subprocess
import sys

_SERVICE = "GameForge 3D Studio"
_ACCOUNT = "atomgameforge-session"
_FILE_NAME = "gameforge_session.bin"


class Vault:
    def __init__(self, config_dir, *, backend=None):
        self._config_dir = config_dir
        self._backend = backend or _default_backend()

    @property
    def _path(self):
        return os.path.join(self._config_dir, _FILE_NAME)

    def load(self):
        try:
            return self._backend.load(self._path)
        except Exception:
            return None

    def save(self, token):
        os.makedirs(self._config_dir, exist_ok=True)
        self._backend.save(self._path, token)

    def clear(self):
        try:
            self._backend.clear(self._path)
        except Exception:
            pass


def _default_backend():
    if sys.platform == "win32":
        return _DPAPIBackend()
    if sys.platform == "darwin" and shutil.which("security"):
        return _KeychainBackend()
    if sys.platform.startswith("linux") and shutil.which("secret-tool") and os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
        return _SecretServiceBackend()
    return _UserFileBackend()


class _UserFileBackend:
    """Plain file readable only by the current user (``0600``). Used where no OS store exists."""

    def load(self, path):
        with open(path, "rb") as fh:
            return self.decode(fh.read())

    def save(self, path, token):
        data = self.encode(token)
        tmp = path + ".tmp"
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
        os.replace(tmp, path)

    def clear(self, path):
        if os.path.exists(path):
            os.remove(path)

    @staticmethod
    def encode(token):
        return token.encode("utf-8")

    @staticmethod
    def decode(data):
        return data.decode("utf-8").strip() or None


class _DPAPIBackend(_UserFileBackend):

    @staticmethod
    def _crypt(data, protect):
        import ctypes
        from ctypes import wintypes

        class DATA_BLOB(ctypes.Structure):
            _fields_ = [("cbData", wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]

        crypt32 = ctypes.windll.crypt32
        kernel32 = ctypes.windll.kernel32
        buffer = ctypes.create_string_buffer(data, len(data))
        blob_in = DATA_BLOB(len(data), buffer)
        blob_out = DATA_BLOB()
        entropy_bytes = _SERVICE.encode("utf-8")
        entropy_buffer = ctypes.create_string_buffer(entropy_bytes, len(entropy_bytes))
        entropy = DATA_BLOB(len(entropy_bytes), entropy_buffer)
        function = crypt32.CryptProtectData if protect else crypt32.CryptUnprotectData
        # CRYPTPROTECT_UI_FORBIDDEN = 0x1.
        if not function(ctypes.byref(blob_in), None, ctypes.byref(entropy), None, None, 0x1, ctypes.byref(blob_out)):
            raise OSError("DPAPI falhou")
        try:
            return ctypes.string_at(blob_out.pbData, blob_out.cbData)
        finally:
            kernel32.LocalFree(blob_out.pbData)

    def encode(self, token):
        return self._crypt(token.encode("utf-8"), True)

    def decode(self, data):
        return self._crypt(data, False).decode("utf-8").strip() or None


class _KeychainBackend:

    def load(self, _path):
        result = subprocess.run(
            ["security", "find-generic-password", "-s", _SERVICE, "-a", _ACCOUNT, "-w"],
            capture_output=True, text=True, check=False,
        )
        return (result.stdout.strip() or None) if result.returncode == 0 else None

    def save(self, _path, token):
        # `-U` updates an existing item. The token is passed as an argument, which `security` requires;
        # it is only visible to the same user for the lifetime of this short process.
        subprocess.run(
            ["security", "add-generic-password", "-U", "-s", _SERVICE, "-a", _ACCOUNT, "-w", token],
            capture_output=True, check=True,
        )

    def clear(self, _path):
        subprocess.run(
            ["security", "delete-generic-password", "-s", _SERVICE, "-a", _ACCOUNT],
            capture_output=True, check=False,
        )


class _SecretServiceBackend:

    _ATTRS = ["service", _SERVICE, "account", _ACCOUNT]

    def load(self, _path):
        result = subprocess.run(["secret-tool", "lookup", *self._ATTRS], capture_output=True, text=True, check=False)
        return (result.stdout.strip() or None) if result.returncode == 0 else None

    def save(self, _path, token):
        # The secret goes through stdin, never the command line.
        subprocess.run(
            ["secret-tool", "store", "--label", _SERVICE, *self._ATTRS],
            input=token, text=True, capture_output=True, check=True,
        )

    def clear(self, _path):
        subprocess.run(["secret-tool", "clear", *self._ATTRS], capture_output=True, check=False)
