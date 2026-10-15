# Frente 7(a) — GameForgeLink: ponte Studio ↔ Unreal Engine 5.7 e "Portar para Unreal Engine 5" — plano de implementação

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** com um clique ("Portar para Unreal Engine 5"), o mapa aberto no Studio (terreno, prédios, objetos de cena e spawns) aparece num projeto da UE 5.7 instalada como Landscape + atores marcados; posições editadas na Unreal voltam ao Studio como alterações pendentes, gravadas pelos fluxos de salvamento que já existem.

**Architecture:** o Studio fala com a Unreal pelo protocolo de Remote Execution do plugin Python da UE, com um cliente copiado da versão MIT das BlenderTools (com laço de `recv`). Dados grandes vão por arquivos numa pasta de troca (`manifest.json` + `.glb` + `.r16`); o socket só leva comandos curtos que avaliam funções do plugin "GameForgeLink" (MIT, repositório separado). O plugin tem um pacote Python (abre o nível do mapa, importa o manifesto, cria e atualiza atores, devolve as posições) e um módulo C++ de editor só para criar o Landscape (`ALandscape::Import`). O Studio continua dono dos arquivos do jogo: a Unreal nunca grava neles.

**Tech Stack:** Python 3 (Studio e plugin da UE), C++ (módulo de editor da UE 5.7.4, `RunUAT BuildPlugin`), protocolo Remote Execution da UE, glTF 2.0 binário (exportador `io_scene_gltf2` do Studio, importador Interchange da UE), RAW 16 bits para o Landscape, numpy (já vem no Studio).

**Spec:** `doc/gameforge/specs/2026-10-14-angelica-editor-design.md` (seção 7, "GameForge Engine e ponte Unreal").

## Global Constraints

- Licenças: Studio (GPL) e Engine (EULA da Epic) são programas separados que conversam por socket e arquivos; nenhum código de um entra no binário do outro.
- O `remote_execution.py` da própria Unreal é EULA e não entra no Studio; o cliente do Studio vem de `D:\claude-scratch\refs-blendertools\BlenderTools\send2ue\dependencies\remote_execution.py` (repositório MIT, "Copyright (c) 2020 - 2024 Epic Games, Inc."), com o texto MIT completo no cabeçalho.
- Plugin "GameForgeLink": licença MIT, repositório próprio em `D:\Atom Sec Labs\ATOM GameForge\GameForgeLink` (fora do repositório do Studio); build empacotado em `D:\Atom Sec Labs\ATOM GameForge\GameForgeLink-Build`.
- Engine alvo: UE 5.7.4 em `D:\Atom Sec Labs\ATOM GameForge\UE_5.7`; o Studio procura antes o registro `HKLM\SOFTWARE\EpicGames\Unreal Engine\5.7\InstalledDirectory`.
- Transporte: multicast `239.0.0.1:6766` (TTL 0, bind `127.0.0.1`), comandos TCP em `127.0.0.1:6776`; o socket só leva comandos curtos (`EvaluateStatement` de `gameforge_link.<função>(...)`), dados grandes vão por arquivos.
- Coordenadas: Unreal = `100 · (x, −z, y)` a partir do espaço da engine (rotação, sem espelho); a partir do Blender (escala 1): Unreal = `100 · (x, −y, z)`; terreno centrado (`x = −W/2 + col·g`, `z = +H/2 − row·g`, a origem vem do próprio `.trn2`).
- Heightmap em RAW 16 bits (`.r16`) em vez de PNG16: o módulo C++ lê direto, sem decodificar imagem.
- Atores marcados `gf:<tipo>:<mapa>:<id>` (tipos `building`, `model`, `effect`, `sound`, `spawn`; o Landscape leva `gf:terrain:<mapa>`) e uma segunda marca `gf.source:<arquivo do jogo>` (o "metadado `gf.source`" da especificação).
- A Unreal é editor espacial/visual; o Studio grava os arquivos do jogo só ao salvar, pelos fluxos existentes (`spawns.build_npcgen`, `scene.build_world`), com desfazer. Nada é apagado no Studio a partir da Unreal.
- Sem janelas durante o trabalho automático: o usuário costuma estar jogando PW. Testes da UE rodam com `UnrealEditor-Cmd.exe ... -unattended -nullrhi -nosplash -nopause`; a UE com janela só na verificação manual da Task 9, combinada com o usuário.
- Clientes de teste só para leitura, em PowerShell: `$env:GAMEFORGE_TEST_CLIENTS = "D:\Atom Sec Labs\ATOM GameForge\GameForge_PWServer\PWNH_v145\PWNH v145\element;D:\Atom Sec Labs\ATOM GameForge\GameForge_PWServer\1.7.8\2 - cliente178\element"`; mapa `GAMEFORGE_TEST_MAP` (padrão `a08`, como em `gameforge_scene_test.py`). O cliente PWU (`D:\jogos\...`) não é usado nesta frente.
- **Rodar no Studio** (definição usada em todas as tarefas): sincronizar os scripts e rodar o teste sem janela:

```powershell
robocopy "D:\Atom Sec Labs\ATOM GameForge\GameForge-3D-Studio\scripts" "D:\Atom Sec Labs\ATOM GameForge\gf-build\bin\5.4\scripts" /E /XD __pycache__ /NJH /NJS /NP /NFL /NDL
& "D:\Atom Sec Labs\ATOM GameForge\gf-build\bin\GameForge3DStudio.exe" -b --factory-startup --python "D:\Atom Sec Labs\ATOM GameForge\GameForge-3D-Studio\tests\python\<teste>.py"
```

- Mudança em C++ do Studio: compilar com `& "D:\claude-scratch\gf-tools\build_j6.bat"` (log em `D:\Atom Sec Labs\ATOM GameForge\_rebuild.log`, termina com `EXIT 0`); nunca rodar o executável durante a compilação.
- Commits: só quando o usuário pedir (regra do projeto). Cada tarefa termina num **checkpoint** (`git status --short` mostra só os arquivos da tarefa), não num commit.

## Review Focus

1. Malha e ator com eixos diferentes: se o importador de `.glb` da Unreal converter eixos de outro jeito que `coords`, os prédios aparecem espelhados ou girados embora os atores estejam no lugar certo. Esperado: a sonda `probe.glb` (um triângulo com cantos conhecidos) chega com os cantos em `100 · (x, −y, z)` — teste na Task 6.
2. Unreal fechada ou sem Remote Execution: o "Portar" abre o projeto, espera o nó responder (até 120 s) numa thread, com o Studio livre, e diz em português o que ligar quando ninguém responde — testes na Task 7.
3. Mapas grandes (`world`, 8192 × 11264 m): portar só a região em volta do cursor 3D, com o Landscape cortado no mesmo quadrado; sem região, recusar acima de 4096 quadrados por lado com mensagem clara — testes na Task 4.
4. Resultado maior que um `recv` (centenas de atores na volta): o cliente junta os pedaços até o JSON fechar, sem despejar o JSON parcial no log — teste na Task 2.
5. Ator apagado na Unreal: aparece como "não está mais na Unreal" e nada é apagado no Studio — teste na Task 8.

## Fatos conferidos (2026-10-14)

- O exportador glTF do Studio exporta uma coleção da biblioteca de modelos (`collection=<nome>`) mesmo com a biblioteca excluída da camada de vista, desde que ela esteja ligada à cena (`scene.model_library` já faz isso); uma coleção solta, fora da cena, sai vazia. Sonda: `D:\claude-scratch\gf-tools\probe_gltf_library.py`.
- A descoberta do Remote Execution funciona no Windows em loopback (nó falso achado em 0,3 s). O `_receive_message` original faz um único `recv(8192)`: com 3000 atores falha e ainda registra no log o JSON parcial inteiro. Sonda: `D:\claude-scratch\gf-tools\probe_remote_fake.py`.
- Em `EvaluateStatement`, a UE devolve `repr(valor)` (`PyUtil::PyObjectToUEStringRepr`); por isso o comando devolve `json.dumps(...)` e o Studio faz `json.loads(ast.literal_eval(result))`. O comando tem de ser uma única expressão (`__import__('gameforge_link')...`), sem `import`.
- UE 5.7: `Windows_SDK.json` pede MSVC ≥ 14.38.33130 e bane 14.44.0–14.44.35210, 14.40–14.43 e 14.39. Instalados: VS 2026 18.7 com MSVC 14.38.33130, 14.50 e 14.51 (aceitos); o 14.44.35207 instalado está banido. A UE 5.7 aceita VS 2026 (`MinimumVisualStudio2026Version` 18.0).
- Criação de Landscape na 5.7: `FLandscapeEditorDetailCustomization_NewLandscape::OnCreateButtonClicked` (`UE_5.7\Engine\Source\Editor\LandscapeEditor\Private\LandscapeEditorDetailCustomization_NewLandscape.cpp:1128`) é o modelo da Task 6.
- Os mapas ficam soltos em `<element>\maps\<mapa>\` (`a08.trn2`, `a08_1.t2bk`, `a08.ecwld`...) nos dois clientes de teste; `TerrainHeightfield`/`load_terrain_heightfield` (`source/blender/io/angelica/formats/angelica_terrain.hh:320,361`) já leem o terreno; nenhum binding Python expõe as alturas ainda.

---

### Task 1: Plugin GameForgeLink — esqueleto, compilação e teste sem janela

**Files:**
- Create: `D:\Atom Sec Labs\ATOM GameForge\GameForgeLink\GameForgeLink.uplugin`
- Create: `...\GameForgeLink\LICENSE`, `...\GameForgeLink\.gitignore`
- Create: `...\GameForgeLink\Content\Python\init_unreal.py`
- Create: `...\GameForgeLink\Content\Python\gameforge_link\__init__.py`
- Create: `...\GameForgeLink\Source\GameForgeLinkEditor\GameForgeLinkEditor.Build.cs`
- Create: `...\GameForgeLink\Source\GameForgeLinkEditor\Private\GameForgeLinkEditorModule.cpp`
- Create: `...\GameForgeLink\Tools\Build.ps1`, `Tools\MakeProject.ps1`, `Tools\RunTest.ps1`
- Test: `...\GameForgeLink\Tests\test_ping.py`

**Interfaces:**
- Produces: `gameforge_link.ping() -> dict` (`{"plugin": "GameForgeLink", "version": 1, "engine": "<versão>"}`); `Tools\Build.ps1` empacota o plugin em `GameForgeLink-Build`; `Tools\MakeProject.ps1 -Path <pasta>` cria `<pasta>\GameForge.uproject` com Python, Editor Scripting, Interchange e GameForgeLink ligados e Remote Execution ligado; `Tools\RunTest.ps1 -Project <uproject> -Script <py>` roda um script na UE sem janela e sai com 0 quando o log tem `GAMEFORGE_TEST OK`.

- [ ] **Step 1: repositório e descritor.**

```powershell
New-Item -ItemType Directory -Force "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink" | Out-Null
git -C "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink" init
```

`GameForgeLink.uplugin`:

```json
{
  "FileVersion": 3,
  "Version": 1,
  "VersionName": "0.1",
  "FriendlyName": "GameForgeLink",
  "Description": "Ligação do GameForge 3D Studio com a Unreal: importa mapas do Perfect World e devolve as posições editadas.",
  "Category": "GameForge",
  "CreatedBy": "ATOM GameForge",
  "EnabledByDefault": true,
  "CanContainContent": true,
  "Installed": false,
  "Modules": [
    { "Name": "GameForgeLinkEditor", "Type": "Editor", "LoadingPhase": "Default" }
  ],
  "Plugins": [
    { "Name": "PythonScriptPlugin", "Enabled": true },
    { "Name": "EditorScriptingUtilities", "Enabled": true }
  ]
}
```

(`CanContainContent` é obrigatório: o plugin Python só procura `Content\Python` em plugins com conteúdo.)

`LICENSE`: texto MIT padrão com `Copyright (c) 2026 ATOM GameForge`. `.gitignore`:

```
Binaries/
Intermediate/
```

- [ ] **Step 2: pacote Python com `ping`.**

`Content\Python\gameforge_link\__init__.py`:

```python
# SPDX-License-Identifier: MIT
"""GameForgeLink: the Unreal side of the GameForge 3D Studio link. The Studio evaluates
``gameforge_link.<function>(...)`` over Remote Execution; every function returns JSON-able values."""

import unreal

VERSION = 1


def ping():
    """What the Studio checks before sending work."""
    return {"plugin": "GameForgeLink", "version": VERSION, "engine": unreal.SystemLibrary.get_engine_version()}
