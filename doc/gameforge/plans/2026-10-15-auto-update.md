# Atualização automática do GameForge 3D Studio — plano de implementação

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Objetivo:** o Studio instalado se atualiza pelo portal atomgameforge.com, perguntando antes. Os pacotes
cumulativos são publicados no /admin › Versões, e o MSI completo fica no GitHub Releases.

**Arquitetura:**
- **Pacote de atualização:** um zip LZMA com o que mudou desde o último MSI, montado por
  `release/windows/gameforge/pack_update.py`, dentro do zip de release que o /admin aceita (`pwops-release.json`
  + `payload`).
- **No Studio:** ele consulta o `plugin-download` (`gameforge-3d-studio`), mostra o changelog e baixa o pacote.
  Confere a assinatura Ed25519 do portal com as mesmas chaves do launcher, em Python puro.
- **Instalação:** depois que o Studio fecha, `apply.py` pede o UAC uma vez, troca os arquivos (o antigo vira
  `.gfold`, e qualquer erro desfaz tudo) e reabre o Studio.
- **Manifestos:** o `gameforge-manifest.json` e o `gameforge-version.json` de cada instalação nascem no próprio
  passo de instalação do CMake.

**Tecnologias:**
- Python 3.13 do Studio, só biblioteca padrão (`zipfile` com LZMA, `ctypes`, `urllib`); Python 3.10+ do sistema
  para o empacotador;
- CMake `install(CODE)`;
- Supabase (conector);
- `gh` (GitHub Releases);
- React/TS no site.

**Spec:** `doc/gameforge/specs/2026-10-15-auto-update-design.md`

## Global Constraints

- **Instalação:** por máquina; aplicar uma atualização pede a permissão do Windows (UAC) uma vez.
- **Perguntar antes:** nada é baixado nem instalado sem o clique do usuário.
- **Plugin no portal:** `gameforge-3d-studio`, `required_plan = 'pro'`.
- **Zip de release:**
  - STORED, com `pwops-release.json` na raiz, `payload` = o pacote e `min_helper_version` = a versão base;
  - changelog com no máximo 600 caracteres;
  - versão `^[0-9A-Za-z.+-]{1,31}$`.
- **Pacote:** no máximo 45 MB; sem `.pdb`.
- **Assinatura:** Ed25519 sobre `JSON.stringify({plugin_id, version, file_path, file_hash, kid})`, com as chaves
  pinadas `key_ec46b5ca` e `key_7717f668`. O `file_path` sai da URL assinada
  (`/object/sign/plugin-files/<file_path>`).
- **MSI:** fica no GitHub Releases de `shaddowdaniel/GameForge-3D-Studio-`, tag `v<versão>`, arquivo
  `GameForge-3D-Studio-Setup.msi`. O link é
  `https://github.com/shaddowdaniel/GameForge-3D-Studio-/releases/latest/download/GameForge-3D-Studio-Setup.msi`.
- **GPL:** o código-fonte de cada release fica publicado (commit, push e a mesma tag) antes do release.
- **Aplicador:** só biblioteca padrão; roda com `5.4\python\bin\python.exe -I`.
- **Texto para o usuário:** em português e sem a palavra "Blender".
- **Commits, push, tag, release e PR:** só com o OK do usuário. O OK pedido na entrega deste plano cobre todos
  os do plano.

## Review Focus

- **Pasta com espaços ou acentos:** com a instalação em `C:\Program Files\ATOM GameForge\GameForge 3D Studio 5.4`
  ou num caminho como `D:\Jogos\Ação`, cada argumento do papel elevado chega inteiro. Teste:
  `ApplyTest.test_elevated_command_line_keeps_each_argument` (Task 4).
- **Download interrompido:** um download que cai no meio nunca vira pacote. O `.part` é apagado e a próxima
  tentativa começa do zero. Teste: `FetchTest.test_truncated_download_fails` (Task 5).
- **Caminho que sai da instalação:** um pacote com `../`, unidade ou caminho absoluto é recusado antes de gravar
  qualquer coisa. Teste: `ApplyTest.test_refusals_change_nothing` (Task 4).
- **Arquivo em uso:** um arquivo travado por antivírus ou outro programa durante a troca faz tudo voltar como
  estava, e o log diz qual arquivo foi. Teste: `ApplyTest.test_file_in_use_puts_everything_back` (Task 4).
- **Versão igual ou mais velha:** o portal com uma versão igual ou mais velha (republicação, retirada) nunca vira
  oferta de atualização. Testes: `VersionTest.test_newer` (Task 1) e `DownloadTest.test_refusals` (Task 5).

---

## Mapa dos arquivos

| Arquivo | Responsabilidade |
|---------|------------------|
| `scripts/modules/gameforge_update/ed25519.py` (novo) | Ed25519 RFC 8032 em Python puro |
| `scripts/modules/gameforge_update/versions.py` (novo) | `ver_cmp`, versão instalada, versões novas |
| `scripts/modules/gameforge_update/signature.py` (novo) | Chaves pinadas, envelope da resposta do portal, conferência |
| `scripts/modules/gameforge_update/package.py` (novo) | Ler e aplicar um pacote (troca com volta atrás) |
| `scripts/modules/gameforge_update/apply.py` (novo) | Script do aplicador: papéis `watch` e `apply` |
| `scripts/modules/gameforge_update/__init__.py` (novo) | Lado do Studio: download conferido, iniciar o aplicador, último resultado |
| `scripts/modules/gameforge_account/portal.py` (muda) | `Portal.fetch_to_file`: download em partes com SHA-256 e progresso |
| `scripts/startup/bl_operators/gameforge_update.py` (novo) | Procura em segundo plano, botão na barra superior, diálogo, instalar |
| `scripts/startup/bl_operators/__init__.py` (muda) | Registra `gameforge_update` |
| `release/windows/gameforge/write_manifest.py` (novo) | Manifesto e versão da instalação (no passo de instalação) |
| `release/windows/gameforge/pack_update.py` (novo) | Empacotador da atualização e da base do MSI |
| `source/creator/CMakeLists.txt` (muda, fim) | `install(CODE)` que roda o `write_manifest.py` |
| `source/blender/blenkernel/BKE_blender_version.h` (muda) | Ciclo `release` (o título deixa de dizer "Alpha") |
| `tests/python/gameforge_update_test.py` (novo) | Testes de tudo acima, com Python comum |
| `D:\claude-scratch\gf-tools\deploy_install.py` (muda) | Copia os dois `.json` para a instalação de desenvolvimento |
| `D:\claude-scratch\gf-tools\update_gui_check.py` (novo) | Print da barra superior e do diálogo |
| `D:\claude-scratch\atomgameforge-site\src\pages\Account.tsx` (muda) | Botão "Baixar o GameForge 3D Studio" no card |

Comando dos testes (repo do Studio): `python tests/python/gameforge_update_test.py [Classe]`.

---

### Task 1: Assinatura do portal e versões

**Files:**
- Create: `scripts/modules/gameforge_update/ed25519.py`, `scripts/modules/gameforge_update/versions.py`,
  `scripts/modules/gameforge_update/signature.py`
- Create (vazio por enquanto): `scripts/modules/gameforge_update/__init__.py` com só o cabeçalho SPDX e uma docstring
  de uma linha
- Test: `tests/python/gameforge_update_test.py`

**Interfaces:**
- Produces:
  - `ed25519.public_key(seed: bytes) -> bytes`;
  - `ed25519.sign(seed: bytes, message: bytes) -> bytes`;
  - `ed25519.verify(public: bytes, message: bytes, signature: bytes) -> bool`;
  - `versions.VERSION_FILE = "gameforge-version.json"`, `versions.MANIFEST_FILE = "gameforge-manifest.json"`;
  - `versions.ver_cmp(a: str, b: str) -> int`;
  - `versions.installed(install_dir: str) -> dict | None`;
  - `versions.newer(listing: dict, version: str) -> list[dict]`;
  - `signature.PINS: dict[str, bytes]`;
  - `signature.envelope_from_download(answer: dict) -> dict | None`, com as chaves `plugin_id`, `version`,
    `file_path`, `file_hash` e `signature`;
  - `signature.envelope_ok(envelope: dict, pins=PINS) -> bool`.

- [ ] **Step 1: Escrever os testes**

`tests/python/gameforge_update_test.py`:

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for the Studio's auto-update (``gameforge_update``): the portal's signature, versions, the update package
(``pack_update.py``), applying it to an installation and the download. No portal, no network, no elevation.

Runs with plain Python, no Studio build needed:
  python3 tests/python/gameforge_update_test.py [TestClass]
"""

import base64
import hashlib
import http.server
import io
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest
import urllib.parse
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts", "modules"))
sys.path.insert(0, os.path.join(ROOT, "release", "windows", "gameforge"))

from gameforge_update import ed25519, signature, versions  # noqa: E402

TEST_SEED = bytes(range(32))
TEST_PINS = {"key_test": ed25519.public_key(TEST_SEED)}
PLUGIN_ID = "11111111-2222-3333-4444-555555555555"


def signed_answer(version, file_hash, *, name=None, seed=TEST_SEED, kid="key_test", base=None):
    """A plugin-download answer as the portal gives it, signed the way admin-sign signs."""
    file_path = "{:s}/{:s}/{:s}".format(PLUGIN_ID, version, name or "gameforge-3d-studio-{:s}.zip".format(version))
    message = json.dumps({"plugin_id": PLUGIN_ID, "version": version, "file_path": file_path,
                          "file_hash": file_hash, "kid": kid}, separators=(",", ":")).encode("utf-8")
    url = ("https://x.supabase.co/storage/v1/object/sign/plugin-files/" + urllib.parse.quote(file_path) +
           "?token=abc")
    return {"version": version, "download_url": url, "file_hash": file_hash, "min_helper_version": base,
            "signature": base64.b64encode(ed25519.sign(seed, message)).decode("ascii")}


class Ed25519Test(unittest.TestCase):

    def test_rfc8032_vectors(self):
        for seed, public, message, sig in (
            ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
             "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
             "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25b"
             "f5f0595bbe24655141438e7a100b"),
            ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
             "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
             "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2e"
             "aeb4302aeeb00d291612bb0c00"),
        ):
            seed, public, message, sig = (bytes.fromhex(v) for v in (seed, public, message, sig))
            self.assertEqual(ed25519.public_key(seed), public)
            self.assertEqual(ed25519.sign(seed, message), sig)
            self.assertTrue(ed25519.verify(public, message, sig))
            self.assertFalse(ed25519.verify(public, message + b"x", sig))

    def test_real_portal_signature(self):
        """The launcher 1.5.0 as the portal signed it (plugin_versions, 2026-10-10): the envelope format is exact."""
        envelope = {"plugin_id": "4ce45c4a-6ad9-495d-8c0e-649c386a973b", "version": "1.5.0",
                    "file_path": "4ce45c4a-6ad9-495d-8c0e-649c386a973b/1.5.0/AtomLauncher.exe",
                    "file_hash": "861638ff411e8f0b311b1230fea2a60f41ebba9eb256c753585718d4fc8bb749",
                    "signature": "r53TpITdEQjy+J6oV04hv/ffys/OVsBxQ5q2r4wDNB1mCwHVe36mzQpSHTeqF9gqhgW9xBHukWIUIpokFqx2CQ=="}
        self.assertTrue(signature.envelope_ok(envelope))
        self.assertFalse(signature.envelope_ok(dict(envelope, version="1.5.1")))


class VersionTest(unittest.TestCase):

    def test_ver_cmp(self):
        self.assertEqual(versions.ver_cmp("1.2.10", "1.2.9"), 1)
        self.assertEqual(versions.ver_cmp("5.4.0", "5.4"), 0)
        self.assertEqual(versions.ver_cmp("1.0.0-beta", "1.0.0"), -1)
        self.assertEqual(versions.ver_cmp("5.4.1", "5.4.2"), -1)

    def test_newer(self):
        listing = {"versions": [{"version": "5.4.2", "changelog": "b"}, {"version": "5.4.0", "changelog": "x"},
                                {"version": "5.4.1", "changelog": "a"}, {"version": "5.3.9"}, {"bad": 1}]}
        self.assertEqual([v["version"] for v in versions.newer(listing, "5.4.0")], ["5.4.1", "5.4.2"])
        self.assertEqual(versions.newer(listing, "5.4.2"), [])
        self.assertEqual(versions.newer({}, "5.4.0"), [])

    def test_installed(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertIsNone(versions.installed(folder))
            with open(os.path.join(folder, versions.VERSION_FILE), "w", encoding="utf-8") as fh:
                json.dump({"version": "5.4.1", "base_version": "5.4.0"}, fh)
            self.assertEqual(versions.installed(folder)["version"], "5.4.1")


class SignatureTest(unittest.TestCase):

    def test_envelope_from_download(self):
        answer = signed_answer("5.4.2", "ab" * 32, name="pacote ação.zip")
        envelope = signature.envelope_from_download(answer)
        self.assertEqual(envelope["file_path"], "{:s}/5.4.2/pacote ação.zip".format(PLUGIN_ID))
        self.assertEqual(envelope["plugin_id"], PLUGIN_ID)
        self.assertIsNone(signature.envelope_from_download(dict(answer, download_url="https://x/y.zip")))
        self.assertIsNone(signature.envelope_from_download(dict(answer, file_hash="xyz")))
        self.assertIsNone(signature.envelope_from_download(dict(answer, version='5.4"2')))

    def test_envelope_ok(self):
        envelope = signature.envelope_from_download(signed_answer("5.4.2", "ab" * 32))
        self.assertTrue(signature.envelope_ok(envelope, TEST_PINS))
        self.assertFalse(signature.envelope_ok(dict(envelope, version="5.4.3"), TEST_PINS))
        self.assertFalse(signature.envelope_ok(dict(envelope, signature="não é base64"), TEST_PINS))
        self.assertFalse(signature.envelope_ok({"signature": envelope["signature"]}, TEST_PINS))
        # The production keys never accept a test signature.
        self.assertFalse(signature.envelope_ok(envelope))


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `python tests/python/gameforge_update_test.py`
Esperado: erro de import, `cannot import name 'ed25519' from 'gameforge_update'`.

- [ ] **Step 3: Escrever `ed25519.py`**

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Ed25519 (RFC 8032) in pure Python: the Studio's Python has no ``cryptography``. ``verify`` checks the portal's
release signatures; ``sign`` and ``public_key`` serve the tests. Not constant time, so not for secret keys in
production: verification only handles public data.
"""

