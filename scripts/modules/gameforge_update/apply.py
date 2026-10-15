# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Applies an update of the GameForge 3D Studio after it closes. Run by the Studio's own Python, standard library
only, in two roles:

    python -I apply.py watch <install dir> <package.zip> <envelope.json> <log> [--dry-run]
        As the user: waits until no Studio window is open, asks Windows for permission (UAC) to run the "apply"
        role, waits for it and opens the Studio again (as the user, never elevated).

    python -I apply.py apply <install dir> <package.zip> <envelope.json> <log> [--dry-run]
        As administrator: copies the package into the installation (out of the user's reach), checks the portal's
        signature and the package's SHA-256 again, and applies it (``package.apply_update``).

``--dry-run`` only exercises the permission and the restart. Every outcome ends in a "RESULTADO ..." line of the
log, which the Studio shows when it opens.
"""

import ctypes
import json
import os
import shutil
import subprocess
import sys
import time
from ctypes import wintypes

# `-I` keeps the user's environment and this folder out of `sys.path`: the package is imported from here.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from gameforge_update import package, signature  # noqa: E402

STUDIO_EXE = "GameForge3DStudio.exe"
LAUNCHER_EXE = "GameForge3DStudio-launcher.exe"
_PACKAGE_COPY = ".gameforge-package.zip"
_NO_WINDOW = 0x08000000


def log(path, text):
    with open(path, "a", encoding="utf-8") as fh:
        fh.write(time.strftime("%Y-%m-%d %H:%M:%S ") + text + "\n")


def studio_running(tasklist_output=None):
    """True while any GameForge3DStudio.exe runs (every window of the Studio). ``tasklist`` writes the OEM code
    page, which the ANSI one cannot always decode (German, Polish, Russian...): the bytes are compared as they are."""
    if tasklist_output is None:
        tasklist_output = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq " + STUDIO_EXE, "/FO", "CSV", "/NH"],
            capture_output=True, creationflags=_NO_WINDOW).stdout
    return '"{:s}"'.format(STUDIO_EXE.lower()).encode("ascii") in tasklist_output.lower()


def writable_by_user(install_dir):
    """True when this process, not elevated, can write the installation (a folder of the user's): then the update
    needs no permission, and elevating would run an apply.py that any program of the user could have changed."""
    probe = os.path.join(install_dir, ".gameforge-write-test")
    try:
        with open(probe, "wb"):
            pass
        os.remove(probe)
    except OSError:
        return False
    return True


def elevated_parameters(arguments):
    """The command line of the elevated run: ``-I``, this script, then ``arguments``."""
    return subprocess.list2cmdline(["-I", os.path.abspath(__file__), *arguments])


class _ShellExecuteInfo(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD), ("fMask", wintypes.ULONG), ("hwnd", wintypes.HWND),
        ("lpVerb", wintypes.LPCWSTR), ("lpFile", wintypes.LPCWSTR), ("lpParameters", wintypes.LPCWSTR),
        ("lpDirectory", wintypes.LPCWSTR), ("nShow", ctypes.c_int), ("hInstApp", wintypes.HINSTANCE),
        ("lpIDList", ctypes.c_void_p), ("lpClass", wintypes.LPCWSTR), ("hkeyClass", wintypes.HKEY),
        ("dwHotKey", wintypes.DWORD), ("hIconOrMonitor", wintypes.HANDLE), ("hProcess", wintypes.HANDLE),
    ]


def run_elevated(arguments):
    """Runs this script as administrator (the UAC prompt) and waits for it. The exit code, or ``None`` when Windows
    did not give the permission."""
    shell32, kernel32 = ctypes.windll.shell32, ctypes.windll.kernel32
    shell32.ShellExecuteExW.argtypes = [ctypes.POINTER(_ShellExecuteInfo)]
    shell32.ShellExecuteExW.restype = wintypes.BOOL
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    info = _ShellExecuteInfo(cbSize=ctypes.sizeof(_ShellExecuteInfo), fMask=0x40,  # SEE_MASK_NOCLOSEPROCESS
                             lpVerb="runas", lpFile=sys.executable, lpParameters=elevated_parameters(arguments),
                             nShow=0)
    if not shell32.ShellExecuteExW(ctypes.byref(info)) or not info.hProcess:
        return None
    kernel32.WaitForSingleObject(info.hProcess, 0xFFFFFFFF)
    code = wintypes.DWORD()
    kernel32.GetExitCodeProcess(info.hProcess, ctypes.byref(code))
    kernel32.CloseHandle(info.hProcess)
    return code.value


def watch(install_dir, package_path, envelope_path, log_path, dry_run=False):
    log(log_path, "esperando o Studio fechar")
    while studio_running():
        time.sleep(1.0)
    if writable_by_user(install_dir):
        code = apply(install_dir, package_path, envelope_path, log_path, dry_run)
    else:
        code = run_elevated(["apply", install_dir, package_path, envelope_path, log_path] +
                            (["--dry-run"] if dry_run else []))
    if code is None:
        log(log_path, "RESULTADO adiado: o Windows não deu a permissão; o Studio oferece de novo na próxima vez")
    elif code != 0:
        log(log_path, "o aplicador terminou com o código {:d}".format(code))
    for name in (LAUNCHER_EXE, STUDIO_EXE):
        exe = os.path.join(install_dir, name)
        if os.path.isfile(exe):
            subprocess.Popen([exe], cwd=install_dir, close_fds=True)
            break


def apply(install_dir, package_path, envelope_path, log_path, dry_run=False):
    if dry_run:
        log(log_path, "RESULTADO teste: a permissão e o reinício funcionam")
        return 0
    copy = os.path.join(install_dir, _PACKAGE_COPY)
    try:
        with open(envelope_path, encoding="utf-8") as fh:
            envelope = json.load(fh)
        # First the copy, in a folder only administrators write: the package cannot change after the check.
        shutil.copyfile(package_path, copy)
        if not signature.envelope_ok(envelope) or package.file_sha256(copy) != envelope.get("file_hash"):
            raise package.UpdateError("o pacote não confere com a assinatura do portal")
        version = package.apply_update(install_dir, copy, log=lambda text: log(log_path, text))
        log(log_path, "RESULTADO atualizado para " + version)
        return 0
    except Exception as ex:  # noqa: BLE001 - every failure ends in the log the Studio shows
        log(log_path, "RESULTADO falhou: {:s}".format(str(ex)))
        return 1
    finally:
        if os.path.exists(copy):
            os.remove(copy)


def main(argv):
    if len(argv) < 6 or argv[1] not in {"watch", "apply"}:
        print(__doc__)
        return 2
    role, install_dir, package_path, envelope_path, log_path = argv[1:6]
    dry_run = "--dry-run" in argv[6:]
    if role == "watch":
        watch(install_dir, package_path, envelope_path, log_path, dry_run)
        return 0
    return apply(install_dir, package_path, envelope_path, log_path, dry_run)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