```

`Content\Python\init_unreal.py`:

```python
# SPDX-License-Identifier: MIT
"""Run by the Python plugin at startup: loads ``gameforge_link`` (errors show in the Output Log)."""
import gameforge_link  # noqa: F401
```

- [ ] **Step 3: módulo C++ mínimo** (a biblioteca do Landscape entra na Task 6).

`Source\GameForgeLinkEditor\GameForgeLinkEditor.Build.cs`:

```csharp
// SPDX-License-Identifier: MIT
using UnrealBuildTool;

public class GameForgeLinkEditor : ModuleRules
{
	public GameForgeLinkEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "Landscape" });
	}
}
```

`Source\GameForgeLinkEditor\Private\GameForgeLinkEditorModule.cpp`:

```cpp
// SPDX-License-Identifier: MIT
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, GameForgeLinkEditor)
```

- [ ] **Step 4: scripts de compilação, projeto e teste.**

`Tools\Build.ps1` (a saída fica fora da pasta do plugin):

```powershell
# SPDX-License-Identifier: MIT
param([string]$Engine = "D:\Atom Sec Labs\ATOM GameForge\UE_5.7",
      [string]$Out = "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink-Build")
$plugin = (Resolve-Path "$PSScriptRoot\..\GameForgeLink.uplugin").Path
& "$Engine\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin "-Plugin=$plugin" "-Package=$Out" -TargetPlatforms=Win64
exit $LASTEXITCODE
```

`Tools\MakeProject.ps1`:

```powershell
# SPDX-License-Identifier: MIT
param([Parameter(Mandatory)] [string]$Path, [string]$Plugin = "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink-Build")
New-Item -ItemType Directory -Force "$Path\Config", "$Path\Plugins" | Out-Null
@'
{
  "FileVersion": 3,
  "EngineAssociation": "5.7",
  "Description": "GameForge",
  "Plugins": [
    { "Name": "PythonScriptPlugin", "Enabled": true },
    { "Name": "EditorScriptingUtilities", "Enabled": true },
    { "Name": "Interchange", "Enabled": true },
    { "Name": "GameForgeLink", "Enabled": true }
  ]
}
'@ | Set-Content -Encoding utf8 "$Path\GameForge.uproject"
@'
[/Script/PythonScriptPlugin.PythonScriptPluginSettings]
bRemoteExecution=True
RemoteExecutionMulticastGroupEndpoint=239.0.0.1:6766
RemoteExecutionMulticastBindAddress=127.0.0.1
RemoteExecutionMulticastTtl=0
'@ | Set-Content -Encoding utf8 "$Path\Config\DefaultEngine.ini"
# Copy-Item puts the folder *inside* an existing destination: remove the old copy first.
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$Path\Plugins\GameForgeLink"
Copy-Item -Recurse -Force $Plugin "$Path\Plugins\GameForgeLink"
```

`Tools\RunTest.ps1` (o analisador de linha de comando da UE corta valores com espaço, por isso o script vai para a pasta temporária, que não tem espaço):

```powershell
# SPDX-License-Identifier: MIT
param([Parameter(Mandatory)] [string]$Project, [Parameter(Mandatory)] [string]$Script,
      [string]$Engine = "D:\Atom Sec Labs\ATOM GameForge\UE_5.7", [switch]$Offscreen)
$temp = [System.IO.Path]::GetTempPath()
$copy = Join-Path $temp ("gf_" + (Split-Path $Script -Leaf))
Copy-Item -Force $Script $copy
$log = Join-Path $temp "gf_unreal_test.log"
Remove-Item -ErrorAction SilentlyContinue $log
$render = if ($Offscreen) { "-RenderOffscreen" } else { "-nullrhi" }
& "$Engine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" $Project "-ExecutePythonScript=$copy" `
    -unattended $render -nosplash -nopause -nosound "-abslog=$log" | Out-Null
$lines = Get-Content $log
$lines | Select-String -Pattern "GAMEFORGE_TEST|Error|Traceback|AssertionError" | Select-Object -First 40
if ($lines | Select-String -Pattern "GAMEFORGE_TEST OK" -Quiet) { exit 0 } else { exit 1 }
```

`Tests\test_ping.py`:

```python
# SPDX-License-Identifier: MIT
import unreal
import gameforge_link

try:
    info = gameforge_link.ping()
    assert info["plugin"] == "GameForgeLink" and info["version"] == 1, info
    print("GAMEFORGE_TEST OK", info)
finally:
    unreal.SystemLibrary.quit_editor()
```

- [ ] **Step 5: compilar e rodar o teste.**

```powershell
& "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink\Tools\Build.ps1"
& "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink\Tools\MakeProject.ps1" -Path "D:\claude-scratch\gf-ue-test"
& "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink\Tools\RunTest.ps1" -Project "D:\claude-scratch\gf-ue-test\GameForge.uproject" -Script "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink\Tests\test_ping.py"
```

Expected: `BUILD SUCCESSFUL` do RunUAT; depois `GAMEFORGE_TEST OK {'plugin': 'GameForgeLink', 'version': 1, 'engine': '5.7.4-...'}` e código 0. Se o UBT recusar o toolset escolhido (o 14.44.35207 instalado está banido), fixar um aceito em `%APPDATA%\Unreal Engine\UnrealBuildTool\BuildConfiguration.xml`:

```xml
<?xml version="1.0" encoding="utf-8" ?>
<Configuration xmlns="https://www.unrealengine.com/BuildConfiguration">
  <WindowsPlatform>
    <CompilerVersion>14.38.33130</CompilerVersion>
  </WindowsPlatform>
</Configuration>
```

e compilar de novo.

- [ ] **Step 6: checkpoint.** `git -C "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink" status --short` lista só os arquivos acima (sem `Binaries/`/`Intermediate/`).

---

### Task 2: Studio — cliente Remote Execution (`unreal.link`)

**Files:**
- Create: `scripts/modules/gameforge_angelica/unreal/__init__.py`
- Create: `scripts/modules/gameforge_angelica/unreal/remote_execution.py` (cópia MIT + laço de `recv`)
- Create: `scripts/modules/gameforge_angelica/unreal/link.py`
- Create: `tests/python/gameforge_unreal_fake.py` (nó falso da Unreal, usado também na Task 7)
- Test: `tests/python/gameforge_unreal_link_test.py`

**Interfaces:**
- Produces: `link.UnrealLink()` com `find_node(wait=0.0) -> dict | None` (dados do nó, com `"node_id"` e `"project_name"`), `connect(node_id) -> None`, `call(function, **kwargs) -> object` (avalia `gameforge_link.<function>(**kwargs)` na Unreal e devolve o valor JSON), `close()`; exceção `link.LinkError` (mensagem em português) para tudo que falhar na conexão. `gameforge_unreal_fake.FakeUnreal(answers)` (thread; `answers` = `{função: valor}`; `.commands` = comandos recebidos; `.stop` = `threading.Event`). Sem `bpy` nestes módulos (rodam em threads e em Python puro).

- [ ] **Step 1: o nó falso.** `tests/python/gameforge_unreal_fake.py`:

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
A fake Unreal Editor for the link tests: answers the Remote Execution discovery (UDP multicast
239.0.0.1:6766 on loopback), accepts the command connection and answers
``gameforge_link.<function>(...)`` calls from a table, recording the commands it got.
"""

import json
import re
import socket
import threading
import uuid

GROUP = ("239.0.0.1", 6766)
_CALL = re.compile(r"__import__\('gameforge_link'\)\.(\w+)\(")


class FakeUnreal(threading.Thread):

    def __init__(self, answers):
        super().__init__(daemon=True)
        self.node_id = str(uuid.uuid4())
        self.answers = answers
        self.commands = []
        self.stop = threading.Event()
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        self.udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.udp.bind(("127.0.0.1", GROUP[1]))
        self.udp.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        self.udp.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 0)
        self.udp.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton("127.0.0.1"))
        self.udp.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                            socket.inet_aton(GROUP[0]) + socket.inet_aton("127.0.0.1"))
        self.udp.settimeout(0.2)

    def _message(self, **fields):
        return json.dumps(dict(version=1, magic="ue_py", source=self.node_id, **fields)).encode()

    def run(self):
        try:
            while not self.stop.is_set():
                try:
                    message = json.loads(self.udp.recv(65536))
                except socket.timeout:
                    continue
                if message.get("source") == self.node_id:
                    continue
                if message["type"] == "ping":
                    self.udp.sendto(self._message(type="pong", data=dict(project_name="GameForge")), GROUP)
                elif message["type"] == "open_connection" and message.get("dest") == self.node_id:
                    address = (message["data"]["command_ip"], message["data"]["command_port"])
                    threading.Thread(target=self._serve, args=(address,), daemon=True).start()
        finally:
            self.udp.close()

    def _serve(self, address):
        with socket.create_connection(address) as sock:
            while not self.stop.is_set():
                try:
                    data = sock.recv(65536)
                except OSError:
                    return
                if not data:
                    return
                message = json.loads(data)
                command = message["data"]["command"]
                self.commands.append(command)
                value = self.answers[_CALL.search(command).group(1)]
                sock.sendall(self._message(type="command_result", dest=message["source"], data=dict(
                    success=True, command=command, result=repr(json.dumps(value)), output=[])))
```

- [ ] **Step 2: o teste.** `tests/python/gameforge_unreal_link_test.py`:

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for the Studio side of the Unreal link (``gameforge_angelica.unreal.link``) against a fake
Unreal node. Runs with plain Python (``python -I tests/python/gameforge_unreal_link_test.py``) or
inside the Studio.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "scripts", "modules"))
sys.path.insert(0, HERE)

from gameforge_angelica.unreal import link  # noqa: E402
from gameforge_unreal_fake import FakeUnreal  # noqa: E402


class LinkTest(unittest.TestCase):

    def test_call_with_large_result(self):
        actors = [{"tag": "gf:spawn:a08:{:d}".format(i), "location_cm": [i, 0, 0]} for i in range(3000)]
        fake = FakeUnreal({"ping": {"plugin": "GameForgeLink", "version": 1}, "read_actors": actors})
        fake.start()
        unreal = link.UnrealLink()
        try:
            node = unreal.find_node(wait=5.0)
            self.assertIsNotNone(node)
            self.assertEqual(node["project_name"], "GameForge")
            unreal.connect(node["node_id"])
            self.assertEqual(unreal.call("ping")["plugin"], "GameForgeLink")
            self.assertEqual(unreal.call("read_actors", map_code="a08"), actors)
            self.assertIn("map_code='a08'", fake.commands[-1])
        finally:
            unreal.close()
            fake.stop.set()
            fake.join()

    def test_no_node_is_reported(self):
        unreal = link.UnrealLink()
        try:
            self.assertIsNone(unreal.find_node(wait=1.5))
            with self.assertRaises(link.LinkError):
                unreal.call("ping")
        finally:
            unreal.close()


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
```

- [ ] **Step 3: rodar e ver falhar.**

Run: `python -I "D:\Atom Sec Labs\ATOM GameForge\GameForge-3D-Studio\tests\python\gameforge_unreal_link_test.py"`
Expected: FAIL com `ModuleNotFoundError: No module named 'gameforge_angelica.unreal'`.

- [ ] **Step 4: copiar o cliente MIT e trocar o `recv`.**

```powershell
New-Item -ItemType Directory -Force "D:\Atom Sec Labs\ATOM GameForge\GameForge-3D-Studio\scripts\modules\gameforge_angelica\unreal" | Out-Null
Copy-Item "D:\claude-scratch\refs-blendertools\BlenderTools\send2ue\dependencies\remote_execution.py" "D:\Atom Sec Labs\ATOM GameForge\GameForge-3D-Studio\scripts\modules\gameforge_angelica\unreal\remote_execution.py"
```

No topo do arquivo copiado, acima da linha `# Copyright Epic Games, Inc. All Rights Reserved.` que já existe:

```python
# SPDX-License-Identifier: MIT
#
# From EpicGames/BlenderTools (send2ue/dependencies/remote_execution.py):
#
# MIT License
#
# Copyright (c) 2020 - 2024 Epic Games, Inc.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#
# Changed for GameForge 3D Studio: _RemoteExecutionCommandConnection._receive_message reads
# until a whole JSON message arrived (a long command result comes in several pieces).
```

Trocar o método `_receive_message` de `_RemoteExecutionCommandConnection` (hoje um único `recv`) por:

```python
    def _receive_message(self, expected_type):
        '''
        Receive a message over the TCP socket from the remote party, reading until the whole JSON
        message arrived (a long result comes in several pieces).

        Args:
            expected_type (string): The type of message we expect to receive.

        Returns:
            The message that was received.
        '''
        data = b''
        while True:
            chunk = self._command_channel_socket.recv(DEFAULT_RECEIVE_BUFFER_SIZE)
            if not chunk:
                break
            data += chunk
            if not chunk.rstrip().endswith(b'}'):
                continue
            try:
                _json.loads(data.decode('utf-8'))
            except ValueError:
                continue  # A "}" inside the message, not its end.
            message = _RemoteExecutionMessage(None, None)
            if message.from_json_bytes(data) and message.passes_receive_filter(self._node_id) and message.type_ == expected_type:
                return message
            break
        raise RuntimeError('Remote party failed to send a valid response!')
```

(O `_json.loads` só roda quando o pedaço termina em `}`: evita tentar decodificar a cada pedaço e evita o `from_json` registrar o JSON parcial no log.)

- [ ] **Step 5: o invólucro do Studio.**

`scripts/modules/gameforge_angelica/unreal/__init__.py`:

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The link between the Studio and Unreal Engine 5 (plugin GameForgeLink on the Unreal side)."""
```

`scripts/modules/gameforge_angelica/unreal/link.py`:

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Calls into a running Unreal Editor over Remote Execution: ``call("import_manifest", path=...)``
evaluates ``gameforge_link.import_manifest(path=...)`` there and returns its value (JSON). Large data
travels in files; only short commands go over the socket. No ``bpy`` here (runs on threads too).
"""

__all__ = ("LinkError", "UnrealLink")

import ast
import json
import time

from . import remote_execution


class LinkError(Exception):
    pass


class UnrealLink:
    def __init__(self):
        self._remote = remote_execution.RemoteExecution()
        self._remote.start()

    def find_node(self, wait=0.0):
        """The first Unreal Editor answering on this machine (waiting up to ``wait`` seconds), or None."""
        end = time.monotonic() + wait
        while True:
            nodes = self._remote.remote_nodes
            if nodes:
                return nodes[0]
            if time.monotonic() >= end:
                return None
            time.sleep(0.2)

    def connect(self, node_id):
        try:
            self._remote.open_command_connection(node_id)
        except (OSError, RuntimeError) as ex:
            raise LinkError("A Unreal não abriu a conexão de comandos: {}".format(ex)) from ex

    def call(self, function, **kwargs):
        """``gameforge_link.<function>(**kwargs)`` evaluated in Unreal; arguments and result are JSON-able."""
        if not self._remote.has_command_connection():
            raise LinkError("A Unreal não está conectada: abra o projeto com o plugin GameForgeLink e o Remote "
                            "Execution do Python ligado")
        arguments = ", ".join("{:s}={!r}".format(key, value) for key, value in sorted(kwargs.items()))
        # One expression (EvaluateStatement); Unreal answers with repr() of the JSON text.
        command = "__import__('json').dumps(__import__('gameforge_link').{:s}({:s}))".format(function, arguments)
        try:
            result = self._remote.run_command(command, unattended=True,
                                              exec_mode=remote_execution.MODE_EVAL_STATEMENT)
        except (OSError, RuntimeError) as ex:
            raise LinkError("A conexão com a Unreal caiu durante {:s}: {}".format(function, ex)) from ex
        if not result.get("success"):
            raise LinkError("A Unreal recusou {:s}: {}".format(function, result.get("result")))
        return json.loads(ast.literal_eval(result["result"]))

    def close(self):
        self._remote.stop()
```