__all__ = (
    "public_key",
    "sign",
    "verify",
)

import hashlib

_P = 2 ** 255 - 19
_L = 2 ** 252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _P - 2, _P) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)


def _hash(data):
    return int.from_bytes(hashlib.sha512(data).digest(), "little")


def _add(a, b):
    # Points in extended coordinates (X, Y, Z, T): x = X/Z, y = Y/Z, x*y = T/Z.
    x1, y1, z1, t1 = a
    x2, y2, z2, t2 = b
    aa = (y1 - x1) * (y2 - x2) % _P
    bb = (y1 + x1) * (y2 + x2) % _P
    cc = 2 * t1 * t2 * _D % _P
    dd = 2 * z1 * z2 % _P
    e, f, g, h = bb - aa, dd - cc, dd + cc, bb + aa
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _mul(scalar, point):
    result = (0, 1, 1, 0)  # The neutral element.
    while scalar > 0:
        if scalar & 1:
            result = _add(result, point)
        point = _add(point, point)
        scalar >>= 1
    return result


def _equal(a, b):
    return (a[0] * b[2] - b[0] * a[2]) % _P == 0 and (a[1] * b[2] - b[1] * a[2]) % _P == 0


def _recover_x(y, sign):
    if y >= _P:
        return None
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P)
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P:
        x = x * _SQRT_M1 % _P
    if (x * x - x2) % _P:
        return None
    if (x & 1) != sign:
        x = _P - x
    return x


_GY = 4 * pow(5, _P - 2, _P) % _P
_GX = _recover_x(_GY, 0)
_G = (_GX, _GY, 1, _GX * _GY % _P)