- [ ] **Step 6: rodar.**

Run: `python -I "D:\Atom Sec Labs\ATOM GameForge\GameForge-3D-Studio\tests\python\gameforge_unreal_link_test.py"`
Expected: `Ran 2 tests ... OK`, sem linhas `Failed to deserialize JSON` na saída. Depois, o mesmo arquivo **rodar no Studio**: OK.

- [ ] **Step 7: checkpoint.** `git status --short` mostra `scripts/modules/gameforge_angelica/unreal/` e os dois arquivos de teste.

---

### Task 3: Coordenadas Studio ↔ Unreal (`unreal.coords`)

**Files:**
- Create: `scripts/modules/gameforge_angelica/unreal/coords.py`
- Test: `tests/python/gameforge_unreal_coords_test.py`

**Interfaces:**
- Produces: `coords.blender_to_unreal(matrix) -> dict` com `location_cm` (3 floats), `x_axis`, `z_axis` (vetores unitários no mundo da Unreal) e `scale` (3 floats); `coords.unreal_to_blender(location_cm, x_axis, z_axis, scale) -> mathutils.Matrix`; `coords.engine_to_unreal(pos) -> tuple`. Valem para objetos abertos com escala 1.

- [ ] **Step 1: o teste.**

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Tests for the Studio <-> Unreal conversions. Run inside the Studio (needs mathutils)."""

import math
import sys
import unittest

from mathutils import Euler, Matrix, Vector

from gameforge_angelica.unreal import coords


class CoordsTest(unittest.TestCase):

    def test_engine_to_unreal(self):
        # Engine x east, y up, z north; Unreal X forward, Y right, Z up, centimeters.
        self.assertEqual(coords.engine_to_unreal((1.0, 2.0, 3.0)), (100.0, -300.0, 200.0))

    def test_round_trip(self):
        m = Matrix.LocRotScale(Vector((12.5, -3.0, 40.0)), Euler((0.3, -0.7, 2.1)), Vector((1.0, 2.0, 0.5)))
        u = coords.blender_to_unreal(m)
        self.assertAlmostEqual(u["location_cm"][1], 300.0)  # Blender y -> Unreal -Y.
        back = coords.unreal_to_blender(u["location_cm"], u["x_axis"], u["z_axis"], u["scale"])
        for a, b in zip(m.col, back.col):
            for x, y in zip(a, b):
                self.assertAlmostEqual(x, y, places=4)

    def test_axes_are_a_rotation(self):
        u = coords.blender_to_unreal(Matrix.Rotation(math.radians(30.0), 4, 'Z'))
        x, z = Vector(u["x_axis"]), Vector(u["z_axis"])
        self.assertAlmostEqual(x.length, 1.0, places=6)
        self.assertAlmostEqual(x.dot(z), 0.0, places=6)
        # A turn of +30° about Blender Z is a turn of -30° about Unreal Z (Y is mirrored).
        self.assertAlmostEqual(math.degrees(math.atan2(x.y, x.x)), -30.0, places=4)


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
```

- [ ] **Step 2:** **rodar no Studio** `gameforge_unreal_coords_test`. Expected: FAIL (`ImportError: cannot import name 'coords'`).

- [ ] **Step 3: implementação.**

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Studio (Blender, Z up, meters) <-> Unreal (Z up, Y mirrored, centimeters): Unreal = 100 * (x, -y, z)
from Blender, and 100 * (x, -z, y) from the engine (Y up). Turns convert by conjugating with the Y
mirror; Unreal builds its rotator from the X and Z axes (``MathLibrary.make_rot_from_xz``). Scale
stays per local axis.
"""

__all__ = ("blender_to_unreal", "engine_to_unreal", "unreal_to_blender")

from mathutils import Matrix, Vector

_MIRROR = Matrix.Diagonal((1.0, -1.0, 1.0))


def engine_to_unreal(pos):
    return (pos[0] * 100.0, -pos[2] * 100.0, pos[1] * 100.0)


def blender_to_unreal(matrix):
    location, rotation, scale = matrix.decompose()
    turn = _MIRROR @ rotation.to_matrix() @ _MIRROR
    return {
        "location_cm": (location.x * 100.0, -location.y * 100.0, location.z * 100.0),
        "x_axis": tuple(turn.col[0]),
        "z_axis": tuple(turn.col[2]),
        "scale": tuple(scale),
    }


def unreal_to_blender(location_cm, x_axis, z_axis, scale):
    x = Vector(x_axis).normalized()
    z = Vector(z_axis).normalized()
    turn = Matrix((x, z.cross(x), z)).transposed()
    rotation = _MIRROR @ turn @ _MIRROR
    location = Vector((location_cm[0] / 100.0, -location_cm[1] / 100.0, location_cm[2] / 100.0))
    return Matrix.LocRotScale(location, rotation.to_quaternion(), Vector(scale))
```

- [ ] **Step 4:** **rodar no Studio** de novo. Expected: `Ran 3 tests ... OK`.

- [ ] **Step 5: checkpoint.** `git status --short` mostra `coords.py` e o teste.

---

### Task 4: Alturas do terreno para Landscape (binding C++ + `unreal.terrain`)

**Files:**
- Modify: `source/blender/io/angelica/IO_angelica_python.cc` (include `angelica_terrain.hh`, função `terrain_heights`, linha na tabela `methods[]`)
- Create: `scripts/modules/gameforge_angelica/unreal/terrain.py`
- Test: `tests/python/gameforge_unreal_terrain_test.py`

**Interfaces:**
- Produces: `_angelica.terrain_heights(map_dir, map_code) -> dict` com `grid` (m), `origin_x`, `origin_z` (posição na engine do vértice noroeste), `verts_x`, `verts_z`, `heights` (bytes float32, linha 0 ao norte); `ValueError` quando o terreno não abre. `terrain.from_heights(field, center=None, radius=0.0, component_quads=63) -> dict` e `terrain.landscape(map_dir, map_code, center=None, radius=0.0, component_quads=63) -> dict`, ambos com `raw16` (bytes `<u2`, `size_x × size_y`, linha 0 ao norte), `size_x`, `size_y`, `component_quads`, `location_cm` (canto noroeste), `scale` (3 floats). `terrain.MAX_QUADS = 4096`. `center` = (x, z) da engine.

- [ ] **Step 1: o teste** (casos sintéticos sem cliente + o mapa real só para leitura).

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Tests for the terrain -> Landscape heightmap. GAMEFORGE_TEST_CLIENTS (first client) is only read."""

import array
import os
import sys
import unittest

import _angelica
from gameforge_angelica.unreal import terrain

CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]
MAP = os.environ.get("GAMEFORGE_TEST_MAP", "a08")


def field(verts_x, verts_z, height=lambda row, col: row * 10.0 + col):
    """A terrain like ``_angelica.terrain_heights`` returns: grid 2 m, north-west vertex at (-4, 3)."""
    values = array.array("f", (height(r, c) for r in range(verts_z) for c in range(verts_x)))
    return {"grid": 2.0, "origin_x": -4.0, "origin_z": 3.0, "verts_x": verts_x, "verts_z": verts_z,
            "heights": values.tobytes()}


def decode(land, row, col):
    """Height in meters of a landscape vertex (32768 is the actor's height, one unit is scale.z / 128 cm)."""
    raw = array.array("H", land["raw16"])
    return ((raw[row * land["size_x"] + col] - 32768) * land["scale"][2] / 128.0 + land["location_cm"][2]) / 100.0


class FromHeightsTest(unittest.TestCase):

    def test_whole_components_and_values(self):
        land = terrain.from_heights(field(5, 4), component_quads=7)
        self.assertEqual((land["size_x"], land["size_y"]), (8, 8))
        self.assertEqual(len(land["raw16"]), 2 * 8 * 8)
        for row, col in ((0, 0), (3, 4), (2, 1)):
            self.assertAlmostEqual(decode(land, row, col), row * 10.0 + col, delta=0.001)
        # The last row and column are repeated to fill the component.
        self.assertAlmostEqual(decode(land, 7, 7), 34.0, delta=0.001)
        self.assertAlmostEqual(decode(land, 1, 6), 14.0, delta=0.001)
        self.assertEqual(tuple(land["location_cm"][:2]), (-400.0, -300.0))
        self.assertEqual(tuple(land["scale"][:2]), (200.0, 200.0))

    def test_region(self):
        # Columns 1..3 and rows 1..2 of the source.
        land = terrain.from_heights(field(5, 4), center=(0.0, 0.0), radius=1.0, component_quads=7)
        self.assertEqual(tuple(land["location_cm"][:2]), (-200.0, -100.0))
        self.assertAlmostEqual(decode(land, 0, 0), 11.0, delta=0.001)
        self.assertAlmostEqual(decode(land, 1, 2), 23.0, delta=0.001)

    def test_flat(self):
        land = terrain.from_heights(field(3, 3, height=lambda r, c: 5.0), component_quads=7)
        self.assertAlmostEqual(decode(land, 1, 1), 5.0, delta=0.001)

    def test_too_big_or_outside_is_refused(self):
        with self.assertRaisesRegex(ValueError, "cursor"):
            terrain.from_heights(field(terrain.MAX_QUADS + 2, 2))
        with self.assertRaisesRegex(ValueError, "fora do terreno"):
            terrain.from_heights(field(5, 4), center=(1000.0, 1000.0), radius=1.0, component_quads=7)


@unittest.skipUnless(CLIENTS, "set GAMEFORGE_TEST_CLIENTS")
class MapTest(unittest.TestCase):

    def test_map_heights(self):
        map_dir = os.path.join(CLIENTS[0], "maps", MAP)
        source = _angelica.terrain_heights(map_dir, MAP)
        self.assertEqual(len(source["heights"]), 4 * source["verts_x"] * source["verts_z"])
        self.assertGreater(source["grid"], 0.0)
        land = terrain.landscape(map_dir, MAP)
        self.assertEqual((land["size_x"] - 1) % land["component_quads"], 0)
        self.assertGreaterEqual(land["size_x"], source["verts_x"])
        heights = array.array("f", source["heights"])
        row, col = source["verts_z"] // 2, source["verts_x"] // 3
        self.assertAlmostEqual(decode(land, row, col), heights[row * source["verts_x"] + col], delta=0.05)

    def test_missing_map(self):
        with self.assertRaises(ValueError):
            _angelica.terrain_heights(os.path.join(CLIENTS[0], "maps"), "nao_existe")


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
```

- [ ] **Step 2:** **rodar no Studio**. Expected: FAIL (`ImportError: cannot import name 'terrain'`).

- [ ] **Step 3: o binding.** Em `IO_angelica_python.cc`, junto dos outros includes: `#include "angelica_terrain.hh"`. Antes da tabela `methods[]`:

```cpp
PyDoc_STRVAR(py_terrain_heights_doc,
             ".. function:: terrain_heights(map_dir, map_code)\n"
             "\n"
             "   The height grid of a map's terrain (``<map_dir>/<map_code>.trn2`` and its blocks): a dict\n"
             "   with ``grid`` (meters), ``origin_x``, ``origin_z`` (engine position of the north-west\n"
             "   vertex), ``verts_x``, ``verts_z`` and ``heights`` (float32 bytes, row-major, row 0 north).\n"
             "   Raises ValueError when the terrain cannot be read.\n");
static PyObject *py_terrain_heights(PyObject * /*self*/, PyObject *args)
{
  const char *map_dir, *map_code;
  if (!PyArg_ParseTuple(args, "ss:terrain_heights", &map_dir, &map_code)) {
    return nullptr;
  }
  TerrainHeightfield field;
  std::string error;
  bool ok;
  Py_BEGIN_ALLOW_THREADS;
  ok = load_terrain_heightfield(map_dir, map_code, field, &error);
  Py_END_ALLOW_THREADS;
  if (!ok) {
    PyErr_SetString(PyExc_ValueError, error.c_str());
    return nullptr;
  }
  PyObject *heights = PyBytes_FromStringAndSize(reinterpret_cast<const char *>(field.heights.data()),
                                                Py_ssize_t(field.heights.size() * sizeof(float)));
  return Py_BuildValue("{s:d,s:d,s:d,s:i,s:i,s:N}",
                       "grid", double(field.grid_size),
                       "origin_x", double(field.origin_x),
                       "origin_z", double(field.origin_z),
                       "verts_x", field.verts_x,
                       "verts_z", field.verts_z,
                       "heights", heights);
}
```

E na tabela `methods[]`: `{"terrain_heights", py_terrain_heights, METH_VARARGS, py_terrain_heights_doc},`.

- [ ] **Step 4: o módulo.** `scripts/modules/gameforge_angelica/unreal/terrain.py`:

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
A map's terrain as an Unreal Landscape heightmap: 16-bit heights (32768 is the landscape actor's
height, one unit is scale.z / 128 cm) in whole components (every source vertex kept; the east and
south edges repeat to fill the last component), placed so that each vertex lands on the engine
position of the same vertex (Unreal = 100 * (x, -z, y)).
"""

__all__ = ("MAX_QUADS", "from_heights", "landscape")

import math

import numpy as np

MAX_QUADS = 4096


def from_heights(field, center=None, radius=0.0, component_quads=63):
    """``field`` as returned by ``_angelica.terrain_heights``; ``center`` (engine x, z) and ``radius``
    keep a square around a place."""
    grid, verts_x, verts_z = field["grid"], field["verts_x"], field["verts_z"]
    col0, row0, col1, row1 = 0, 0, verts_x - 1, verts_z - 1
    if center is not None and radius > 0.0:
        col0 = max(col0, math.floor((center[0] - radius - field["origin_x"]) / grid))
        col1 = min(col1, math.ceil((center[0] + radius - field["origin_x"]) / grid))
        row0 = max(row0, math.floor((field["origin_z"] - center[1] - radius) / grid))
        row1 = min(row1, math.ceil((field["origin_z"] - center[1] + radius) / grid))
        if col0 >= col1 or row0 >= row1:
            raise ValueError("a região escolhida fica fora do terreno")
    quads_x, quads_z = col1 - col0, row1 - row0
    if max(quads_x, quads_z) > MAX_QUADS:
        raise ValueError("terreno grande demais para um Landscape ({:d} x {:d} quadrados): porte só a região em "
                         "volta do cursor 3D".format(quads_x, quads_z))
    size_x = math.ceil(quads_x / component_quads) * component_quads + 1
    size_y = math.ceil(quads_z / component_quads) * component_quads + 1
    heights = np.frombuffer(field["heights"], dtype=np.float32).reshape(verts_z, verts_x)
    region = heights[row0:row1 + 1, col0:col1 + 1] * np.float32(100.0)  # Centimeters.
    region = np.pad(region, ((0, size_y - region.shape[0]), (0, size_x - region.shape[1])), mode="edge")
    low, high = float(region.min()), float(region.max())
    mid = (low + high) / 2.0
    z_scale = max((high - low) / 2.0, 1.0) * 1.01 / 256.0  # The 16-bit range covers +-256 * scale.z cm.
    raw = np.clip(np.rint(32768.0 + (region - mid) * (128.0 / z_scale)), 0, 65535).astype("<u2")
    west = field["origin_x"] + col0 * grid
    north = field["origin_z"] - row0 * grid
    return {
        "raw16": raw.tobytes(),
        "size_x": size_x,
        "size_y": size_y,
        "component_quads": component_quads,
        "location_cm": (west * 100.0, -north * 100.0, mid),
        "scale": (grid * 100.0, grid * 100.0, z_scale),
    }


def landscape(map_dir, map_code, center=None, radius=0.0, component_quads=63):
    import _angelica
    return from_heights(_angelica.terrain_heights(map_dir, map_code), center, radius, component_quads)
```

(`component_quads` = 63 é um tamanho de seção aceito pela UE: 7/15/31/63/127/255.)

- [ ] **Step 5:** compilar o Studio (`build_j6.bat`, `EXIT 0`) e **rodar no Studio** `gameforge_unreal_terrain_test`. Expected: `Ran 6 tests ... OK`.

- [ ] **Step 6: checkpoint.** `git status --short` mostra `IO_angelica_python.cc`, `terrain.py` e o teste.

---

### Task 5: Pasta de troca (`unreal.export`) e dados de teste da Unreal

**Files:**
- Create: `scripts/modules/gameforge_angelica/unreal/export.py`
- Create: `tests/python/gameforge_unreal_test_data.py` (gera os dados dos testes das Tasks 6 e 8)
- Test: `tests/python/gameforge_unreal_export_test.py`

**Interfaces:**
- Consumes: `coords.blender_to_unreal`, `terrain.landscape`; `scene.scene_collections()`, `scene.PROP_FILE/PROP_CLIENT/PROP_SCALE/PROP_KIND/PROP_ID/PROP_PATH`; prédios são objetos com `instance_collection` cuja coleção tem `["gameforge_model"]` = caminho completo do `.bmd`; `spawns.spawn_collections()`, `spawns.PROP_FILE` (`"<mapa>\\npcgen.data"`), `ob.gameforge_spawn.kind` / `.index`.
- Produces: `export.KINDS` (`{'BUILDING': "building", 'MODEL': "model", 'EFFECT': "effect", 'SOUND': "sound"}`); `export.tagged_objects(map_code, client_root=None) -> {tag: (kind, object, source)}`; `export.write_manifest(folder, client_root, map_code, center=None, radius=0.0) -> str` (caminho do `manifest.json`; `ValueError` se algum mapa aberto tiver escala ≠ 1 ou o terreno não abrir). Manifesto versão 1:

```json
{"format": "gameforge-link", "version": 1, "client": "...", "map": "a08",
 "terrain": {"file": "terrain.r16", "size_x": 505, "size_y": 505, "component_quads": 63,
             "location_cm": [x, y, z], "scale": [sx, sy, sz]},
 "assets": [{"id": "litmodels\\a08\\1\\litmodel_11.bmd", "file": "assets/litmodels_a08_1_litmodel_11.glb"}],
 "actors": [{"tag": "gf:building:a08:76", "kind": "building", "label": "litmodel_11",
             "source": "litmodels\\a08\\1\\litmodel_11.bmd", "asset": "litmodels\\a08\\1\\litmodel_11.bmd",
             "location_cm": [..], "x_axis": [..], "z_axis": [..], "scale": [..]}]}
```

- [ ] **Step 1: o teste.**

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Tests for the Unreal exchange folder. GAMEFORGE_TEST_CLIENTS (first client) is only read."""

import json
import os
import shutil
import struct
import sys
import tempfile
import unittest

from gameforge_angelica import scene
from gameforge_angelica.unreal import export

CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]
MAP = os.environ.get("GAMEFORGE_TEST_MAP", "a08")


def glb_meshes(path):
    with open(path, "rb") as fh:
        data = fh.read()
    length = struct.unpack_from("<I", data, 12)[0]
    return len(json.loads(data[20:20 + length]).get("meshes", []))