def _compress(point):
    z_inv = pow(point[2], _P - 2, _P)
    x, y = point[0] * z_inv % _P, point[1] * z_inv % _P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decompress(data):
    if len(data) != 32:
        return None
    y = int.from_bytes(data, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    return None if x is None else (x, y, 1, x * y % _P)


def _expand(seed):
    digest = hashlib.sha512(seed).digest()
    a = int.from_bytes(digest[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, digest[32:]


def public_key(seed):
    """The 32-byte public key of a 32-byte secret seed."""
    return _compress(_mul(_expand(seed)[0], _G))


def sign(seed, message):
    a, prefix = _expand(seed)
    public = _compress(_mul(a, _G))
    r = _hash(prefix + message) % _L
    r_bytes = _compress(_mul(r, _G))
    s = (r + _hash(r_bytes + public + message) % _L * a) % _L
    return r_bytes + int.to_bytes(s, 32, "little")


def verify(public, message, signature):
    """True when ``signature`` (64 bytes) was made over ``message`` by the key ``public`` (32 bytes)."""
    if len(public) != 32 or len(signature) != 64:
        return False
    a = _decompress(public)
    r = _decompress(signature[:32])
    if a is None or r is None:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= _L:
        return False
    h = _hash(signature[:32] + public + message) % _L
    return _equal(_mul(s, _G), _add(r, _mul(h, a)))
```

- [ ] **Step 4: Escrever `versions.py`**

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Versions of the Studio: the order the launcher, the DLL and the portal use, and what is installed."""

__all__ = (
    "MANIFEST_FILE",
    "VERSION_FILE",
    "installed",
    "newer",
    "ver_cmp",
)

import functools
import json
import os
import re

VERSION_FILE = "gameforge-version.json"
MANIFEST_FILE = "gameforge-manifest.json"
_VERSION = re.compile(r"(?:(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:\.(\d+))?)?(.*)", re.S)


def ver_cmp(a, b):
    """-1, 0 or 1. Up to four numbers, one by one (missing = 0); with equal numbers the suffix decides as in
    semver (1.0.0 > 1.0.0-beta). The rule of the launcher (`ver_cmp`), the DLL and the portal (`verCmp`)."""
    x, y = _VERSION.fullmatch(a).groups(), _VERSION.fullmatch(b).groups()
    for i in range(4):
        difference = int(x[i] or 0) - int(y[i] or 0)
        if difference:
            return 1 if difference > 0 else -1
    if not x[4] or not y[4]:
        return -1 if x[4] else (1 if y[4] else 0)
    return (x[4] > y[4]) - (x[4] < y[4])


def installed(install_dir):
    """``{"version", "base_version", "build"}`` of an installation; ``None`` without the file (a development build)."""
    try:
        with open(os.path.join(install_dir, VERSION_FILE), encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) and data.get("version") else None


def newer(listing, version):
    """The versions of a ``plugin-download`` list answer above ``version``, oldest first."""
    found = [item for item in listing.get("versions") or []
             if isinstance(item, dict) and item.get("version") and ver_cmp(str(item["version"]), version) > 0]
    found.sort(key=functools.cmp_to_key(lambda a, b: ver_cmp(str(a["version"]), str(b["version"]))))
    return found
```

- [ ] **Step 5: Escrever `signature.py`**

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The portal's release signature (``admin-sign`` at atomgameforge.com): Ed25519 over the UTF-8 bytes of
``JSON.stringify({plugin_id, version, file_path, file_hash, kid})``, base64. The same check as the launcher
(``PWOps/155dll/launcher/updater.hpp``, ``upd__sig_ok``): ``plugin-download`` answers neither ``file_path`` nor
``kid``, so the path comes from the signed Storage URL, the plugin id is its first segment and each pinned key is
tried with its own kid.
"""

__all__ = (
    "PINS",
    "envelope_from_download",
    "envelope_ok",
)

import base64
import binascii
import json
import re
import urllib.parse

from . import ed25519

# The portal's signing keys (`signing_keys`; the launcher's and the DLL's `g_joiaPins`). A new key is pinned by an
# update signed with a key that is already here, before the portal starts signing with it.
PINS = {
    "key_ec46b5ca": bytes.fromhex("dddcf28b09b79297058bbfffa5de5584c0a72c25f28d59aa1dde919e25dcafe4"),
    "key_7717f668": bytes.fromhex("1aeba158eaba9092aa3505ef30979ec86d03d5e944108e3d76d19d93ae8f7a5a"),
}
_MARK = "/object/sign/plugin-files/"
_HEX64 = re.compile(r"[0-9a-f]{64}")


def envelope_from_download(answer):
    """The signed fields of a ``plugin-download`` answer (plus the signature), or ``None`` when they cannot be
    the /admin's."""
    url = str(answer.get("download_url") or "")
    at = url.find(_MARK)
    if at < 0:
        return None
    try:
        path = urllib.parse.unquote(url[at + len(_MARK):].split("?", 1)[0].split("#", 1)[0], errors="strict")
    except UnicodeDecodeError:
        return None
    version = str(answer.get("version") or "")
    file_hash = str(answer.get("file_hash") or "")
    plugin_id = path.split("/", 1)[0]
    if ("/" not in path or not 0 < len(plugin_id) <= 64 or not version or not _HEX64.fullmatch(file_hash)
            or any(ord(c) < 0x20 or c in '"\\' for c in path + version)):
        return None
    return {"plugin_id": plugin_id, "version": version, "file_path": path, "file_hash": file_hash,
            "signature": str(answer.get("signature") or "")}


def envelope_ok(envelope, pins=PINS):
    """True when one of the pinned keys signed the envelope."""
    try:
        sig = base64.b64decode(envelope["signature"], validate=True)
        fields = {key: envelope[key] for key in ("plugin_id", "version", "file_path", "file_hash")}
    except (KeyError, TypeError, ValueError, binascii.Error):
        return False
    for kid, key in pins.items():
        message = json.dumps(dict(fields, kid=kid), separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        if ed25519.verify(key, message, sig):
            return True
    return False
```

- [ ] **Step 6: Rodar e ver passar**

Run: `python tests/python/gameforge_update_test.py`
Esperado: `Ran 7 tests ... OK`.

- [ ] **Step 7: Commit**

```bash
git add scripts/modules/gameforge_update tests/python/gameforge_update_test.py
git commit -m "GameForge: verify the portal's release signature (Ed25519, pure Python)"
```

---

### Task 2: Versão e manifesto da instalação

**Files:**
- Create: `release/windows/gameforge/write_manifest.py`
- Modify: `source/creator/CMakeLists.txt` (fim do arquivo)
- Modify: `source/blender/blenkernel/BKE_blender_version.h:27`
- Modify: `D:\claude-scratch\gf-tools\deploy_install.py` (lista `EXTRA`)
- Test: `tests/python/gameforge_update_test.py` (classe `ManifestTest`)

**Interfaces:**
- Produces:
  - `write_manifest.write(root: str, version: str, build: str = "") -> dict[str, str]`;
  - `write_manifest.manifest(root: str) -> dict[str, str]`;
  - os arquivos `<instalação>/gameforge-manifest.json` (`{caminho com "/": sha256}`, sem `__pycache__` e sem os
    dois `.json`) e `<instalação>/gameforge-version.json` (`{"version", "base_version", "build"}`), ambos com
    `json.dump(..., ensure_ascii=False, indent=0, sort_keys=True)`.

- [ ] **Step 1: Escrever o teste**

No `gameforge_update_test.py`, depois dos imports, acrescentar `import write_manifest  # noqa: E402` e esta classe:

```python
def make_build(root, version, files):
    """A fake installation: ``files`` = {relative path: bytes}, plus its manifest and version files."""
    for path, data in files.items():
        target = os.path.join(root, *path.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as fh:
            fh.write(data)
    write_manifest.write(root, version)
    return root


class ManifestTest(unittest.TestCase):

    def test_manifest_and_version(self):
        with tempfile.TemporaryDirectory() as root:
            make_build(root, "5.4.0", {"x.txt": b"x", "sub/y.bin": b"y", "sub/__pycache__/z.pyc": b"z"})
            with open(os.path.join(root, versions.MANIFEST_FILE), encoding="utf-8") as fh:
                files = json.load(fh)
            self.assertEqual(files, {"sub/y.bin": hashlib.sha256(b"y").hexdigest(),
                                     "x.txt": hashlib.sha256(b"x").hexdigest()})
            self.assertEqual(versions.installed(root), {"version": "5.4.0", "base_version": "5.4.0", "build": ""})
            # Running again does not list its own files.
            self.assertEqual(write_manifest.write(root, "5.4.0"), files)
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `python tests/python/gameforge_update_test.py ManifestTest`
Esperado: `ModuleNotFoundError: No module named 'write_manifest'`.

- [ ] **Step 3: Escrever `release/windows/gameforge/write_manifest.py`**

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Writes, at the root of an installation of GameForge 3D Studio:

- ``gameforge-manifest.json``: ``{relative path: sha256}`` of every installed file;
- ``gameforge-version.json``: ``{"version", "base_version", "build"}``. An installation from the installer is
  its own base; the auto-update rewrites both files.

Run by the last install rule (``source/creator/CMakeLists.txt``) with the installed Python, so the installer
(CPack installs into its own folder first), ``gf-build\\bin`` and a development deploy all carry them::

    python -I write_manifest.py <install dir> <version> [<source dir, for the git hash>]
"""

import hashlib
import json
import os
import subprocess
import sys

MANIFEST_FILE = "gameforge-manifest.json"
VERSION_FILE = "gameforge-version.json"


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def manifest(root):
    files = {}
    for folder, dirs, names in os.walk(root):
        dirs[:] = [d for d in dirs if d != "__pycache__"]
        for name in names:
            path = os.path.join(folder, name)
            relative = os.path.relpath(path, root).replace(os.sep, "/")
            if relative not in {MANIFEST_FILE, VERSION_FILE}:
                files[relative] = file_sha256(path)
    return files


def _write_json(path, data):
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=0, sort_keys=True)


def git_hash(source_dir):
    try:
        done = subprocess.run(["git", "-C", source_dir, "rev-parse", "--short=12", "HEAD"],
                              capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return done.stdout.strip() if done.returncode == 0 else ""


def write(root, version, build=""):
    files = manifest(root)
    _write_json(os.path.join(root, MANIFEST_FILE), files)
    _write_json(os.path.join(root, VERSION_FILE), {"version": version, "base_version": version, "build": build})
    return files


if __name__ == "__main__":
    write(sys.argv[1], sys.argv[2], git_hash(sys.argv[3]) if len(sys.argv) > 3 else "")
```

- [ ] **Step 4: Rodar e ver passar**

Run: `python tests/python/gameforge_update_test.py ManifestTest`
Esperado: `OK`.

- [ ] **Step 5: Ligar no passo de instalação (CMake) e mudar o ciclo para `release`**

No fim de `source/creator/CMakeLists.txt`, depois do bloco "Post-install script":

```cmake
# -----------------------------------------------------------------------------
# GameForge: version and manifest of the installed files (auto-update).
# The last install rule: every file is in place. CPack installs into its own folder, so the installer gets
# them too.

if(WIN32)
  install(CODE "
    execute_process(
      COMMAND \"\${CMAKE_INSTALL_PREFIX}/${TARGETDIR_VER}/python/bin/python.exe\" -I
              \"${CMAKE_SOURCE_DIR}/release/windows/gameforge/write_manifest.py\"
              \"\${CMAKE_INSTALL_PREFIX}\"
              \"${BLENDER_VERSION_MAJOR}.${BLENDER_VERSION_MINOR}.${BLENDER_VERSION_PATCH}\"
              \"${CMAKE_SOURCE_DIR}\"
      RESULT_VARIABLE _gameforge_result
    )
    if(NOT _gameforge_result EQUAL 0)
      message(FATAL_ERROR \"GameForge: write_manifest.py failed (\${_gameforge_result})\")
    endif()
  ")
endif()
```

Em `source/blender/blenkernel/BKE_blender_version.h`, linha 27: `#define BLENDER_VERSION_CYCLE alpha` →
`#define BLENDER_VERSION_CYCLE release`.

Em `D:\claude-scratch\gf-tools\deploy_install.py`, a lista `EXTRA` ganha `"gameforge-manifest.json",
"gameforge-version.json"`.

- [ ] **Step 6: Build e conferência**

Run (PowerShell; nenhum Studio aberto): `& "D:\claude-scratch\gf-tools\build_j6.bat"`
Esperado: `_rebuild.log` termina com `EXIT 0`, porque o cabeçalho de versão recompila boa parte do código.

Conferir:
- `gf-build\bin\gameforge-version.json` tem `"version": "5.4.0"`, `"base_version": "5.4.0"` e o hash do git;
- `gf-build\bin\gameforge-manifest.json` tem 7000 entradas ou mais, inclusive `GameForge3DStudio.exe`.

Run (PowerShell):

```powershell
Get-Content "D:\Atom Sec Labs\ATOM GameForge\gf-build\bin\gameforge-version.json"
(Get-Content "D:\Atom Sec Labs\ATOM GameForge\gf-build\bin\gameforge-manifest.json" | ConvertFrom-Json).PSObject.Properties.Name.Count
```

- [ ] **Step 7: Suíte inteira (o ciclo `release` esconde as opções experimentais)**

Run: cada `tests/python/gameforge_*_test.py` com
`gf-build\bin\GameForge3DStudio.exe -b --factory-startup --python <teste>` e o ambiente canônico:
- `GAMEFORGE_TEST_CLIENTS` = 1.7.8; PWNH; PWU;
- `GAMEFORGE_TEST_SERVER` = `D:\Atom Sec Labs\ATOM GameForge\pwserver_legado\PWServer\gamed\config`;
- `GAMEFORGE_ACCOUNT_REQUIRED=0`.

`gameforge_account_test`, `gameforge_plugins_test`, `gameforge_agent_test` e `gameforge_update_test` rodam com
`python` comum. Depois, o `ui_check.py`.

Esperado: tudo verde, como na última rodada completa de 2026-10-15. O título da janela diz
"GameForge 3D Studio 5.4.0", sem "Alpha".

- [ ] **Step 8: Commit**

```bash
git add release/windows/gameforge/write_manifest.py source/creator/CMakeLists.txt source/blender/blenkernel/BKE_blender_version.h tests/python/gameforge_update_test.py
git commit -m "GameForge: write the version and file manifest at install; release cycle"
```

---

### Task 3: Empacotador da atualização

**Files:**
- Create: `release/windows/gameforge/pack_update.py`
- Create: `scripts/modules/gameforge_update/package.py`, por enquanto só com as constantes `UPDATE_FILE` e
  `UpdateError`. O resto entra na Task 4.
- Test: `tests/python/gameforge_update_test.py` (classe `PackTest`)

**Interfaces:**
- Consumes:
  - `versions.ver_cmp`, `versions.VERSION_FILE`, `versions.MANIFEST_FILE` (Task 1);
  - `write_manifest.write` (Task 2, nos testes).
- Produces:
  - `package.UPDATE_FILE = "gameforge-update.json"`;
  - `pack_update.record_installer(build_dir: str, manifests: str) -> str` (a versão);
  - `pack_update.pack(build_dir: str, changelog: str, manifests: str, out: str, max_bytes: int = MAX_BYTES)
    -> tuple[str, list[str], list[str]]`, com o caminho do zip de release, os mudados e os removidos;
  - o formato do pacote:
    `{"schema": 1, "version", "base_version", "files": {path: sha256}, "removed": [path],
    "manifest": {path: sha256}}`;
  - `<manifests>/<versão>.json` = `{"version", "base_version", "files"}`.

- [ ] **Step 1: Escrever o teste**

Acrescentar `import pack_update  # noqa: E402` e `from gameforge_update import package  # noqa: E402` aos imports, e:

```python
class Builds:
    """Three builds of a fake Studio and the installer manifest of the first (5.4.0)."""

    def __init__(self, tmp):
        self.manifests = os.path.join(tmp, "manifests")
        self.out = os.path.join(tmp, "out")
        self.b0 = make_build(os.path.join(tmp, "b0"), "5.4.0", {
            "a.txt": b"a0", "b.txt": b"b0", "sub/c.dll": b"c0", "blender.pdb": b"p0"})
        self.b1 = make_build(os.path.join(tmp, "b1"), "5.4.1", {
            "a.txt": b"a1", "b.txt": b"b0", "sub/c.dll": b"c0", "sub/d.py": b"d1", "blender.pdb": b"p1"})
        # a.txt goes back to the installer's bytes; b.txt is gone.
        self.b2 = make_build(os.path.join(tmp, "b2"), "5.4.2", {
            "a.txt": b"a0", "sub/c.dll": b"c0", "sub/d.py": b"d1", "blender.pdb": b"p2"})
        pack_update.record_installer(self.b0, self.manifests)


def open_release(release):
    """(pwops-release.json, its zip entries, gameforge-update.json, names in the package, package bytes)."""
    with zipfile.ZipFile(release) as rz:
        manifest = json.loads(rz.read("pwops-release.json"))
        payload = rz.read(manifest["payload"])
        infos = rz.infolist()
    with zipfile.ZipFile(io.BytesIO(payload)) as zf:
        return manifest, infos, json.loads(zf.read(package.UPDATE_FILE)), sorted(zf.namelist()), payload


class PackTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.builds = Builds(self.tmp)

    def test_cumulative_package(self):
        b = self.builds
        pack_update.pack(b.b1, "Primeira", b.manifests, b.out)
        release, _changed, _removed = pack_update.pack(b.b2, "Segunda", b.manifests, b.out)
        manifest, infos, update, names, payload = open_release(release)
        # a.txt is back to the installer's bytes but changed in 5.4.1: an installation on 5.4.1 still needs it.
        self.assertEqual(sorted(update["files"]), ["a.txt", "sub/d.py"])
        self.assertEqual(update["removed"], ["b.txt"])
        self.assertEqual((update["version"], update["base_version"]), ("5.4.2", "5.4.0"))
        self.assertNotIn("blender.pdb", update["manifest"])
        self.assertEqual(names, sorted(["a.txt", "sub/d.py", package.UPDATE_FILE]))
        # The zip for the /admin: STORED, the payload listed with its SHA-256, the base as min_helper_version.
        self.assertTrue(all(info.compress_type == zipfile.ZIP_STORED for info in infos))
        self.assertEqual((manifest["schema"], manifest["plugin"], manifest["version"]), (1, "gameforge-3d-studio", "5.4.2"))
        self.assertEqual(manifest["min_helper_version"], "5.4.0")
        self.assertEqual(manifest["files"], [{"path": manifest["payload"], "sha256": hashlib.sha256(payload).hexdigest(),
                                              "bytes": len(payload)}])
        self.assertTrue(os.path.isfile(os.path.join(b.manifests, "5.4.2.json")))

    def test_refusals(self):
        b = self.builds
        with self.assertRaises(SystemExit):
            pack_update.pack(b.b0, "Mesma versão", b.manifests, b.out)
        with self.assertRaises(SystemExit):
            pack_update.pack(b.b1, "", b.manifests, b.out)
        with self.assertRaises(SystemExit):
            pack_update.pack(b.b1, "x" * 601, b.manifests, b.out)
        with self.assertRaises(SystemExit):
            pack_update.pack(b.b1, "Grande demais", b.manifests, b.out, max_bytes=100)
        with self.assertRaises(SystemExit):
            pack_update.pack(b.b1, "Sem base", os.path.join(self.tmp, "vazio"), b.out)
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `python tests/python/gameforge_update_test.py PackTest`
Esperado: `ModuleNotFoundError: No module named 'pack_update'`.

- [ ] **Step 3: Criar `package.py` com as constantes**

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
An update package of the Studio and how it is applied.

The package is a zip (LZMA) with ``gameforge-update.json`` and the files that changed since the installed base,
built by ``release/windows/gameforge/pack_update.py``::

    {"schema": 1, "version": "5.4.2", "base_version": "5.4.0",
     "files": {path: sha256},     # the files in the zip
     "removed": [path],           # files of earlier versions that the new one no longer has
     "manifest": {path: sha256}}  # every file of the new version

Paths are relative to the installation, with ``/``.
"""

__all__ = (
    "UPDATE_FILE",
    "UpdateError",
)

UPDATE_FILE = "gameforge-update.json"


class UpdateError(Exception):
    pass
```

- [ ] **Step 4: Escrever `release/windows/gameforge/pack_update.py`**

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Builds a GameForge 3D Studio update for the portal (atomgameforge.com /admin > Versões).

    python pack_update.py <build dir> --changelog "texto" --manifests <folder> --out <folder>
        The update from the last installer up to the build: <out>/gameforge-3d-studio-<v>-release.zip (the zip to
        upload) and <manifests>/<v>.json.

    python pack_update.py <build dir> --record-installer --manifests <folder>
        After publishing a full installer (MSI) of the build: its manifest becomes the base of the next updates.

The update is cumulative: it carries every file that differs between the build and the installer or any update
published since, so one package takes any of those installations to the new version. <build dir> is an
installation with gameforge-manifest.json (gf-build\\bin, or the folder CPack installed into).
"""

import argparse
import functools
import hashlib
import io
import json
import os
import re
import sys
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "scripts", "modules"))

from gameforge_update.package import UPDATE_FILE  # noqa: E402
from gameforge_update.versions import MANIFEST_FILE, VERSION_FILE, ver_cmp  # noqa: E402

PLUGIN = "gameforge-3d-studio"
MAX_BYTES = 45 * 1024 * 1024  # The portal stores files up to 50 MB (Supabase Free).
CHANGELOG_MAX = 600           # release-zip.ts (CHANGELOG_MAX).
_VERSION_RE = re.compile(r"[0-9A-Za-z.+-]{1,31}")


def _skip(path):
    # Debug symbols: big, rebuilt every time and only read by crash reports. Updates leave them as they are.
    return path.lower().endswith(".pdb")


def _write_json(path, data):
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=0, sort_keys=True)


def load_build(build_dir):
    with open(os.path.join(build_dir, VERSION_FILE), encoding="utf-8") as fh:
        version = json.load(fh)["version"]
    with open(os.path.join(build_dir, MANIFEST_FILE), encoding="utf-8") as fh:
        files = {path: digest for path, digest in json.load(fh).items() if not _skip(path)}
    return version, files


def load_manifests(folder):
    found = []
    if os.path.isdir(folder):
        for name in os.listdir(folder):
            if name.endswith(".json"):
                with open(os.path.join(folder, name), encoding="utf-8") as fh:
                    found.append(json.load(fh))
    found.sort(key=functools.cmp_to_key(lambda a, b: ver_cmp(a["version"], b["version"])))
    return found


def record_installer(build_dir, manifests):
    version, files = load_build(build_dir)
    os.makedirs(manifests, exist_ok=True)
    _write_json(os.path.join(manifests, version + ".json"), {"version": version, "base_version": version, "files": files})
    return version


def pack(build_dir, changelog, manifests, out, max_bytes=MAX_BYTES):
    version, files = load_build(build_dir)
    published = load_manifests(manifests)
    if not published:
        raise SystemExit("sem o manifesto do instalador: rode --record-installer depois de publicar o MSI")
    latest = published[-1]
    if not _VERSION_RE.fullmatch(version) or ver_cmp(version, latest["version"]) <= 0:
        raise SystemExit("a versão {:s} do build não é maior que a {:s} já publicada".format(version, latest["version"]))
    changelog = changelog.strip()
    if not changelog or len(changelog) > CHANGELOG_MAX:
        raise SystemExit("changelog vazio ou com mais de {:d} caracteres".format(CHANGELOG_MAX))
    base = latest["base_version"]
    since_base = [m["files"] for m in published if m["base_version"] == base]
    changed = sorted(path for path, digest in files.items() if any(old.get(path) != digest for old in since_base))
    removed = sorted({path for old in since_base for path in old} - set(files))
    update = {"schema": 1, "version": version, "base_version": base,
              "files": {path: files[path] for path in changed}, "removed": removed, "manifest": files}

    name = "{:s}-{:s}.zip".format(PLUGIN, version)
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_LZMA) as zf:
        zf.writestr(UPDATE_FILE, json.dumps(update, ensure_ascii=False, indent=0, sort_keys=True))
        for path in changed:
            zf.write(os.path.join(build_dir, *path.split("/")), path)
    data = buffer.getvalue()
    if len(data) > max_bytes:
        raise SystemExit("o pacote tem {:.1f} MB (máximo {:.0f}): publique um instalador completo (MSI)".format(
            len(data) / 2 ** 20, max_bytes / 2 ** 20))

    # The zip for the /admin (release-zip.ts): STORED, the manifest at the root, the package as the payload.
    release = {"schema": 1, "plugin": PLUGIN, "version": version, "changelog": changelog,
               "files": [{"path": name, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}],
               "payload": name, "min_helper_version": base}
    os.makedirs(out, exist_ok=True)
    release_path = os.path.join(out, "{:s}-{:s}-release.zip".format(PLUGIN, version))
    with zipfile.ZipFile(release_path, "w", compression=zipfile.ZIP_STORED) as zf:
        zf.writestr("pwops-release.json", json.dumps(release, ensure_ascii=False, indent=1))
        zf.writestr(name, data)
    _write_json(os.path.join(manifests, version + ".json"), {"version": version, "base_version": base, "files": files})
    return release_path, changed, removed


def main(argv=None):
    parser = argparse.ArgumentParser(description="Atualização do GameForge 3D Studio para o portal.")
    parser.add_argument("build", help="a instalação do build, com gameforge-manifest.json")
    parser.add_argument("--manifests", required=True, help="a pasta dos manifestos publicados")
    parser.add_argument("--record-installer", action="store_true", help="registra o build como base (depois do MSI)")
    parser.add_argument("--changelog", default="", help="o texto do changelog (até 600 caracteres)")
    parser.add_argument("--out", default=".", help="onde gravar o zip para o /admin")
    args = parser.parse_args(argv)
    if args.record_installer:
        print("base registrada:", record_installer(args.build, args.manifests))
        return
    release, changed, removed = pack(args.build, args.changelog, args.manifests, args.out)
    print("{:s}: {:d} arquivo(s) no pacote, {:d} removido(s)".format(release, len(changed), len(removed)))


if __name__ == "__main__":
    main()
```

- [ ] **Step 5: Rodar e ver passar**

Run: `python tests/python/gameforge_update_test.py PackTest`
Esperado: `OK`.

- [ ] **Step 6: Commit**

```bash
git add release/windows/gameforge/pack_update.py scripts/modules/gameforge_update/package.py tests/python/gameforge_update_test.py
git commit -m "GameForge: build cumulative update packages for the portal"
```

---

### Task 4: Aplicar o pacote (com volta atrás) e o script do aplicador

**Files:**
- Modify: `scripts/modules/gameforge_update/package.py` (o arquivo inteiro abaixo)
- Create: `scripts/modules/gameforge_update/apply.py`
- Test: `tests/python/gameforge_update_test.py` (classe `ApplyTest`)

**Interfaces:**
- Consumes:
  - `versions.installed`, `versions.ver_cmp`, `versions.MANIFEST_FILE`, `versions.VERSION_FILE` (Task 1);
  - `signature.envelope_ok`, `signature.PINS` (Task 1);
  - o formato do pacote (Task 3).
- Produces:
  - `package.file_sha256(path) -> str`;
  - `package.read_update(zip_path) -> dict`;
  - `package.apply_update(install_dir, zip_path, log=print) -> str` (a versão nova);
  - `apply.main(argv) -> int`, com os papéis:
    `watch <install> <package.zip> <envelope.json> <log> [--dry-run]` e
    `apply <install> <package.zip> <envelope.json> <log> [--dry-run]`;
  - `apply.elevated_parameters(arguments: list[str]) -> str`;
  - `apply.studio_running(tasklist_output: str | None = None) -> bool`;
  - o log com linhas `RESULTADO <texto>`.

- [ ] **Step 1: Escrever os testes**

Acrescentar aos imports `from gameforge_update import apply as applier  # noqa: E402` e:

```python
def read(root, path):
    with open(os.path.join(root, *path.split("/")), "rb") as fh:
        return fh.read()


def snapshot(root):
    """Every file of a folder: {relative path: bytes}."""
    found = {}
    for folder, _dirs, names in os.walk(root):
        for name in names:
            path = os.path.join(folder, name)
            with open(path, "rb") as fh:
                found[os.path.relpath(path, root)] = fh.read()
    return found


class ApplyTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        b = self.builds = Builds(self.tmp)
        pack_update.pack(b.b1, "Primeira", b.manifests, b.out)
        release, _changed, _removed = pack_update.pack(b.b2, "Segunda", b.manifests, b.out)
        self.p2 = os.path.join(self.tmp, "p2.zip")
        with open(self.p2, "wb") as fh:
            fh.write(open_release(release)[4])

    def install(self, build):
        target = os.path.join(self.tmp, "install-" + os.path.basename(build))
        shutil.copytree(build, target)
        return target

    def test_from_installer_and_from_update(self):
        for start in (self.builds.b0, self.builds.b1):
            install = self.install(start)
            pdb = read(install, "blender.pdb")
            self.assertEqual(package.apply_update(install, self.p2, log=lambda _text: None), "5.4.2")
            self.assertEqual(read(install, "a.txt"), b"a0")
            self.assertEqual(read(install, "sub/d.py"), b"d1")
            self.assertFalse(os.path.exists(os.path.join(install, "b.txt")))
            self.assertEqual(read(install, "blender.pdb"), pdb)  # Debug symbols are left alone.
            self.assertEqual(versions.installed(install)["version"], "5.4.2")
            self.assertEqual(versions.installed(install)["base_version"], "5.4.0")
            with open(os.path.join(install, versions.MANIFEST_FILE), encoding="utf-8") as fh:
                manifest = json.load(fh)
            self.assertEqual(manifest["sub/d.py"], hashlib.sha256(b"d1").hexdigest())
            self.assertNotIn("b.txt", manifest)
            self.assertIn("blender.pdb", manifest)
            leftovers = [name for _folder, dirs, names in os.walk(install) for name in names + dirs
                         if name.endswith(".gfold") or name == ".gameforge-update"]
            self.assertEqual(leftovers, [])

    @unittest.skipUnless(sys.platform == "win32", "Windows file locks")
    def test_file_in_use_puts_everything_back(self):
        install = self.install(self.builds.b1)
        before = snapshot(install)
        with open(os.path.join(install, "b.txt"), "rb"):  # Opened without sharing delete: it cannot be renamed.
            with self.assertRaises(PermissionError) as caught:
                package.apply_update(install, self.p2, log=lambda _text: None)
        self.assertIn("b.txt", str(caught.exception))
        self.assertEqual(snapshot(install), before)

    def test_refusals_change_nothing(self):
        install = self.install(self.builds.b0)
        before = snapshot(install)
        evil = os.path.join(self.tmp, "evil.zip")
        with zipfile.ZipFile(evil, "w") as zf:
            zf.writestr(package.UPDATE_FILE, json.dumps({
                "schema": 1, "version": "5.4.3", "base_version": "5.4.0", "removed": [],
                "files": {"../x.dll": "0" * 64}, "manifest": {"../x.dll": "0" * 64}}))
            zf.writestr("../x.dll", b"x")
        with self.assertRaises(package.UpdateError):
            package.apply_update(install, evil)
        later = os.path.join(self.tmp, "later.zip")  # Starts from a base newer than the installation.
        with zipfile.ZipFile(later, "w") as zf:
            zf.writestr(package.UPDATE_FILE, json.dumps({
                "schema": 1, "version": "5.5.1", "base_version": "5.5.0", "removed": [], "files": {}, "manifest": {}}))
        with self.assertRaises(package.UpdateError):
            package.apply_update(install, later)
        self.assertEqual(snapshot(install), before)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "x.dll")))
        package.apply_update(install, self.p2, log=lambda _text: None)
        updated = snapshot(install)
        with self.assertRaises(package.UpdateError):  # The same version again.
            package.apply_update(install, self.p2, log=lambda _text: None)
        self.assertEqual(snapshot(install), updated)

    def test_apply_role_checks_the_signature(self):
        install = self.install(self.builds.b0)
        log = os.path.join(self.tmp, "update.log")
        signature.PINS["key_test"] = TEST_PINS["key_test"]
        self.addCleanup(signature.PINS.pop, "key_test", None)
        good = signature.envelope_from_download(signed_answer("5.4.2", package.file_sha256(self.p2)))
        for envelope, expected in ((dict(good, version="5.4.9"), 1), (good, 0)):
            path = os.path.join(self.tmp, "envelope.json")
            with open(path, "w", encoding="utf-8") as fh:
                json.dump(envelope, fh)
            self.assertEqual(applier.apply(install, self.p2, path, log), expected)
        with open(log, encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn("RESULTADO falhou: o pacote não confere com a assinatura do portal", text)
        self.assertIn("RESULTADO atualizado para 5.4.2", text)
        self.assertFalse(os.path.exists(os.path.join(install, ".gameforge-package.zip")))

    @unittest.skipUnless(sys.platform == "win32", "Windows command lines")
    def test_elevated_command_line_keeps_each_argument(self):
        import ctypes
        from ctypes import wintypes
        arguments = ["apply", r"C:\Program Files\ATOM GameForge\GameForge 3D Studio 5.4",
                     r"C:\Users\Zé Ação\AppData\Local\x y\pacote.zip", 'D:\\a"b\\envelope.json', "C:\\log dir\\"]
        parse = ctypes.windll.shell32.CommandLineToArgvW
        parse.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
        parse.restype = ctypes.POINTER(wintypes.LPWSTR)
        count = ctypes.c_int()
        argv = parse("python.exe " + applier.elevated_parameters(arguments), ctypes.byref(count))
        parsed = [argv[i] for i in range(count.value)]
        ctypes.windll.kernel32.LocalFree(argv)
        self.assertEqual(parsed[1:3], ["-I", os.path.abspath(applier.__file__)])
        self.assertEqual(parsed[3:], arguments)

    def test_studio_running(self):
        self.assertTrue(applier.studio_running('"GameForge3DStudio.exe","1234","Console","1","500.000 K"\r\n'))
        self.assertFalse(applier.studio_running("INFORMAÇÕES: nenhuma tarefa em execução.\r\n"))
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `python tests/python/gameforge_update_test.py ApplyTest`
Esperado: `ImportError: cannot import name 'apply' from 'gameforge_update'`.

- [ ] **Step 3: Escrever o `package.py` completo**

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
An update package of the Studio and how it is applied.

The package is a zip (LZMA) with ``gameforge-update.json`` and the files that changed since the installed base,
built by ``release/windows/gameforge/pack_update.py``::

    {"schema": 1, "version": "5.4.2", "base_version": "5.4.0",
     "files": {path: sha256},     # the files in the zip
     "removed": [path],           # files of earlier versions that the new one no longer has
     "manifest": {path: sha256}}  # every file of the new version

Paths are relative to the installation, with ``/``.
"""

__all__ = (
    "UPDATE_FILE",
    "UpdateError",
    "apply_update",
    "file_sha256",
    "read_update",
)

import hashlib
import json
import os
import shutil
import zipfile

from .versions import MANIFEST_FILE, VERSION_FILE, installed, ver_cmp

UPDATE_FILE = "gameforge-update.json"
_OLD_SUFFIX = ".gfold"
_WORK_DIR = ".gameforge-update"


class UpdateError(Exception):
    pass


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _safe(path):
    """A relative path that stays inside the installation (the rule of the portal's `relOk`)."""
    return (isinstance(path, str) and bool(path) and all(part not in {"", ".", ".."} for part in path.split("/"))
            and not any(c in '\\:"*?<>|' or ord(c) < 0x20 for c in path))


def read_update(zip_path):
    """The package's ``gameforge-update.json``, checked against the zip's contents."""
    try:
        with zipfile.ZipFile(zip_path) as zf:
            info = json.loads(zf.read(UPDATE_FILE).decode("utf-8"))
            names = set(zf.namelist()) - {UPDATE_FILE}
    except (OSError, KeyError, ValueError, zipfile.BadZipFile) as ex:
        raise UpdateError("pacote de atualização ilegível: {:s}".format(str(ex))) from ex
    if not isinstance(info, dict) or info.get("schema") != 1 or not info.get("version") or not info.get("base_version"):
        raise UpdateError("pacote de atualização de formato desconhecido")
    files, removed, manifest = info.get("files") or {}, info.get("removed") or [], info.get("manifest") or {}
    for path in [*files, *removed, *manifest]:
        if not _safe(path):
            raise UpdateError("caminho inseguro no pacote: {!r}".format(path))
    if set(files) != names or any(manifest.get(path) != digest for path, digest in files.items()):
        raise UpdateError("os arquivos do pacote não batem com a lista dele")
    info["files"], info["removed"], info["manifest"] = files, removed, manifest
    return info


def _write_json(path, data):
    temp = path + ".tmp"
    with open(temp, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=0, sort_keys=True)
    os.replace(temp, path)


def _delete_soon(path):
    """Deletes now or, when the file is in use (the applier's own python.exe), when Windows restarts."""
    try:
        os.remove(path)
    except OSError:
        import ctypes
        ctypes.windll.kernel32.MoveFileExW(path, None, 0x4)  # MOVEFILE_DELAY_UNTIL_REBOOT


def _forget_old_files(install_dir):
    """The `.gfold` files an earlier update could not delete (they were in use)."""
    for folder, _dirs, names in os.walk(install_dir):
        for name in names:
            if name.endswith(_OLD_SUFFIX):
                try:
                    os.remove(os.path.join(folder, name))
                except OSError:
                    pass


def apply_update(install_dir, zip_path, log=print):
    """Applies a package to an installation and returns the new version. On any error every file goes back as it
    was and the error is raised again. Checking the package's signature is the caller's job (``apply.py``)."""
    current = installed(install_dir)
    if current is None:
        raise UpdateError("instalação sem {:s}".format(VERSION_FILE))
    info = read_update(zip_path)
    if ver_cmp(info["version"], current["version"]) <= 0:
        raise UpdateError("a versão {:s} já está instalada".format(current["version"]))
    if ver_cmp(info["base_version"], current["version"]) > 0:
        raise UpdateError("a atualização {:s} parte da {:s}, mais nova que a instalada ({:s}): use o instalador "
                          "completo".format(info["version"], info["base_version"], current["version"]))
    try:
        with open(os.path.join(install_dir, MANIFEST_FILE), encoding="utf-8") as fh:
            old = json.load(fh)
    except (OSError, ValueError) as ex:
        raise UpdateError("instalação sem {:s}".format(MANIFEST_FILE)) from ex
    files, manifest = info["files"], info["manifest"]
    changed = sorted(path for path, digest in manifest.items() if old.get(path) != digest)
    missing = [path for path in changed if path not in files]
    if missing:
        raise UpdateError("o pacote não traz {:d} arquivo(s) que mudaram, ex.: {:s}".format(len(missing), missing[0]))
    removed = sorted(path for path in info["removed"] if path not in manifest)
    _forget_old_files(install_dir)

    work = os.path.join(install_dir, _WORK_DIR)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    done = []  # (target, kind) in order; kind: "replaced", "added" or "removed".
    try:
        # 1. The new files next to the installation (same disk, so placing them is a rename), each one checked.
        with zipfile.ZipFile(zip_path) as zf:
            for i, path in enumerate(changed):
                staged = os.path.join(work, str(i))
                with zf.open(path) as src, open(staged, "wb") as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
                if file_sha256(staged) != manifest[path]:
                    raise UpdateError("arquivo corrompido no pacote: {:s}".format(path))
        # 2. Each old file becomes `<name>.gfold` (Windows lets a program in use be renamed); the new one takes
        #    its place.
        for i, path in enumerate(changed):
            target = os.path.join(install_dir, *path.split("/"))
            if os.path.exists(target):
                os.replace(target, target + _OLD_SUFFIX)
                done.append((target, "replaced"))
            else:
                os.makedirs(os.path.dirname(target), exist_ok=True)
                done.append((target, "added"))
            os.replace(os.path.join(work, str(i)), target)
        for path in removed:
            target = os.path.join(install_dir, *path.split("/"))
            if os.path.exists(target):
                os.replace(target, target + _OLD_SUFFIX)
                done.append((target, "removed"))
        # 3. The bookkeeping. Files the update does not know (debug symbols) stay listed as they are.
        new_manifest = dict(manifest)
        for path, digest in old.items():
            if path not in new_manifest and path not in removed:
                new_manifest[path] = digest
        _write_json(os.path.join(install_dir, MANIFEST_FILE), new_manifest)
        _write_json(os.path.join(install_dir, VERSION_FILE), {
            "version": info["version"], "base_version": info["base_version"], "build": info.get("build", "")})
    except BaseException:
        for target, kind in reversed(done):
            try:
                if kind in {"replaced", "added"} and os.path.exists(target):
                    os.remove(target)
                if kind in {"replaced", "removed"}:
                    os.replace(target + _OLD_SUFFIX, target)
            except OSError as ex:
                log("não deu para devolver {:s}: {:s}".format(target, str(ex)))
        _write_json(os.path.join(install_dir, MANIFEST_FILE), old)
        _write_json(os.path.join(install_dir, VERSION_FILE), current)
        raise
    finally:
        shutil.rmtree(work, ignore_errors=True)
    for target, kind in done:
        if kind != "added":
            _delete_soon(target + _OLD_SUFFIX)
    log("{:d} arquivo(s) trocado(s), {:d} removido(s)".format(len(changed), len(removed)))
    return info["version"]
```

- [ ] **Step 4: Escrever `apply.py`**

```python
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
    """True while any GameForge3DStudio.exe runs (every window of the Studio)."""
    if tasklist_output is None:
        tasklist_output = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq " + STUDIO_EXE, "/FO", "CSV", "/NH"],
            capture_output=True, text=True, creationflags=_NO_WINDOW).stdout
    return '"{:s}"'.format(STUDIO_EXE.lower()) in tasklist_output.lower()


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
    code = run_elevated(["apply", install_dir, package_path, envelope_path, log_path] + (["--dry-run"] if dry_run else []))
    if code is None:
        log(log_path, "RESULTADO adiado: o Windows não deu a permissão; o Studio oferece de novo na próxima vez")
    elif code != 0:
        log(log_path, "o aplicador terminou com o código {:d}".format(code))
    exe = os.path.join(install_dir, LAUNCHER_EXE)
    if not os.path.isfile(exe):
        exe = os.path.join(install_dir, STUDIO_EXE)
    subprocess.Popen([exe], cwd=install_dir, close_fds=True)


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
```

- [ ] **Step 5: Rodar e ver passar**

Run: `python tests/python/gameforge_update_test.py ApplyTest`
Esperado: `OK`, com 6 testes.

- [ ] **Step 6: Commit**

```bash
git add scripts/modules/gameforge_update/package.py scripts/modules/gameforge_update/apply.py tests/python/gameforge_update_test.py
git commit -m "GameForge: apply update packages with rollback, elevated after the Studio closes"
```

---

### Task 5: O Studio procura, pergunta, baixa e instala

**Files:**
- Modify: `scripts/modules/gameforge_update/__init__.py` (o arquivo inteiro abaixo)
- Modify: `scripts/modules/gameforge_account/portal.py` (imports `hashlib` e `http.client`; método `fetch_to_file`
  depois de `fetch`)
- Create: `scripts/startup/bl_operators/gameforge_update.py`
- Modify: `scripts/startup/bl_operators/__init__.py` (`_modules`, `register`, `unregister`)
- Create: `D:\claude-scratch\gf-tools\update_gui_check.py`
- Test: `tests/python/gameforge_update_test.py` (classes `DownloadTest` e `FetchTest`)

**Interfaces:**
- Consumes:
  - `signature.envelope_from_download`, `signature.envelope_ok`, `versions.newer`, `versions.installed`,
    `versions.ver_cmp`, `package.UpdateError`;
  - `Portal.plugin_versions(slug, token)`, `Portal.plugin_download(slug, token)`
    (`gameforge_account.portal`);
  - `gameforge_account.get()._portal` (o mesmo acesso do `gameforge_plugins.py`).
- Produces:
  - `gameforge_update.SLUG = "gameforge-3d-studio"`, `gameforge_update.INSTALLER_PAGE`;
  - `gameforge_update.NeedsInstaller`;
  - `gameforge_update.download(portal, session_token, installed_version, progress=None, pins=None)
    -> (package_path, envelope_path, version)`;
  - `gameforge_update.start_install(install_dir, package_path, envelope_path, python=sys.executable) -> log_path`;
  - `gameforge_update.last_result() -> str`;
  - `Portal.fetch_to_file(url, path, *, max_bytes, progress=None) -> str` (sha256 hex);
  - os operadores `gameforge.update_check`, `gameforge.update_show`, `gameforge.update_install` e
    `gameforge.update_installer_page`.

- [ ] **Step 1: Escrever os testes**

Acrescentar aos imports `import gameforge_update  # noqa: E402` e
`from gameforge_account.portal import NetworkError, Portal, PortalError  # noqa: E402`, e:

```python
class FakePortal:
    def __init__(self, answer, data):
        self.answer, self.data = answer, data

    def plugin_download(self, slug, token):
        assert (slug, token) == (gameforge_update.SLUG, "tok")
        return self.answer

    def fetch_to_file(self, url, path, *, max_bytes, progress=None):
        with open(path, "wb") as fh:
            fh.write(self.data)
        if progress is not None:
            progress(len(self.data), len(self.data))
        return hashlib.sha256(self.data).hexdigest()


class DownloadTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        old = os.environ.get("LOCALAPPDATA")
        os.environ["LOCALAPPDATA"] = self.tmp
        self.addCleanup(lambda: os.environ.__setitem__("LOCALAPPDATA", old) if old else os.environ.pop("LOCALAPPDATA"))

    def test_downloads_and_checks(self):
        data = b"pacote"
        answer = signed_answer("5.4.2", hashlib.sha256(data).hexdigest(), base="5.4.0")
        package_path, envelope_path, version = gameforge_update.download(
            FakePortal(answer, data), "tok", "5.4.1", pins=TEST_PINS)
        self.assertEqual(version, "5.4.2")
        with open(package_path, "rb") as fh:
            self.assertEqual(fh.read(), data)
        with open(envelope_path, encoding="utf-8") as fh:
            self.assertTrue(signature.envelope_ok(json.load(fh), TEST_PINS))

    def test_refusals(self):
        data = b"pacote"
        digest = hashlib.sha256(data).hexdigest()
        with self.assertRaises(gameforge_update.NeedsInstaller):
            gameforge_update.download(FakePortal(signed_answer("5.5.1", digest, base="5.5.0"), data), "tok", "5.4.2",
                                      pins=TEST_PINS)
        cases = (
            signed_answer("5.4.2", digest, seed=bytes(32)),             # Another key.
            signed_answer("5.4.2", hashlib.sha256(b"outro").hexdigest()),  # Not what was downloaded.
            signed_answer("5.4.2", digest, name="Setup.msi"),           # Not an update package.
            signed_answer("5.4.1", digest),                             # Already installed.
        )
        for answer in cases:
            with self.assertRaises(gameforge_update.UpdateError):
                gameforge_update.download(FakePortal(answer, data), "tok", "5.4.1", pins=TEST_PINS)
        self.assertEqual([n for n in os.listdir(gameforge_update.staging_dir()) if n.endswith((".zip", ".part"))], [])


class _Files(http.server.BaseHTTPRequestHandler):
    body = b"x" * (3 * 1024 * 1024 + 5)

    def do_GET(self):
        self.send_response(200)
        self.send_header("content-length", str(len(self.body)))
        self.end_headers()
        self.wfile.write(self.body if self.path == "/full" else self.body[:1000])  # "/short": the link drops.

    def log_message(self, *_args):
        pass


class FetchTest(unittest.TestCase):

    def setUp(self):
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Files)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.shutdown)
        self.base = "http://127.0.0.1:{:d}".format(self.server.server_address[1])
        self.portal = Portal(opener=urllib.request.build_opener())
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_streams_with_progress(self):
        seen = []
        path = os.path.join(self.tmp, "full.part")
        digest = self.portal.fetch_to_file(self.base + "/full", path, max_bytes=10 * 2 ** 20,
                                           progress=lambda done, total: seen.append((done, total)))
        self.assertEqual(digest, hashlib.sha256(_Files.body).hexdigest())
        self.assertEqual(seen[-1], (len(_Files.body), len(_Files.body)))
        self.assertGreater(len(seen), 1)

    def test_truncated_download_fails(self):
        with self.assertRaises(NetworkError):
            self.portal.fetch_to_file(self.base + "/short", os.path.join(self.tmp, "short.part"), max_bytes=10 * 2 ** 20)

    def test_too_big(self):
        with self.assertRaises(PortalError):
            self.portal.fetch_to_file(self.base + "/full", os.path.join(self.tmp, "big.part"), max_bytes=1000)
```

Acrescentar também `import urllib.request` aos imports do teste.

- [ ] **Step 2: Rodar e ver falhar**

Run: `python tests/python/gameforge_update_test.py DownloadTest FetchTest`
Esperado: `AttributeError: module 'gameforge_update' has no attribute 'download'`.

- [ ] **Step 3: `Portal.fetch_to_file`**

Em `scripts/modules/gameforge_account/portal.py`, acrescentar `import hashlib` e `import http.client` aos imports e,
depois de `fetch`:

```python
    def fetch_to_file(self, url, path, *, max_bytes, progress=None):
        """
        Download a signed storage URL into ``path`` in pieces, refusing more than ``max_bytes``. Returns the
        SHA-256 (hex) of what was written; ``progress(done, total)`` runs after each piece (``total`` is 0 when the
        server does not say). A connection that drops halfway raises ``NetworkError``.
        """
        digest = hashlib.sha256()
        request = urllib.request.Request(url, method="GET")
        try:
            with self._opener.open(request, timeout=_DOWNLOAD_TIMEOUT_SEC) as response, open(path, "wb") as fh:
                total = int(response.headers.get("content-length") or 0)
                done = 0
                while True:
                    chunk = response.read(1 << 20)
                    if not chunk:
                        break
                    done += len(chunk)
                    if done > max_bytes:
                        raise PortalError("arquivo maior que o esperado")
                    digest.update(chunk)
                    fh.write(chunk)
                    if progress is not None:
                        progress(done, total)
        except urllib.error.HTTPError as ex:
            raise PortalError("download recusado ({:d})".format(ex.code), status=ex.code) from ex
        except (urllib.error.URLError, OSError, ssl.SSLError, http.client.HTTPException) as ex:
            raise NetworkError("Não foi possível baixar de atomgameforge.com ({:s})".format(str(ex))) from ex
        return digest.hexdigest()
```

- [ ] **Step 4: `gameforge_update/__init__.py`**

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The Studio's own updates from atomgameforge.com, through the same ``plugin-download`` endpoint and /admin
publishing as the PWOps launcher. The plugin ``gameforge-3d-studio`` carries cumulative update packages
(``release/windows/gameforge/pack_update.py``); the full installer is on GitHub Releases.

The Studio asks before anything (``bl_operators.gameforge_update``), downloads and checks the package here, and
``apply.py`` applies it once the Studio closes, with the permission of Windows (the installation is per machine).
"""

__all__ = (
    "INSTALLER_PAGE",
    "NeedsInstaller",
    "SLUG",
    "UpdateError",
    "download",
    "last_result",
    "staging_dir",
    "start_install",
)

import json
import os
import subprocess
import sys

from . import signature
from .package import UpdateError
from .versions import ver_cmp

SLUG = "gameforge-3d-studio"
INSTALLER_PAGE = "https://atomgameforge.com/conta"
MAX_PACKAGE_BYTES = 50 * 1024 * 1024  # The portal's Storage limit (Supabase Free).
APPLY_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "apply.py")
LOG_NAME = "update.log"


class NeedsInstaller(Exception):
    """The published update starts from a version newer than the installed one: the full installer is needed."""


def staging_dir():
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
    path = os.path.join(base, "ATOM GameForge", "GameForge 3D Studio", "updates")
    os.makedirs(path, exist_ok=True)
    return path


def download(portal, session_token, installed_version, progress=None, pins=None):
    """Downloads the current update and checks it: ``(package path, envelope path, version)``. The signature is
    checked before downloading and the SHA-256 after, like the launcher does."""
    answer = portal.plugin_download(SLUG, session_token)
    base = str(answer.get("min_helper_version") or "")
    if base and ver_cmp(base, installed_version) > 0:
        raise NeedsInstaller(base)
    envelope = signature.envelope_from_download(answer)
    if envelope is None or not signature.envelope_ok(envelope, pins or signature.PINS):
        raise UpdateError("assinatura inválida: a atualização foi recusada")
    if not envelope["file_path"].lower().endswith(".zip"):
        raise UpdateError("o portal ofereceu um arquivo que não é um pacote de atualização")
    if ver_cmp(envelope["version"], installed_version) <= 0:
        raise UpdateError("a versão {:s} já está instalada".format(installed_version))
    folder = staging_dir()
    name = "{:s}-{:s}".format(SLUG, envelope["version"])
    package_path = os.path.join(folder, name + ".zip")
    part = package_path + ".part"
    try:
        digest = portal.fetch_to_file(answer["download_url"], part, max_bytes=MAX_PACKAGE_BYTES, progress=progress)
        if digest != envelope["file_hash"]:
            raise UpdateError("o arquivo baixado não confere com a assinatura")
        os.replace(part, package_path)
    finally:
        if os.path.exists(part):
            os.remove(part)
    envelope_path = os.path.join(folder, name + ".json")
    with open(envelope_path, "w", encoding="utf-8") as fh:
        json.dump(envelope, fh, ensure_ascii=False)
    return package_path, envelope_path, envelope["version"]


def start_install(install_dir, package_path, envelope_path, python=sys.executable):
    """Starts the applier's "watch" role, detached: it waits for the Studio to close. Returns the log path."""
    log = os.path.join(staging_dir(), LOG_NAME)
    subprocess.Popen([python, "-I", APPLY_SCRIPT, "watch", install_dir, package_path, envelope_path, log],
                     creationflags=0x00000008 | 0x00000200,  # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
                     close_fds=True)
    return log


def last_result():
    """The last applier outcome (a "RESULTADO ..." line without the prefix), once: the log is set aside after reading."""
    path = os.path.join(staging_dir(), LOG_NAME)
    try:
        with open(path, encoding="utf-8") as fh:
            lines = [line.split("RESULTADO ", 1)[1].strip() for line in fh if "RESULTADO " in line]
        os.replace(path, path + ".lido")
    except OSError:
        return ""
    return lines[-1] if lines else ""
```

- [ ] **Step 5: Rodar e ver passar**

Run: `python tests/python/gameforge_update_test.py`
Esperado: `OK`, com a suíte inteira do arquivo.

- [ ] **Step 6: Os operadores, a barra superior e o menu**

`scripts/startup/bl_operators/gameforge_update.py`:

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The Studio's own updates (``gameforge_update``). Once the account is authorized, a background check asks the
portal for versions newer than the installed one; the top bar then offers "Atualização <versão>", whose dialog shows
what changed and, when confirmed, downloads and checks the package. "Instalar e reiniciar" starts the applier and
closes the Studio. Development builds (no ``gameforge-version.json``) never check.
"""

__all__ = (
    "GAMEFORGE_OT_update_check",
    "GAMEFORGE_OT_update_show",
    "GAMEFORGE_OT_update_install",
    "GAMEFORGE_OT_update_installer_page",
)

import os
import textwrap
import threading
import time

import bpy
from bpy.types import Operator

import gameforge_account
import gameforge_update
from gameforge_update import versions

_TIMER_SEC = 10.0
_RECHECK_SEC = 6 * 3600.0


class _Update:
    lock = threading.Lock()
    thread = None
    next_check = 0.0
    current = None          # The installed {"version", "base_version"}; None in development builds.
    available = []          # Newer versions [{"version", "changelog", "published_at"}], oldest first.
    busy = ""               # What runs in the background ("Baixando a atualização 40%").
    staged = None           # (package, envelope, version): downloaded, checked, waiting for "Instalar".
    needs_installer = ""    # The base version of an update this installation cannot take.
    message = ""            # The last error, for the dialog.
    menus_added = False
    result_shown = False


def install_dir():
    return os.path.dirname(bpy.app.binary_path)


def _start_thread(target, *args):
    with _Update.lock:
        if _Update.thread is not None and _Update.thread.is_alive():
            return False
        _Update.thread = threading.Thread(target=target, args=args, name="gameforge-update", daemon=True)
        _Update.thread.start()
    return True


def _check(token):
    try:
        listing = gameforge_account.get()._portal.plugin_versions(gameforge_update.SLUG, token)
        _Update.available = versions.newer(listing, _Update.current["version"])
        _Update.message = ""
    except Exception as ex:  # noqa: BLE001 - offline, or nothing published yet: the next check tries again
        _Update.message = str(ex)
    _Update.next_check = time.monotonic() + _RECHECK_SEC


def start_check(force=False):
    """A background check when the account is authorized. False when it cannot run."""
    token = gameforge_account.get().session_token()
    if _Update.current is None or not token:
        return False
    if force or time.monotonic() >= _Update.next_check:
        _start_thread(_check, token)
    return True


def _download(token):
    def progress(done, total):
        _Update.busy = ("Baixando a atualização {:d}%".format(min(99, done * 100 // total)) if total else
                        "Baixando a atualização...")

    _Update.busy = "Baixando a atualização..."
    try:
        _Update.staged = gameforge_update.download(
            gameforge_account.get()._portal, token, _Update.current["version"], progress)
        _Update.message = ""
    except gameforge_update.NeedsInstaller as ex:
        _Update.needs_installer = str(ex)
    except Exception as ex:  # noqa: BLE001 - shown in the dialog; the next try starts over
        _Update.message = str(ex)
    _Update.busy = ""


def _draw_topbar(self, context):
    if context.region.alignment != 'RIGHT':  # The upper bar draws a left and a right region.
        return
    layout = self.layout
    if _Update.staged is not None:
        layout.operator("gameforge.update_install", text="Instalar a {:s} e reiniciar".format(_Update.staged[2]),
                        icon='FILE_REFRESH', translate=False)
    elif _Update.busy:
        layout.label(text=_Update.busy, icon='SORTTIME', translate=False)
    elif _Update.needs_installer:
        layout.operator("gameforge.update_installer_page", text="Baixe o instalador novo", icon='ERROR',
                        translate=False)
    elif _Update.available:
        layout.operator("gameforge.update_show", text="Atualização " + str(_Update.available[-1]["version"]),
                        icon='IMPORT', translate=False)


def _draw_menu(self, _context):
    if _Update.current is None:
        return
    self.layout.separator()
    self.layout.operator("gameforge.update_check", icon='FILE_REFRESH', translate=False)


def _redraw_topbars():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'TOPBAR':
                area.tag_redraw()


def _show_result(text):
    windows = bpy.context.window_manager.windows
    if not windows:
        return

    def draw(self, _context):
        self.layout.label(text=text, translate=False)

    with bpy.context.temp_override(window=windows[0]):
        bpy.context.window_manager.popup_menu(draw, title="Atualização do GameForge 3D Studio",
                                              icon='ERROR' if text.startswith("falhou") else 'INFO')


def _tick():
    if not _Update.menus_added and hasattr(bpy.types, "TOPBAR_HT_upper_bar") and hasattr(bpy.types, "TOPBAR_MT_blender"):
        # The top bar registers after the operators: the entries are added on the first tick.
        bpy.types.TOPBAR_HT_upper_bar.append(_draw_topbar)
        bpy.types.TOPBAR_MT_blender.append(_draw_menu)
        _Update.menus_added = True
    if not _Update.result_shown:
        _Update.result_shown = True
        result = gameforge_update.last_result()
        if result:
            _show_result(result)
    try:
        start_check()
    except Exception as ex:  # noqa: BLE001
        print("GameForge: falha ao procurar atualizações:", ex)
    _redraw_topbars()
    return 1.0 if _Update.busy else _TIMER_SEC


class GAMEFORGE_OT_update_check(Operator):
    """Look for a new version of the GameForge 3D Studio at atomgameforge.com"""
    bl_idname = "gameforge.update_check"
    bl_label = "Procurar atualizações"

    def execute(self, _context):
        if not start_check(force=True):
            self.report({'WARNING'}, "Entre com a conta atomgameforge.com para procurar atualizações")
            return {'CANCELLED'}
        self.report({'INFO'}, "Procurando atualizações do GameForge 3D Studio...")
        return {'FINISHED'}


class GAMEFORGE_OT_update_show(Operator):
    """See what is new in the GameForge 3D Studio and download the update"""
    bl_idname = "gameforge.update_show"
    bl_label = "Atualização do GameForge 3D Studio"

    @classmethod
    def poll(cls, _context):
        return bool(_Update.available) and _Update.current is not None

    def invoke(self, context, _event):
        return context.window_manager.invoke_props_dialog(self, width=560, confirm_text="Baixar agora")

    def draw(self, _context):
        layout = self.layout
        layout.label(text="Você tem a versão {:s}. O que há de novo:".format(_Update.current["version"]),
                     translate=False)
        for item in reversed(_Update.available):
            box = layout.box()
            box.label(text="Versão " + str(item["version"]), icon='INFO', translate=False)
            col = box.column(align=True)
            col.scale_y = 0.85
            for line in str(item.get("changelog") or "").splitlines():
                for piece in textwrap.wrap(line, 90) or [""]:
                    col.label(text=piece, translate=False)
        if _Update.message:
            layout.label(text=_Update.message, icon='ERROR', translate=False)
        layout.label(text="Depois de baixar, você escolhe quando instalar.", icon='INFO', translate=False)

    def execute(self, _context):
        token = gameforge_account.get().session_token()
        if not token:
            self.report({'WARNING'}, "Entre com a conta atomgameforge.com para baixar a atualização")
            return {'CANCELLED'}
        _Update.message = ""
        _start_thread(_download, token)
        return {'FINISHED'}


class GAMEFORGE_OT_update_install(Operator):
    """Close the Studio and install the update: Windows asks for permission once and the Studio opens again"""
    bl_idname = "gameforge.update_install"
    bl_label = "Instalar a atualização"

    @classmethod
    def poll(cls, _context):
        return _Update.staged is not None

    def invoke(self, context, event):
        return context.window_manager.invoke_confirm(
            self, event, title="Instalar a versão {:s}?".format(_Update.staged[2]),
            message="O Studio fecha (perguntando se quer salvar), o Windows pede permissão e o Studio abre de novo "
                    "atualizado. Se você cancelar o fechamento, a atualização é instalada quando o Studio fechar.",
            confirm_text="Instalar e reiniciar")

    def execute(self, _context):
        package_path, envelope_path, _version = _Update.staged
        gameforge_update.start_install(install_dir(), package_path, envelope_path)
        _Update.staged = None
        bpy.ops.wm.quit_blender('INVOKE_DEFAULT')
        return {'FINISHED'}


class GAMEFORGE_OT_update_installer_page(Operator):
    """This update needs the full installer: open the page to download it"""
    bl_idname = "gameforge.update_installer_page"
    bl_label = "Baixar o instalador"

    def execute(self, _context):
        bpy.ops.wm.url_open(url=gameforge_update.INSTALLER_PAGE)
        return {'FINISHED'}


classes = (
    GAMEFORGE_OT_update_check,
    GAMEFORGE_OT_update_show,
    GAMEFORGE_OT_update_install,
    GAMEFORGE_OT_update_installer_page,
)


def register():
    # The classes are registered by `bl_operators` (from `classes`), before this runs.
    if bpy.app.background:
        return
    _Update.current = versions.installed(install_dir())
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=_TIMER_SEC, persistent=True)


def unregister():
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    if _Update.menus_added:  # At exit the top bar may already be unregistered.
        for name, function in (("TOPBAR_HT_upper_bar", _draw_topbar), ("TOPBAR_MT_blender", _draw_menu)):
            owner = getattr(bpy.types, name, None)
            if owner is not None:
                owner.remove(function)
        _Update.menus_added = False
```

Em `scripts/startup/bl_operators/__init__.py`:
- `_modules`: acrescentar `"gameforge_update",` depois de `"gameforge_tasks",`;
- `register()`: acrescentar `gameforge_update.register()` depois de `gameforge_agent.register()`;
- `unregister()`: acrescentar `gameforge_update.unregister()` antes de `gameforge_agent.unregister()`.

- [ ] **Step 7: Conferir a interface com janela**

`D:\claude-scratch\gf-tools\update_gui_check.py`:

```python
"""GUI check of the update offer: fakes an installed 5.4.0 with a 5.4.1 published, captures the top bar and the
dialog. Usage: GameForge3DStudio.exe --factory-startup --no-window-focus --window-geometry 80 20 1760 990 ^
    --python update_gui_check.py -- <out dir>"""
import os
import sys
import time
import traceback

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from winshot import window_shot  # noqa: E402

OUT = sys.argv[sys.argv.index("--") + 1:][0]
os.makedirs(OUT, exist_ok=True)
bpy.context.preferences.view.show_splash = False
bpy.context.preferences.view.show_tooltips = False

from bl_operators import gameforge_update as update  # noqa: E402

update._Update.current = {"version": "5.4.0", "base_version": "5.4.0"}
update._Update.available = [{"version": "5.4.1", "changelog": "Painel de agentes de IA com o Claude.\n"
                                                              "Correções no editor de mapas."}]
update._Update.next_check = time.monotonic() + 3600  # No real check.


def steps():
    yield 2.0
    window_shot(os.path.join(OUT, "atualizacao-barra-janela.bmp"))
    window = bpy.context.window_manager.windows[0]
    with bpy.context.temp_override(window=window, screen=window.screen):
        bpy.ops.gameforge.update_show('INVOKE_DEFAULT')
    yield 1.5
    window_shot(os.path.join(OUT, "atualizacao-dialogo-janela.bmp"))


_steps = steps()


def _tick():
    try:
        return next(_steps)
    except StopIteration:
        pass
    except Exception:  # noqa: BLE001
        traceback.print_exc()
    print("DONE")
    with bpy.context.temp_override(window=bpy.context.window_manager.windows[0]):
        bpy.ops.wm.quit_blender()
    return None


bpy.app.timers.register(_tick, first_interval=3.0)
```

Run (PowerShell): `robocopy` dos scripts para `gf-build\bin\5.4\scripts`, depois
`gf-build\bin\GameForge3DStudio.exe --factory-startup --no-window-focus --window-geometry 80 20 1760 990 --python
D:\claude-scratch\gf-tools\update_gui_check.py -- D:\claude-scratch\gf-tools\update_out`, com
`GAMEFORGE_ACCOUNT_REQUIRED=0`.
Esperado: nos dois prints (convertidos para PNG), "Atualização 5.4.1" no canto direito da barra superior e o
diálogo com o changelog e "Baixar agora".

- [ ] **Step 8: Commit**

```bash
git add scripts/modules/gameforge_update/__init__.py scripts/modules/gameforge_account/portal.py scripts/startup/bl_operators/gameforge_update.py scripts/startup/bl_operators/__init__.py tests/python/gameforge_update_test.py
git commit -m "GameForge: offer, download and install Studio updates from the portal"
```

---

### Task 6: O plugin no portal

**Files:** nenhum. É o banco de produção, pelo conector do Supabase (projeto `cbwkbwaicqrqnjoopcju`).

- [ ] **Step 1: Mostrar o SQL ao usuário e rodar com o OK dele (dado na aprovação do plano)**

```sql
insert into public.plugins (slug, name, description, required_plan, is_active, category, sort_order, all_servers)
values ('gameforge-3d-studio', 'GameForge 3D Studio', 'Atualizações do GameForge 3D Studio para Windows',
        'pro', true, 'studio', 0, true)
returning id, slug, required_plan;
```

- [ ] **Step 2: Conferir**

Esperado:
- `select slug, required_plan, is_active from plugins where slug = 'gameforge-3d-studio'` devolve uma linha com
  `pro` e `true`;
- sem versão publicada, o `plugin-download` em modo lista responde `versions: []`. O Studio só não mostra nada.

---

### Task 7: MSI completo, base das atualizações e release no GitHub

**Files:** nenhum código novo. Builds, `instalador\`, `instalador\manifests\` e o repositório no GitHub.

- [ ] **Step 1: Publicar o código-fonte (GPL)**

Com o OK do usuário:
- commit do trabalho do editor que hoje está fora do git (`git status` lista 292 caminhos);
- push da branch `gameforge/angelica-editor`;
- PR rascunho para `main`;
- tag `v5.4.0` no commit do build.

```bash
cd "D:/Atom Sec Labs/ATOM GameForge/GameForge-3D-Studio"
git status --short | wc -l
git add -A && git status --short | grep -v "^[AM] " | head   # Nada fora do esperado (builds, logs).
git commit -m "GameForge: Angelica editor (client library, models, maps, spawns, records, quests, AI agent panel)"
git push -u origin gameforge/angelica-editor
gh pr create --draft --base main --title "GameForge 3D Studio 5.4.0" --body "Editor Angelica, painel de agentes de IA e atualização automática."
```

Esperado: push aceito, PR rascunho criado. A tag vem no Step 4, no commit exato do build.

- [ ] **Step 2: Build e MSI**

Run (PowerShell, sem Studio aberto):
1. `& "D:\claude-scratch\gf-tools\build_j6.bat"`, que deve terminar com `EXIT 0`;
2. a suíte da Task 2, Step 7;
3. `powershell -File "D:\Atom Sec Labs\ATOM GameForge\_gerar_instalador.ps1"`.

Esperado: `instalador\gameforge-3d-studio-5.4.0-<hash>-windows64.msi`.

- [ ] **Step 3: Conferir o MSI e registrar a base**

```powershell
$stage = Get-ChildItem "C:\gfpk\_CPack_Packages" -Recurse -Filter "gameforge-version.json" | Select-Object -First 1
Get-Content $stage.FullName   # {"base_version": "5.4.0", "build": "<hash>", "version": "5.4.0"}
python "D:\Atom Sec Labs\ATOM GameForge\GameForge-3D-Studio\release\windows\gameforge\pack_update.py" $stage.DirectoryName --record-installer --manifests "D:\Atom Sec Labs\ATOM GameForge\instalador\manifests"
```

Esperado: `base registrada: 5.4.0`, e `instalador\manifests\5.4.0.json` existe.

- [ ] **Step 4: Tag e release no GitHub**

```bash
cd "D:/Atom Sec Labs/ATOM GameForge/GameForge-3D-Studio"
git tag v5.4.0 && git push origin v5.4.0
cp "D:/Atom Sec Labs/ATOM GameForge/instalador/gameforge-3d-studio-5.4.0-"*"-windows64.msi" /d/claude-scratch/GameForge-3D-Studio-Setup.msi
gh release create v5.4.0 --repo shaddowdaniel/GameForge-3D-Studio- --title "GameForge 3D Studio 5.4.0" \
  --notes "Instalador do GameForge 3D Studio 5.4.0 para Windows. As próximas versões chegam pela atualização automática do Studio." \
  /d/claude-scratch/GameForge-3D-Studio-Setup.msi
curl -sI https://github.com/shaddowdaniel/GameForge-3D-Studio-/releases/latest/download/GameForge-3D-Studio-Setup.msi | head -3
```

Esperado: `HTTP/1.1 302 Found`, com `location:` para o arquivo do release.

- [ ] **Step 5: Atualizar a instalação de desenvolvimento**

Run: `python -I D:\claude-scratch\gf-tools\deploy_install.py`, sem Studio aberto.
Esperado: `copied ...`. `D:\ATOM GameForge\GameForge 3D Studio 5.4\gameforge-version.json` diz 5.4.0.

---

### Task 8: Botão de download no card da "Minha conta"

**Files:**
- Modify: `D:\claude-scratch\atomgameforge-site\src\pages\Account.tsx:351-388` (o card genérico das ferramentas)

- [ ] **Step 1: Branch nova a partir da `main`**

```bash
cd /d/claude-scratch/atomgameforge-site
git fetch origin main && git checkout -b claude/conta-baixar-studio origin/main
```

- [ ] **Step 2: O botão**

Perto do topo do arquivo, depois dos imports:

```tsx
// O instalador completo do GameForge 3D Studio (GitHub Releases, sempre o último); as versões seguintes chegam pela
// atualização automática do próprio Studio (plugin-download, plugin "gameforge-3d-studio").
const STUDIO_INSTALLER_URL =
  "https://github.com/shaddowdaniel/GameForge-3D-Studio-/releases/latest/download/GameForge-3D-Studio-Setup.msi";
```

No card genérico, depois do `<a href={openHref} ...>Abrir {t.name}</a>` (dentro do `t.included ? (...)`), trocar o
ramo `t.included` por:

```tsx
                        {t.included ? (
                          <>
                            <a
                              href={openHref}
                              className="text-xs uppercase tracking-wider font-display px-3 py-2 rounded-md bg-forge-orange text-background text-center hover:opacity-90"
                            >
                              Abrir {t.name}
                            </a>
                            {t.slug === "estudio" && (
                              <a
                                href={STUDIO_INSTALLER_URL}
                                className="text-xs uppercase tracking-wider font-display px-3 py-2 rounded-md border border-forge-orange/50 text-forge-orange text-center hover:bg-forge-orange/10"
                              >
                                Baixar o GameForge 3D Studio (Windows)
                              </a>
                            )}
                          </>
                        ) : (
```

- [ ] **Step 3: Lint e build**

```bash
node scripts/no-em-dash.mjs && npm run build
git checkout -- supabase/functions/mcp/index.ts 2>/dev/null; rm -f package-lock.json
git status --short
```

Esperado: o lint passa, o build passa e só `src/pages/Account.tsx` aparece modificado.

- [ ] **Step 4: Commit, push e PR rascunho**

```bash
git add src/pages/Account.tsx
git commit -m "Conta: botão para baixar o GameForge 3D Studio no card do Estúdio"
git push -u origin claude/conta-baixar-studio
gh pr create --draft --title "Minha conta: baixar o GameForge 3D Studio" --body "Botão no card da ferramenta Estúdio para o instalador do GitHub Releases (sempre o último)."
```

---

### Task 9: Verificação com o usuário

Estes passos precisam de você na frente do computador: o UAC e a instalação do MSI só aceitam o seu clique.

- [ ] **Step 1: Ensaio da permissão e do reinício (sem mudar arquivos)**

Sem Studio aberto, rodar:

```powershell
& "D:\ATOM GameForge\GameForge 3D Studio 5.4\5.4\python\bin\python.exe" -I "D:\ATOM GameForge\GameForge 3D Studio 5.4\5.4\scripts\modules\gameforge_update\apply.py" watch "D:\ATOM GameForge\GameForge 3D Studio 5.4" x.zip x.json "$env:TEMP\gf-update-test.log" --dry-run
```

Esperado:
- o Windows pede permissão (o usuário clica em Sim);
- o log termina com `RESULTADO teste: a permissão e o reinício funcionam`;
- o Studio abre sozinho, sem ser como administrador.

- [ ] **Step 2: Instalar o MSI publicado**

O usuário baixa pelo botão da "Minha conta" e instala. O MSI atualiza a instalação que já existe.

Esperado:
- o Studio abre pelo atalho do menu Iniciar sem o "reparo" do Windows Installer;
- o título diz "GameForge 3D Studio 5.4.0".

- [ ] **Step 3: A primeira atualização de verdade (5.4.1)**

1. Quando houver uma mudança real: subir `BLENDER_VERSION_PATCH` para 1, build, e rodar
   `pack_update.py gf-build\bin --changelog "..." --manifests instalador\manifests --out instalador`.
2. O usuário sobe o `gameforge-3d-studio-5.4.1-release.zip` em /admin › Versões.

Esperado:
- no Studio 5.4.0 instalado, "Atualização 5.4.1" aparece em até 10 s depois do login;
- "Baixar agora" mostra o progresso, e "Instalar a 5.4.1 e reiniciar" fecha o Studio;
- o UAC aparece e o Studio reabre com o aviso "atualizado para 5.4.1".