@unittest.skipUnless(CLIENTS, "set GAMEFORGE_TEST_CLIENTS")
class ExportTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.collection, _problems = scene.load_map_scene(CLIENTS[0], MAP, building_limit=3)

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="gameforge-unreal-export-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_manifest(self):
        with open(export.write_manifest(self.tmp, CLIENTS[0], MAP), encoding="utf-8") as fh:
            manifest = json.load(fh)
        self.assertEqual((manifest["format"], manifest["version"], manifest["map"]), ("gameforge-link", 1, MAP))
        land = manifest["terrain"]
        self.assertEqual(os.path.getsize(os.path.join(self.tmp, land["file"])), 2 * land["size_x"] * land["size_y"])
        actors = manifest["actors"]
        self.assertEqual(len({a["tag"] for a in actors}), len(actors))
        for actor in actors:
            self.assertTrue(actor["tag"].startswith("gf:{:s}:{:s}:".format(actor["kind"], MAP)), actor["tag"])
            self.assertEqual("asset" in actor, actor["kind"] == "building")
        buildings = [a for a in actors if a["kind"] == "building"]
        self.assertTrue(1 <= len(buildings) <= 3, len(buildings))
        files = {a["id"]: a["file"] for a in manifest["assets"]}
        for building in buildings:
            self.assertGreater(glb_meshes(os.path.join(self.tmp, files[building["asset"]])), 0)

    def test_scale_must_be_one(self):
        self.collection[scene.PROP_SCALE] = 0.01
        try:
            with self.assertRaisesRegex(ValueError, "escala 1"):
                export.write_manifest(self.tmp, CLIENTS[0], MAP)
        finally:
            self.collection[scene.PROP_SCALE] = 1.0


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
```

- [ ] **Step 2:** **rodar no Studio**. Expected: FAIL (`ImportError: cannot import name 'export'`).

- [ ] **Step 3: implementação.** `scripts/modules/gameforge_angelica/unreal/export.py`:

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The exchange folder of "Portar para Unreal Engine 5": ``manifest.json`` (version 1), the terrain
as a 16-bit Landscape heightmap (``terrain.r16``) and each building model once as ``.glb``.
Buildings become mesh actors; scene models, effects, sounds and spawn areas become markers. Every
actor carries its tag ``gf:<kind>:<map>:<id>`` and the game file it comes from (``source``).
"""

__all__ = ("KINDS", "tagged_objects", "write_manifest")

import json
import os
import re

from . import coords, terrain

KINDS = {'BUILDING': "building", 'MODEL': "model", 'EFFECT': "effect", 'SOUND': "sound"}


def _scene_collections(map_code, client_root=None):
    from .. import scene
    world_file = "maps\\{0}\\{0}.ecwld".format(map_code).lower()
    return [c for c in scene.scene_collections() if c.get(scene.PROP_FILE, "").lower() == world_file
            and client_root in (None, c.get(scene.PROP_CLIENT))]


def _spawn_collections(map_code):
    from .. import spawns
    return [c for c in spawns.spawn_collections()
            if c.get(spawns.PROP_FILE, "").lower().startswith(map_code.lower() + "\\")]


def tagged_objects(map_code, client_root=None):
    """``{tag: (kind, object, source)}`` of the scene objects and spawn areas of a map open in the
    Studio (spawn areas are numbered by their index in ``npcgen.data``)."""
    from .. import scene, spawns
    found = {}
    for collection in _scene_collections(map_code, client_root):
        for ob in collection.all_objects:
            kind = KINDS.get(ob.get(scene.PROP_KIND))
            if kind is None:
                continue
            source = ob.get(scene.PROP_PATH, "")
            if kind == "building" and ob.instance_collection is not None:
                source = ob.instance_collection["gameforge_model"]
            found["gf:{:s}:{:s}:{:d}".format(kind, map_code, ob.get(scene.PROP_ID, 0))] = (kind, ob, source)
    for collection in _spawn_collections(map_code):
        for ob in collection.objects:
            if ob.gameforge_spawn.kind == 'AREA' and ob.gameforge_spawn.index >= 0:
                tag = "gf:spawn:{:s}:{:d}".format(map_code, ob.gameforge_spawn.index)
                found[tag] = ("spawn", ob, collection[spawns.PROP_FILE])
    return found


def _file_name(model):
    # ponytail: two model paths differing only in punctuation would share a file; not seen in clients.
    return re.sub(r"[^A-Za-z0-9_]+", "_", os.path.splitext(model)[0]).strip("_")


def write_manifest(folder, client_root, map_code, center=None, radius=0.0):
    """Write the exchange folder of the map open in the Studio; returns the manifest's path.
    ``center`` (engine x, z) and ``radius`` limit the terrain to a square around a place."""
    import bpy
    from .. import scene
    for collection in _scene_collections(map_code, client_root) + _spawn_collections(map_code):
        if float(collection.get(scene.PROP_SCALE, 1.0)) != 1.0:
            raise ValueError("abra o mapa com escala 1 para portar para a Unreal ({:s})".format(collection.name))
    os.makedirs(os.path.join(folder, "assets"), exist_ok=True)
    land = terrain.landscape(os.path.join(client_root, "maps", map_code), map_code, center=center, radius=radius)
    with open(os.path.join(folder, "terrain.r16"), "wb") as fh:
        fh.write(land.pop("raw16"))
    manifest = {"format": "gameforge-link", "version": 1, "client": client_root, "map": map_code,
                "terrain": dict(file="terrain.r16", **land), "assets": [], "actors": []}
    exported = {}
    for tag, (kind, ob, source) in sorted(tagged_objects(map_code, client_root).items()):
        actor = {"tag": tag, "kind": kind, "label": ob.name, "source": source}
        actor.update(coords.blender_to_unreal(ob.matrix_world))
        if kind == "building" and ob.instance_collection is not None:
            if source not in exported:
                exported[source] = "assets/{:s}.glb".format(_file_name(source))
                # Exports the model collection even with the model library excluded from the view layer.
                bpy.ops.export_scene.gltf(filepath=os.path.join(folder, exported[source]), export_format='GLB',
                                          collection=ob.instance_collection.name, export_apply=True,
                                          export_animations=False)
                manifest["assets"].append({"id": source, "file": exported[source]})
            actor["asset"] = source
        manifest["actors"].append(actor)
    path = os.path.join(folder, "manifest.json")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, ensure_ascii=False, indent=1)
    return path
```

- [ ] **Step 4: gerador dos dados de teste da Unreal.** `tests/python/gameforge_unreal_test_data.py`:

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Writes the data of the GameForgeLink tests (they run inside Unreal) into the folder given after
``--``: the exchange folder of GAMEFORGE_TEST_MAP (default a08) from the first GAMEFORGE_TEST_CLIENTS
client with one building, and ``probe.glb``: one triangle with corners at Blender (0, 0, 0),
(1, 0, 0) and (0, 2, 3), which pins the axis conversion of Unreal's glb importer.
"""

import os
import sys

import bpy

from gameforge_angelica import scene
from gameforge_angelica.unreal import export

folder = sys.argv[sys.argv.index("--") + 1]
client = [p for p in os.environ["GAMEFORGE_TEST_CLIENTS"].split(";") if p.strip()][0]
map_code = os.environ.get("GAMEFORGE_TEST_MAP", "a08")
scene.load_map_scene(client, map_code, building_limit=1)
export.write_manifest(folder, client, map_code)

mesh = bpy.data.meshes.new("probe")
mesh.from_pydata([(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 2.0, 3.0)], [], [(0, 1, 2)])
probe = bpy.data.collections.new("probe")
bpy.context.scene.collection.children.link(probe)  # The glTF exporter only sees collections of the scene.
probe.objects.link(bpy.data.objects.new("probe", mesh))
bpy.ops.export_scene.gltf(filepath=os.path.join(folder, "probe.glb"), export_format='GLB', collection=probe.name)
print("GAMEFORGE_TEST_DATA", folder)
```

- [ ] **Step 5:** **rodar no Studio** `gameforge_unreal_export_test`. Expected: `Ran 2 tests ... OK`. Depois gerar os dados e conferir:

```powershell
& "D:\Atom Sec Labs\ATOM GameForge\gf-build\bin\GameForge3DStudio.exe" -b --factory-startup --python "D:\Atom Sec Labs\ATOM GameForge\GameForge-3D-Studio\tests\python\gameforge_unreal_test_data.py" -- "D:\claude-scratch\gf-ue-test\TestData"
Get-ChildItem -Recurse "D:\claude-scratch\gf-ue-test\TestData" | Select-Object Name, Length
```

Expected: `manifest.json`, `terrain.r16`, `probe.glb` e um `assets\*.glb`.

- [ ] **Step 6: checkpoint.** `git status --short` mostra `export.py` e os dois arquivos de teste.

---

### Task 6: Lado Unreal — importar a pasta de troca (Landscape em C++, malhas, atores)

**Files:**
- Create: `...\GameForgeLink\Source\GameForgeLinkEditor\Public\GameForgeLandscapeLibrary.h`
- Create: `...\GameForgeLink\Source\GameForgeLinkEditor\Private\GameForgeLandscapeLibrary.cpp`
- Create: `...\GameForgeLink\Content\Python\gameforge_link\importer.py`
- Modify: `...\GameForgeLink\Content\Python\gameforge_link\__init__.py` (expõe `import_manifest`)
- Test: `...\GameForgeLink\Tests\test_import.py` (lê os dados de `GAMEFORGE_LINK_TEST_DATA`, gerados pela Task 5)

**Interfaces:**
- Consumes: manifesto versão 1 e `probe.glb` (Task 5).
- Produces: `UGameForgeLandscapeLibrary::ImportHeightmap(RawPath, SizeX, SizeY, ComponentQuads, Location, Scale, Label) -> ALandscape*` (no Python: `unreal.GameForgeLandscapeLibrary.import_heightmap(...)`); `gameforge_link.importer.open_level(map_code)` (abre ou cria `/Game/GameForge/Maps/<mapa>`); `gameforge_link.importer.mesh_of(glb_path, folder) -> unreal.StaticMesh` (importa uma vez por pasta; erro se o arquivo virar mais de uma malha); `gameforge_link.import_manifest(path) -> dict` (`{"landscape": bool, "assets": novos, "actors": novos, "updated": n}`), que salva o nível no fim.

- [ ] **Step 1: o teste na UE.** `Tests\test_import.py`:

```python
# SPDX-License-Identifier: MIT
"""Run by Tools/RunTest.ps1 inside Unreal, on the Studio test data (GAMEFORGE_LINK_TEST_DATA)."""
import json
import os

import unreal
import gameforge_link
from gameforge_link import importer

DATA = os.environ["GAMEFORGE_LINK_TEST_DATA"]


def close(a, b, tolerance=0.5):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


try:
    for leftover in ("/Game/GameForge", "/Game/GameForgeTest"):  # Saved by the previous run.
        if unreal.EditorAssetLibrary.does_directory_exist(leftover):
            unreal.EditorAssetLibrary.delete_directory(leftover)
    # Axis conversion of the glb importer: Blender (x, y, z) -> Unreal 100 * (x, -y, z).
    probe = importer.mesh_of(os.path.join(DATA, "probe.glb"), "/Game/GameForgeTest/Probe")
    box = probe.get_bounding_box()
    assert close(box.min.to_tuple(), (0.0, -200.0, 0.0)) and close(box.max.to_tuple(), (100.0, 0.0, 300.0)), box

    path = os.path.join(DATA, "manifest.json")
    with open(path, encoding="utf-8") as fh:
        manifest = json.load(fh)
    first = gameforge_link.import_manifest(path)
    assert first["landscape"] and first["assets"] >= 1 and first["actors"] == len(manifest["actors"]), first
    landscape = next(a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
                     if isinstance(a, unreal.Landscape))
    assert close(landscape.get_actor_location().to_tuple(), manifest["terrain"]["location_cm"], 1.0)
    again = gameforge_link.import_manifest(path)
    assert again == {"landscape": False, "assets": 0, "actors": 0, "updated": first["actors"]}, again
    print("GAMEFORGE_TEST OK", first, again)
finally:
    unreal.SystemLibrary.quit_editor()
```

- [ ] **Step 2:** compilar o plugin, recriar o projeto e rodar (ainda sem `importer`):

```powershell
& "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink\Tools\Build.ps1"
& "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink\Tools\MakeProject.ps1" -Path "D:\claude-scratch\gf-ue-test"
$env:GAMEFORGE_LINK_TEST_DATA = "D:\claude-scratch\gf-ue-test\TestData"
& "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink\Tools\RunTest.ps1" -Project "D:\claude-scratch\gf-ue-test\GameForge.uproject" -Script "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink\Tests\test_import.py"
```

Expected: código 1, `ImportError: cannot import name 'importer'` no log.

- [ ] **Step 3: a biblioteca C++ do Landscape** (como o botão "Create" da ferramenta de Landscape da 5.7).

`Public\GameForgeLandscapeLibrary.h`:

```cpp
// SPDX-License-Identifier: MIT
#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "GameForgeLandscapeLibrary.generated.h"

class ALandscape;

UCLASS()
class UGameForgeLandscapeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Create a landscape in the editor world from a 16-bit RAW heightmap (SizeX * SizeY values,
	 * little endian, row 0 first); one section per component of ComponentQuads quads. */
	UFUNCTION(BlueprintCallable, Category = "GameForge")
	static ALandscape* ImportHeightmap(const FString& RawPath, int32 SizeX, int32 SizeY, int32 ComponentQuads,
	                                   FVector Location, FVector Scale, const FString& Label);
};
```

`Private\GameForgeLandscapeLibrary.cpp`:

```cpp
// SPDX-License-Identifier: MIT
#include "GameForgeLandscapeLibrary.h"

#include "Editor.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "Misc/FileHelper.h"

ALandscape* UGameForgeLandscapeLibrary::ImportHeightmap(const FString& RawPath, int32 SizeX, int32 SizeY,
                                                         int32 ComponentQuads, FVector Location, FVector Scale,
                                                         const FString& Label)
{
	TArray<uint8> Bytes;
	if (ComponentQuads <= 0 || (SizeX - 1) % ComponentQuads != 0 || (SizeY - 1) % ComponentQuads != 0 ||
	    !FFileHelper::LoadFileToArray(Bytes, *RawPath) || Bytes.Num() != SizeX * SizeY * 2)
	{
		UE_LOG(LogTemp, Error, TEXT("GameForgeLink: invalid heightmap %s (%d x %d, %d quads)"), *RawPath, SizeX, SizeY,
		       ComponentQuads);
		return nullptr;
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return nullptr;
	}
	TArray<uint16> Heights;
	Heights.SetNumUninitialized(SizeX * SizeY);
	FMemory::Memcpy(Heights.GetData(), Bytes.GetData(), Bytes.Num());

	// As FLandscapeEditorDetailCustomization_NewLandscape::OnCreateButtonClicked (UE 5.7).
	ALandscape* Landscape = World->SpawnActor<ALandscape>(Location, FRotator::ZeroRotator);
	Landscape->SetActorRelativeScale3D(Scale);
	TMap<FGuid, TArray<uint16>> HeightDataPerLayers;
	HeightDataPerLayers.Add(FGuid(), MoveTemp(Heights));
	TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerDataPerLayers;
	MaterialLayerDataPerLayers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());
	Landscape->Import(FGuid::NewGuid(), 0, 0, SizeX - 1, SizeY - 1, 1, ComponentQuads, HeightDataPerLayers, TEXT(""),
	                  MaterialLayerDataPerLayers, ELandscapeImportAlphamapType::Additive,
	                  TArrayView<const FLandscapeLayer>());
	Landscape->SetActorLabel(Label);
	if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
	{
		Info->UpdateLayerInfoMap(Landscape);
	}
	return Landscape;
}
```

- [ ] **Step 4: o importador.** `Content\Python\gameforge_link\importer.py`:

```python
# SPDX-License-Identifier: MIT
"""Import a GameForge 3D Studio exchange folder (manifest version 1) into the level of its map."""

import json
import os

import unreal

ROOT = "/Game/GameForge"


def open_level(map_code):
    """Open (or create) the level of a map, so that every port of it updates the same actors."""
    path = "{:s}/Maps/{:s}".format(ROOT, map_code)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if world.get_path_name().split(".")[0] == path:
        return
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        levels.load_level(path)
    else:
        levels.new_level(path)


def tagged_actors():
    """``{gf tag: actor}`` of the open level."""
    found = {}
    for actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
        for tag in actor.tags:
            if str(tag).startswith("gf:"):
                found[str(tag)] = actor
    return found


def mesh_of(path, folder):
    """The static mesh of a .glb, imported once into its own folder (a file must become one mesh)."""
    meshes = [m for m in (unreal.load_asset(p) for p in unreal.EditorAssetLibrary.list_assets(folder, recursive=False))
              if isinstance(m, unreal.StaticMesh)]
    if not meshes:
        task = unreal.AssetImportTask()
        task.filename = path
        task.destination_path = folder
        task.automated = True
        task.replace_existing = True
        task.save = True
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        meshes = [m for m in (unreal.load_asset(p) for p in task.imported_object_paths)
                  if isinstance(m, unreal.StaticMesh)]
    if len(meshes) != 1:
        raise RuntimeError("{:s}: {:d} malhas na Unreal, esperava 1".format(path, len(meshes)))
    return meshes[0]


def _place(actor, data):
    rotation = unreal.MathLibrary.make_rot_from_xz(unreal.Vector(*data["x_axis"]), unreal.Vector(*data["z_axis"]))
    actor.set_actor_location_and_rotation(unreal.Vector(*data["location_cm"]), rotation, False, False)
    actor.set_actor_scale3d(unreal.Vector(*data["scale"]))


def import_manifest(path):
    with open(path, encoding="utf-8") as fh:
        manifest = json.load(fh)
    if manifest.get("format") != "gameforge-link" or manifest.get("version") != 1:
        raise ValueError("not a GameForge exchange manifest (version 1): " + path)
    folder = os.path.dirname(path)
    map_code = manifest["map"]
    open_level(map_code)
    existing = tagged_actors()
    result = {"landscape": False, "assets": 0, "actors": 0, "updated": 0}

    terrain = manifest.get("terrain")
    terrain_tag = "gf:terrain:" + map_code
    if terrain and terrain_tag not in existing:
        landscape = unreal.GameForgeLandscapeLibrary.import_heightmap(
            os.path.join(folder, terrain["file"]), terrain["size_x"], terrain["size_y"], terrain["component_quads"],
            unreal.Vector(*terrain["location_cm"]), unreal.Vector(*terrain["scale"]), "Terreno " + map_code)
        if landscape is None:
            raise RuntimeError("o Landscape de {:s} não foi criado (veja o Output Log)".format(map_code))
        landscape.set_editor_property("tags", [unreal.Name(terrain_tag)])
        result["landscape"] = True

    meshes = {}
    for asset in manifest.get("assets", []):
        name = os.path.splitext(os.path.basename(asset["file"]))[0]
        destination = "{:s}/{:s}/Models/{:s}".format(ROOT, map_code, name)
        is_new = not unreal.EditorAssetLibrary.list_assets(destination, recursive=False)
        meshes[asset["id"]] = mesh_of(os.path.join(folder, asset["file"]), destination)
        result["assets"] += int(is_new)

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for data in manifest.get("actors", []):
        actor = existing.get(data["tag"])
        if actor is not None:
            _place(actor, data)
            result["updated"] += 1
            continue
        location = unreal.Vector(*data["location_cm"])
        mesh = meshes.get(data.get("asset"))
        if mesh is not None:
            actor = actors.spawn_actor_from_object(mesh, location)
        else:
            actor = actors.spawn_actor_from_class(unreal.Note, location)
        actor.set_actor_label(data.get("label") or data["tag"])
        actor.set_editor_property("tags", [unreal.Name(data["tag"]), unreal.Name("gf.source:" + data["source"])])
        actor.set_folder_path(unreal.Name("GameForge/{:s}/{:s}".format(map_code, data["kind"])))
        _place(actor, data)
        result["actors"] += 1
    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()
    return result
```

Em `gameforge_link/__init__.py`, depois de `ping`:

```python
from .importer import import_manifest  # noqa: E402,F401
```

- [ ] **Step 5:** compilar o plugin, recriar o projeto e rodar (os mesmos comandos da Step 2). Expected: `GAMEFORGE_TEST OK {'landscape': True, ...} {'landscape': False, 'assets': 0, 'actors': 0, 'updated': N}` e código 0. Se falhar:
  - a sonda chega com cantos trocados: corrigir no Studio (opção do exportador glTF ou uma rotação aplicada à malha antes de exportar em `export.write_manifest`), nunca em `_place`, e regenerar os dados (Task 5, Step 5);
  - `mesh_of` reclama de várias malhas: combinar as malhas na importação (opção "combine static meshes" do pipeline Interchange passado à importação) e manter o erro para qualquer outro caso;
  - o Landscape não nasce com `-nullrhi` (as camadas de edição da 5.7 usam a GPU): rodar com `RunTest.ps1 -Offscreen` (renderiza sem janela).

- [ ] **Step 6: checkpoint.** `git -C "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink" status --short` mostra os arquivos desta tarefa.

---

### Task 7: "Portar para Unreal Engine 5" (projeto, operador em thread e interface)

**Files:**
- Create: `scripts/modules/gameforge_angelica/unreal/project.py`
- Create: `scripts/startup/bl_operators/gameforge_unreal.py`
- Modify: `scripts/startup/bl_operators/__init__.py` (`"gameforge_unreal",` na lista `_modules`)
- Modify: `scripts/startup/bl_ui/space_view3d_gameforge.py` (painel `VIEW3D_PT_gameforge_unreal`)
- Modify: `scripts/startup/bl_ui/space_topbar.py` (Arquivo › "Portar para Unreal Engine 5...")
- Modify (fora do repo): `D:\claude-scratch\gf-tools\ui_check.py`
- Test: `tests/python/gameforge_unreal_port_test.py`

**Interfaces:**
- Consumes: `link.UnrealLink`, `link.LinkError`, `export.write_manifest`, `gameforge_unreal_fake.FakeUnreal`.
- Produces: `project.engine_dir() -> str`; `project.ensure_project(folder, plugin_dir) -> str` (cria `GameForge.uproject` + `Config\DefaultEngine.ini` com Remote Execution e copia o plugin, se o projeto ainda não existe); `project.launch(engine, uproject)` (`LinkError` se a UE não existe); `project.port(manifest, project_dir, plugin_dir, wait=120.0) -> dict` (sem `bpy`: acha a Unreal ou abre o projeto, espera, importa; `LinkError` em português). `bl_operators.gameforge_unreal.open_map() -> (client, map_code) | None`. Operador `gameforge.unreal_port` (propriedades `project_dir`, `around_cursor`, `radius`): exporta na hora e faz o resto numa thread, terminando por um timer modal, como `gameforge_character._AsyncOperator`.

- [ ] **Step 1: o teste.**

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The port to Unreal against a fake Unreal node (no Unreal is opened)."""

import ast
import os
import re
import sys
import tempfile
import time
import unittest

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gameforge_unreal_fake import FakeUnreal  # noqa: E402
from gameforge_angelica import scene  # noqa: E402
from gameforge_angelica.unreal import link, project  # noqa: E402

CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]
MAP = os.environ.get("GAMEFORGE_TEST_MAP", "a08")


def _no_launch(*_args):
    raise AssertionError("the test must not open Unreal")


class PortTest(unittest.TestCase):

    def setUp(self):
        self.launch = project.launch

    def tearDown(self):
        project.launch = self.launch

    @unittest.skipUnless(CLIENTS, "set GAMEFORGE_TEST_CLIENTS")
    def test_operator_sends_the_manifest(self):
        project.launch = _no_launch
        fake = FakeUnreal({"import_manifest": {"landscape": True, "assets": 1, "actors": 3, "updated": 0}})
        fake.start()
        try:
            scene.load_map_scene(CLIENTS[0], MAP, building_limit=1)
            self.assertEqual(bpy.ops.gameforge.unreal_port(project_dir=tempfile.mkdtemp()), {'RUNNING_MODAL'})
            end = time.monotonic() + 30.0
            while not fake.commands and time.monotonic() < end:
                time.sleep(0.2)
            self.assertTrue(fake.commands, "Unreal got no command")
            path = ast.literal_eval(re.search(r"import_manifest\(path=('.*?')\)", fake.commands[0]).group(1))
            self.assertTrue(os.path.isfile(path), path)
        finally:
            fake.stop.set()

    def test_no_answer(self):
        launched = []
        project.launch = lambda engine, uproject: launched.append(uproject)
        folder = tempfile.mkdtemp()
        with self.assertRaisesRegex(link.LinkError, "não respondeu"):
            project.port("manifest.json", folder, "", wait=1.0)
        self.assertEqual(launched, [os.path.join(folder, "GameForge.uproject")])
        with open(os.path.join(folder, "Config", "DefaultEngine.ini"), encoding="utf-8") as fh:
            self.assertIn("bRemoteExecution=True", fh.read())


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
```

- [ ] **Step 2:** **rodar no Studio**. Expected: FAIL (`ImportError: cannot import name 'project'`).

- [ ] **Step 3: `project.py`.**

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The Unreal Engine install, the GameForge project the Studio ports maps into, and the port itself
once the exchange folder is written. No ``bpy`` here: ``port`` runs on a thread.
"""

__all__ = ("engine_dir", "ensure_project", "launch", "port")

import json
import os
import shutil
import subprocess

from . import link

DEFAULT_ENGINE = r"D:\Atom Sec Labs\ATOM GameForge\UE_5.7"
_PLUGINS = ("PythonScriptPlugin", "EditorScriptingUtilities", "Interchange", "GameForgeLink")
_INI = """[/Script/PythonScriptPlugin.PythonScriptPluginSettings]
bRemoteExecution=True
RemoteExecutionMulticastGroupEndpoint=239.0.0.1:6766
RemoteExecutionMulticastBindAddress=127.0.0.1
RemoteExecutionMulticastTtl=0
"""


def engine_dir():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\EpicGames\Unreal Engine\5.7") as key:
            folder = winreg.QueryValueEx(key, "InstalledDirectory")[0]
        if os.path.isfile(os.path.join(folder, "Engine", "Binaries", "Win64", "UnrealEditor.exe")):
            return folder
    except OSError:
        pass
    return DEFAULT_ENGINE


def ensure_project(folder, plugin_dir):
    """The GameForge project in ``folder``, created (with the plugin copied in) when missing."""
    uproject = os.path.join(folder, "GameForge.uproject")
    if os.path.isfile(uproject):
        return uproject
    os.makedirs(os.path.join(folder, "Config"), exist_ok=True)
    with open(uproject, "w", encoding="utf-8") as fh:
        json.dump({"FileVersion": 3, "EngineAssociation": "5.7", "Description": "GameForge",
                   "Plugins": [{"Name": name, "Enabled": True} for name in _PLUGINS]}, fh, indent=2)
    with open(os.path.join(folder, "Config", "DefaultEngine.ini"), "w", encoding="utf-8") as fh:
        fh.write(_INI)
    if plugin_dir and os.path.isdir(plugin_dir):
        shutil.copytree(plugin_dir, os.path.join(folder, "Plugins", "GameForgeLink"), dirs_exist_ok=True)
    return uproject


def launch(engine, uproject):
    exe = os.path.join(engine, "Engine", "Binaries", "Win64", "UnrealEditor.exe")
    if not os.path.isfile(exe):
        raise link.LinkError("Unreal Engine 5.7 não encontrada em {:s}".format(engine))
    subprocess.Popen([exe, uproject])


def port(manifest, project_dir, plugin_dir, wait=120.0):
    """Import ``manifest`` in the running Unreal, opening the GameForge project first when no Unreal
    answers. Returns the importer's counts; raises link.LinkError."""
    unreal = link.UnrealLink()
    try:
        node = unreal.find_node(wait=2.0)
        if node is None:
            launch(engine_dir(), ensure_project(project_dir, plugin_dir))
            node = unreal.find_node(wait=wait)
        if node is None:
            raise link.LinkError("A Unreal não respondeu em {:.0f} s: confira se o projeto abriu com o plugin "
                                 "GameForgeLink e o Remote Execution do Python ligado".format(wait))
        unreal.connect(node["node_id"])
        return unreal.call("import_manifest", path=manifest)
    finally:
        unreal.close()
```

- [ ] **Step 4: o operador.** `scripts/startup/bl_operators/gameforge_unreal.py`:

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Portar para Unreal Engine 5: write the exchange folder of the map open in the Studio, then (on a
thread, the Studio stays usable while Unreal opens) import it in Unreal through GameForgeLink.
"""

__all__ = ("GAMEFORGE_OT_unreal_port", "open_map")

import os
import tempfile
import threading

import bpy
from bpy.props import BoolProperty, FloatProperty, StringProperty
from bpy.types import Operator

from gameforge_angelica import scene

# ponytail: the plugin build of this machine; the suite installer (Frente 7b) will ship the plugin.
PLUGIN_DIR = r"D:\Atom Sec Labs\ATOM GameForge\GameForgeLink-Build"


def open_map():
    """``(client, map code)`` of the last map scene opened in the Studio, or None."""
    collections = scene.scene_collections()
    if not collections:
        return None
    collection = collections[-1]
    return collection[scene.PROP_CLIENT], collection[scene.PROP_FILE].split("\\")[1]


class GAMEFORGE_OT_unreal_port(Operator):
    """Levar o mapa aberto (terreno, prédios, objetos de cena e spawns) para um projeto da Unreal Engine 5"""
    bl_idname = "gameforge.unreal_port"
    bl_label = "Portar para Unreal Engine 5"

    project_dir: StringProperty(
        name="Projeto da Unreal",
        description="Pasta do projeto da Unreal que recebe os mapas (criado na primeira vez)",
        subtype='DIR_PATH',
        default=os.path.join(os.path.expanduser("~"), "Documents", "GameForge Unreal"),
    )
    around_cursor: BoolProperty(
        name="Só em volta do cursor 3D",
        description="Portar só o terreno num quadrado em volta do cursor 3D (obrigatório em mapas grandes)",
        default=False,
    )
    radius: FloatProperty(name="Distância", default=500.0, min=50.0, subtype='DISTANCE')

    _timer = None
    _thread = None
    _result = None
    _error = None

    @classmethod
    def poll(cls, _context):
        return open_map() is not None

    def invoke(self, context, _event):
        return context.window_manager.invoke_props_dialog(self, width=460, confirm_text="Portar")

    def execute(self, context):
        from gameforge_angelica.unreal import export, project
        client, map_code = open_map()
        center = tuple(context.scene.cursor.location.xy) if self.around_cursor else None
        try:
            manifest = export.write_manifest(tempfile.mkdtemp(prefix="gameforge-unreal-"), client, map_code,
                                             center=center, radius=self.radius if self.around_cursor else 0.0)
        except (OSError, RuntimeError, ValueError) as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        project_dir = bpy.path.abspath(self.project_dir)

        def run():
            try:
                self._result = project.port(manifest, project_dir, PLUGIN_DIR)
            except Exception as ex:  # The thread's end: every error must reach the user.
                self._error = str(ex) or repr(ex)

        # Same pattern as gameforge_character._AsyncOperator.
        self._thread = threading.Thread(target=run, name="gameforge-unreal", daemon=True)
        self._thread.start()
        self._timer = context.window_manager.event_timer_add(0.5, window=context.window)
        context.window_manager.modal_handler_add(self)
        self.report({'INFO'}, "Enviando {:s} para a Unreal...".format(map_code))
        return {'RUNNING_MODAL'}

    def modal(self, context, event):
        if event.type != 'TIMER' or self._thread.is_alive():
            return {'PASS_THROUGH'}
        context.window_manager.event_timer_remove(self._timer)
        if self._error:
            self.report({'ERROR'}, self._error)
            return {'CANCELLED'}
        self.report({'INFO'}, "Unreal: {actors:d} atores novos, {updated:d} atualizados, {assets:d} modelos novos".format(
            **self._result))
        return {'FINISHED'}


classes = (
    GAMEFORGE_OT_unreal_port,
)
```

Em `scripts/startup/bl_operators/__init__.py`, na lista `_modules`, depois de `"gameforge_tasks",`: `"gameforge_unreal",` (o arquivo usa CRLF; manter).

- [ ] **Step 5: interface.** Em `space_view3d_gameforge.py`, antes de `classes = (`:

```python
class VIEW3D_PT_gameforge_unreal(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "GameForge"
    bl_label = "Unreal Engine 5"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, _context):
        layout = self.layout
        layout.operator("gameforge.unreal_port", text="Portar para Unreal Engine 5...", icon='EXPORT')
```

e `VIEW3D_PT_gameforge_unreal,` no fim da tupla `classes`. Em `space_topbar.py`, depois da linha `layout.operator("gameforge.client_save_undo", text="Desfazer Último Salvamento no Cliente")`:

```python
        layout.operator("gameforge.unreal_port", text="Portar para Unreal Engine 5...", icon='EXPORT')
```

Em `D:\claude-scratch\gf-tools\ui_check.py`, antes da linha final `print("UICHECK problems:", ...)`:

```python
run_panel("VIEW3D_PT_gameforge_unreal")
```

- [ ] **Step 6:** **rodar no Studio** `gameforge_unreal_port_test` e o verificador de painéis:

```powershell
& "D:\Atom Sec Labs\ATOM GameForge\gf-build\bin\GameForge3DStudio.exe" -b --factory-startup --python "D:\claude-scratch\gf-tools\ui_check.py" -- "D:\Atom Sec Labs\ATOM GameForge\GameForge_PWServer\1.7.8\2 - cliente178\element"
```

Expected: `Ran 2 tests ... OK`; `UICHECK VIEW3D_PT_gameforge_unreal drawn` e `UICHECK problems: none`.

- [ ] **Step 7: checkpoint.** `git status --short` mostra `project.py`, `gameforge_unreal.py`, `bl_operators/__init__.py`, os dois arquivos de `bl_ui` e o teste.

---

### Task 8: Volta — posições editadas na Unreal viram alterações no Studio

**Files:**
- Modify: `...\GameForgeLink\Content\Python\gameforge_link\importer.py` (`read_actors`) e `__init__.py` (exporta)
- Create: `scripts/modules/gameforge_angelica/unreal/sync.py`
- Modify: `scripts/startup/bl_operators/gameforge_unreal.py` (`GameForgeUnrealChange`, `gameforge.unreal_fetch`, `gameforge.unreal_apply`, `register`/`unregister`)
- Modify: `scripts/startup/bl_ui/space_view3d_gameforge.py` (`GAMEFORGE_UL_unreal_changes`, painel com os botões e a lista)
- Modify (fora do repo): `D:\claude-scratch\gf-tools\ui_check.py`
- Test: `tests/python/gameforge_unreal_sync_test.py`; `...\GameForgeLink\Tests\test_read.py`

**Interfaces:**
- Consumes: `export.tagged_objects`, `coords.blender_to_unreal`/`unreal_to_blender`, `link.UnrealLink`, `open_map()`.
- Produces (Unreal): `gameforge_link.read_actors(map_code) -> [{tag, location_cm, x_axis, z_axis, scale}]` (abre o nível do mapa antes). Produces (Studio): `sync.differences(map_code, actors) -> [{tag, object, state, matrix}]` (`state` em `'MOVED'`, `'MISSING_IN_UNREAL'`, `'NEW_IN_UNREAL'`; `object` = nome no Studio ou `""`; `matrix` do Blender ou `None`); `sync.apply(differences) -> int` (move objetos sem trava de posição; nunca apaga). `WindowManager.gameforge_unreal_changes` (`tag`, `object`, `state`) e `gameforge_unreal_change_index`.

- [ ] **Step 1: o teste do Studio** (sem Unreal).

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Unreal -> Studio differences and their application (no Unreal needed). Clients are only read."""

import os
import sys
import unittest

from gameforge_angelica import scene
from gameforge_angelica.unreal import coords, export, sync

CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]
MAP = os.environ.get("GAMEFORGE_TEST_MAP", "a08")


@unittest.skipUnless(CLIENTS, "set GAMEFORGE_TEST_CLIENTS")
class SyncTest(unittest.TestCase):

    def test_moved_missing_new_and_apply(self):
        collection, _problems = scene.load_map_scene(CLIENTS[0], MAP, building_limit=0)
        obs = sorted((o for o in collection.all_objects if o.get(scene.PROP_KIND) in scene.EDITABLE),
                     key=lambda o: o.name)
        self.assertGreaterEqual(len(obs), 3)
        tag_of = {ob.name: tag for tag, (_kind, ob, _source) in export.tagged_objects(MAP).items()}
        moved, gone = obs[0], obs[1]
        actors = []
        for ob in obs:
            if ob is gone:
                continue  # Deleted in Unreal.
            matrix = ob.matrix_world.copy()
            if ob is moved:
                matrix.translation.x += 2.0
            actors.append(dict(tag=tag_of[ob.name], **coords.blender_to_unreal(matrix)))
        new_tag = "gf:effect:{:s}:999999".format(MAP)
        actors.append(dict(tag=new_tag, **coords.blender_to_unreal(moved.matrix_world)))

        found = {d["tag"]: d for d in sync.differences(MAP, actors)}
        self.assertEqual(found[tag_of[moved.name]]["state"], 'MOVED')
        self.assertEqual(found[tag_of[gone.name]]["state"], 'MISSING_IN_UNREAL')
        self.assertEqual(found[new_tag]["state"], 'NEW_IN_UNREAL')
        self.assertEqual(sum(1 for d in found.values() if d["state"] == 'MOVED'), 1)

        target = moved.matrix_world.translation.x + 2.0
        self.assertEqual(sync.apply(list(found.values())), 1)
        self.assertAlmostEqual(moved.matrix_world.translation.x, target, places=4)
        self.assertIn(gone.name, collection.all_objects)  # Nothing is deleted.
        self.assertTrue(scene.scene_changed(collection))


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
```

- [ ] **Step 2:** **rodar no Studio**. Expected: FAIL (`ImportError: cannot import name 'sync'`).

- [ ] **Step 3: `sync.py`.**

```python
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
What changed in Unreal for a map: the actors tagged ``gf:<kind>:<map>:<id>`` (read by
``gameforge_link.read_actors``) compared with the Studio objects of the same tags. Applying moves
the Studio objects only; the game files are written by the usual "Salvar no cliente do jogo".
Nothing is deleted from here.
"""

__all__ = ("apply", "differences")

from . import coords, export

_TOLERANCE_CM = 0.5
_TOLERANCE_AXIS = 1e-3


def differences(map_code, actors):
    objects = export.tagged_objects(map_code)
    result = []
    for actor in actors:
        found = objects.get(actor["tag"])
        matrix = coords.unreal_to_blender(actor["location_cm"], actor["x_axis"], actor["z_axis"], actor["scale"])
        if found is None:
            result.append({"tag": actor["tag"], "object": "", "state": 'NEW_IN_UNREAL', "matrix": matrix})
            continue
        here = coords.blender_to_unreal(found[1].matrix_world)
        moved = any(abs(a - b) > _TOLERANCE_CM for a, b in zip(here["location_cm"], actor["location_cm"]))
        turned = any(abs(a - b) > _TOLERANCE_AXIS for a, b in zip(tuple(here["x_axis"]) + tuple(here["z_axis"]),
                                                                  tuple(actor["x_axis"]) + tuple(actor["z_axis"])))
        if moved or turned:
            result.append({"tag": actor["tag"], "object": found[1].name, "state": 'MOVED', "matrix": matrix})
    seen = {actor["tag"] for actor in actors}
    for tag, (_kind, ob, _source) in objects.items():
        if tag not in seen:
            result.append({"tag": tag, "object": ob.name, "state": 'MISSING_IN_UNREAL', "matrix": None})
    return result


def apply(found):
    """Move the Studio objects moved in Unreal; returns how many. Locked objects (buildings: their
    place lives in the .bmd and on the server) stay."""
    import bpy
    count = 0
    for difference in found:
        ob = bpy.data.objects.get(difference["object"]) if difference["state"] == 'MOVED' else None
        if ob is not None and not any(ob.lock_location):
            ob.matrix_world = difference["matrix"]
            count += 1
    return count
```

- [ ] **Step 4:** **rodar no Studio** `gameforge_unreal_sync_test`. Expected: `Ran 1 test ... OK`.

- [ ] **Step 5: `read_actors` na Unreal.** No fim de `importer.py`:

```python
def read_actors(map_code):
    """Placement of the GameForge actors of a map (its level is opened first), for the Studio."""
    open_level(map_code)
    kinds = ("gf:building:", "gf:model:", "gf:effect:", "gf:sound:", "gf:spawn:")
    result = []
    for actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
        tag = next((str(t) for t in actor.tags if str(t).startswith(kinds) and str(t).split(":")[2] == map_code), None)
        if tag is None:
            continue
        rotation = actor.get_actor_rotation()
        result.append({"tag": tag,
                       "location_cm": list(actor.get_actor_location().to_tuple()),
                       "x_axis": list(rotation.get_forward_vector().to_tuple()),
                       "z_axis": list(rotation.get_up_vector().to_tuple()),
                       "scale": list(actor.get_actor_scale3d().to_tuple())})
    return result
```

Em `__init__.py`, trocar a linha de import por `from .importer import import_manifest, read_actors  # noqa: E402,F401`.

`Tests\test_read.py`:

```python
# SPDX-License-Identifier: MIT
"""Run by Tools/RunTest.ps1 inside Unreal: read_actors returns what import_manifest placed, and moves."""
import json
import os

import unreal
import gameforge_link

DATA = os.environ["GAMEFORGE_LINK_TEST_DATA"]


def close(a, b, tolerance):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


try:
    if unreal.EditorAssetLibrary.does_directory_exist("/Game/GameForge"):  # Saved by the previous run.
        unreal.EditorAssetLibrary.delete_directory("/Game/GameForge")
    path = os.path.join(DATA, "manifest.json")
    with open(path, encoding="utf-8") as fh:
        manifest = json.load(fh)
    gameforge_link.import_manifest(path)
    before = {a["tag"]: a for a in gameforge_link.read_actors(manifest["map"])}
    sent_tags = {a["tag"] for a in manifest["actors"]}
    assert set(before) == sent_tags, sorted(set(before) ^ sent_tags)
    for sent in manifest["actors"]:
        got = before[sent["tag"]]
        assert close(got["location_cm"], sent["location_cm"], 0.5), (sent, got)
        assert close(got["x_axis"] + got["z_axis"], sent["x_axis"] + sent["z_axis"], 1e-3), (sent, got)
    tag = manifest["actors"][0]["tag"]
    actor = next(a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
                 if tag in [str(t) for t in a.tags])
    actor.set_actor_location(actor.get_actor_location() + unreal.Vector(100.0, 0.0, 0.0), False, False)
    after = {a["tag"]: a for a in gameforge_link.read_actors(manifest["map"])}
    assert abs(after[tag]["location_cm"][0] - before[tag]["location_cm"][0] - 100.0) < 0.01, after[tag]
    print("GAMEFORGE_TEST OK", len(after))
finally:
    unreal.SystemLibrary.quit_editor()
```

Rodar com os dados da Task 5 e o plugin recompilado:

```powershell
& "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink\Tools\Build.ps1"
& "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink\Tools\MakeProject.ps1" -Path "D:\claude-scratch\gf-ue-test"
$env:GAMEFORGE_LINK_TEST_DATA = "D:\claude-scratch\gf-ue-test\TestData"
& "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink\Tools\RunTest.ps1" -Project "D:\claude-scratch\gf-ue-test\GameForge.uproject" -Script "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink\Tests\test_read.py"
```

Expected: `GAMEFORGE_TEST OK <n>` e código 0.

- [ ] **Step 6: operadores do Studio.** Em `gameforge_unreal.py`: no topo, trocar o import de `bpy.props` por `from bpy.props import BoolProperty, CollectionProperty, FloatProperty, IntProperty, StringProperty` e o de `bpy.types` por `from bpy.types import Operator, PropertyGroup`; acrescentar `"GameForgeUnrealChange", "GAMEFORGE_OT_unreal_fetch", "GAMEFORGE_OT_unreal_apply"` a `__all__`. Antes de `classes = (`:

```python
class GameForgeUnrealChange(PropertyGroup):
    tag: StringProperty()
    object: StringProperty()
    state: StringProperty()


_FOUND = []  # The last differences read from Unreal (with their matrices), as listed in the panel.


def _store(context, found):
    _FOUND[:] = found
    changes = context.window_manager.gameforge_unreal_changes
    changes.clear()
    for difference in found:
        item = changes.add()
        item.tag, item.object, item.state = difference["tag"], difference["object"], difference["state"]


class GAMEFORGE_OT_unreal_fetch(Operator):
    """Ler da Unreal as posições dos objetos deste mapa e listar o que mudou"""
    bl_idname = "gameforge.unreal_fetch"
    bl_label = "Trazer da Unreal"

    @classmethod
    def poll(cls, _context):
        return open_map() is not None

    def execute(self, context):
        from gameforge_angelica.unreal import link, sync
        _client, map_code = open_map()
        # ponytail: synchronous, a short wait when Unreal is closed; a thread if reading gets slow.
        unreal = link.UnrealLink()
        try:
            node = unreal.find_node(wait=2.0)
            if node is None:
                raise link.LinkError("A Unreal não está aberta com o projeto do GameForge")
            unreal.connect(node["node_id"])
            actors = unreal.call("read_actors", map_code=map_code)
        except link.LinkError as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        finally:
            unreal.close()
        found = sync.differences(map_code, actors)
        _store(context, found)
        self.report({'INFO'}, "{:d} objeto(s) movido(s) na Unreal".format(
            sum(1 for d in found if d["state"] == 'MOVED')))
        return {'FINISHED'}


class GAMEFORGE_OT_unreal_apply(Operator):
    """Mover no Studio os objetos movidos na Unreal (depois, salve no cliente do jogo)"""
    bl_idname = "gameforge.unreal_apply"
    bl_label = "Aplicar no Studio"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, _context):
        return any(d["state"] == 'MOVED' for d in _FOUND)

    def execute(self, context):
        from gameforge_angelica.unreal import sync
        count = sync.apply(_FOUND)
        _store(context, [d for d in _FOUND if d["state"] != 'MOVED'])
        self.report({'INFO'}, "{:d} objeto(s) movido(s) no Studio".format(count))
        return {'FINISHED'}
```

`classes` e o registro (as classes são registradas por `bl_operators` antes de `register()`):

```python
classes = (
    GameForgeUnrealChange,
    GAMEFORGE_OT_unreal_port,
    GAMEFORGE_OT_unreal_fetch,
    GAMEFORGE_OT_unreal_apply,
)


def register():
    # The classes are registered by `bl_operators` (from `classes`), before this runs.
    bpy.types.WindowManager.gameforge_unreal_changes = CollectionProperty(type=GameForgeUnrealChange)
    bpy.types.WindowManager.gameforge_unreal_change_index = IntProperty()


def unregister():
    del bpy.types.WindowManager.gameforge_unreal_changes
    del bpy.types.WindowManager.gameforge_unreal_change_index
```

- [ ] **Step 7: painel.** Em `space_view3d_gameforge.py`, antes de `VIEW3D_PT_gameforge_unreal`:

```python
class GAMEFORGE_UL_unreal_changes(UIList):
    STATES = {
        'MOVED': ("movido na Unreal", 'ORIENTATION_GLOBAL'),
        'MISSING_IN_UNREAL': ("não está mais na Unreal", 'ERROR'),
        'NEW_IN_UNREAL': ("novo na Unreal (não importado)", 'ADD'),
    }

    def draw_item(self, _context, layout, _data, item, _icon, _active_data, _active_propname, _index):
        text, icon = self.STATES.get(item.state, (item.state, 'NONE'))
        split = layout.split(factor=0.5, align=True)
        split.label(text=item.object or item.tag, icon=icon, translate=False)
        split.label(text=text, translate=False)

    def draw_filter(self, _context, _layout):
        pass
```

e trocar o `draw` de `VIEW3D_PT_gameforge_unreal` por:

```python
    def draw(self, context):
        layout = self.layout
        layout.operator("gameforge.unreal_port", text="Portar para Unreal Engine 5...", icon='EXPORT')
        row = layout.row(align=True)
        row.operator("gameforge.unreal_fetch", text="Trazer da Unreal", icon='IMPORT')
        row.operator("gameforge.unreal_apply", text="Aplicar no Studio", icon='CHECKMARK')
        wm = context.window_manager
        if len(wm.gameforge_unreal_changes):
            layout.template_list("GAMEFORGE_UL_unreal_changes", "", wm, "gameforge_unreal_changes", wm,
                                 "gameforge_unreal_change_index", rows=6)
            layout.operator("gameforge.client_save", text="Salvar no cliente do jogo...", icon='PACKAGE')
```

com `GAMEFORGE_UL_unreal_changes,` na tupla `classes`, antes de `VIEW3D_PT_gameforge_unreal,`. Em `ui_check.py`, trocar a linha `run_panel("VIEW3D_PT_gameforge_unreal")` da Task 7 por:

```python
change = bpy.context.window_manager.gameforge_unreal_changes.add()
change.tag, change.object, change.state = "gf:effect:a08:1", "Efeito", 'MOVED'
run_panel("VIEW3D_PT_gameforge_unreal")
```

- [ ] **Step 8:** **rodar no Studio** `gameforge_unreal_sync_test` e `gameforge_unreal_port_test` de novo e o `ui_check.py` (comando da Task 7, Step 6). Expected: OK nos testes; `UICHECK problems: none`.

- [ ] **Step 9: checkpoint.** `git status --short` (Studio) mostra `sync.py`, `gameforge_unreal.py`, `space_view3d_gameforge.py` e o teste; `git -C "D:\Atom Sec Labs\ATOM GameForge\GameForgeLink" status --short` mostra `importer.py`, `__init__.py` e `test_read.py`.

---

### Task 9: Bateria completa, verificação com o usuário, documentação e instalação

**Files:**
- Modify: `doc/gameforge/specs/2026-10-14-angelica-editor-design.md` (seção 7: "Estado (data)")

- [ ] **Step 1:** **rodar no Studio** todos os `tests/python/gameforge_*_test.py` (um processo por arquivo), mais `python -I tests/python/gameforge_unreal_link_test.py`, `gameforge_account_test.py` e `gameforge_plugins_test.py` com Python puro, e os três testes da UE (`test_ping.py`, `test_import.py`, `test_read.py`). Expected: tudo OK.

- [ ] **Step 2: verificação com o usuário** (a Unreal abre janela: combinar um momento em que ele não esteja jogando). No Studio: abrir os objetos e os spawns de a08 no cliente 1.7.8 e "Portar para Unreal Engine 5" com um projeto novo. Na Unreal: Landscape alinhado aos prédios e aos marcadores; mover um efeito e uma área de spawn. No Studio: "Trazer da Unreal" → "Aplicar no Studio" → "Salvar no cliente do jogo" num **cliente de teste copiado**; reabrir e conferir as posições.

- [ ] **Step 3:** atualizar a seção 7 da especificação com "Estado" (o que funciona; limites: modelos com esqueleto viram marcadores, camadas de textura do terreno e dados do jogo não vão, malhas reimportadas só quando a pasta delas não existe), instalar com `python -I D:\claude-scratch\gf-tools\deploy_install.py` (antes, conferir que nenhum `GameForge3DStudio` está aberto) e fazer o checkpoint final (`git status --short`).

---

## Fora deste plano (próximos planos da Frente 7)

- Dados do jogo na Unreal (registros do `elements.data`, missões) como Data Tables: a especificação os coloca na Frente 7(a) ("assets, mapas, dados"); ficam para o próximo plano, depois de a ponte estar de pé.
- Modelos com esqueleto (NPCs, monstros, modelos de cena) como Skeletal Mesh com animações; camadas de textura do Landscape a partir das máscaras `.t2mk`; reimportar malhas alteradas no Studio.
- (b) Rebranding e build do fork `shaddowdaniel/GameForgeEngine` e o instalador único; (c) port da lógica do jogo.
