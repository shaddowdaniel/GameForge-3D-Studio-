# Frente 1 — Rebranding completo — plano de implementação

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** nenhum "Blender" visível ao usuário no Windows: processo (`tasklist`, Gerenciador de Tarefas), barra de tarefas, associação de arquivos, pastas de usuário, diálogos nativos, menus, dicas, traduções, scripts e instalador.

**Architecture:** o alvo CMake continua `blender` (34 usos internos); no Windows muda só o nome de saída (`GameForge3DStudio.exe`, `GameForge3DStudio-launcher.exe`). Os nomes ficam em definições globais do CMake (`BLENDER_WIN_EXE_NAME`, `BLENDER_WIN_LAUNCHER_NAME`, `BLENDER_WIN_APPID`, `BLENDER_WIN_APPID_FRIENDLY_NAME`) usadas por launcher, GHOST, registro e crash. Textos são trocados no código-fonte e as traduções são reconciliadas por script.

**Tech Stack:** CMake/Ninja (VS 2026), C/C++ (GHOST, blenlib, blenkernel, windowmanager), Python (UI), gettext `.po`, WiX.

**Spec:** `doc/gameforge/specs/2026-10-14-angelica-editor-design.md` (seção 1).

## Global Constraints

- Nome do executável: `GameForge3DStudio.exe`; launcher: `GameForge3DStudio-launcher.exe`.
- Nome do produto: `GameForge 3D Studio`; empresa: `ATOM GameForge`; site: `https://atomgameforge.com`.
- AppUserModelID/ProgID: `GameForge3DStudio.<major>.<minor>`; nome amigável: `GameForge 3D Studio <major>.<minor>`.
- Pasta de usuário: `%APPDATA%\ATOM GameForge\GameForge 3D Studio\<ver>`; migração copia (nunca apaga) a antiga `%APPDATA%\Blender Foundation\Blender\<ver>`.
- Manter: extensão `.blend`, assemblies `blender.crt`/`blender.shared`, nome do alvo CMake, nome interno do keymap `Blender`, nomes informados à Vulkan/OpenXR, textos de licença, comentários de código.
- Build: `D:\Atom Sec Labs\ATOM GameForge\_rebuild.bat` (log em `_rebuild.log`), saída em `gf-build\bin`.

## Review Focus

- Atalhos/associação `.blend` antigos apontando para `blender-launcher.exe` deixam de funcionar após a troca: o registro novo precisa ser feito (`--register`) e o ProgID antigo removido quando aponta para esta pasta.
- Caminho do executável perto de `MAX_PATH` ao trocar o nome pelo do launcher (+9 caracteres): checar tamanho antes de copiar.
- Migração interrompida no meio não pode deixar a pasta nova "pela metade": copiar para pasta temporária e renomear no fim.
- `BLENDER_USER_RESOURCES` ou instalação portátil: a migração não pode agir fora do caminho padrão.
- `--version` muda de texto: `url_prefill_startup.py` usa regex sobre ele.

---

### Task 1: Nomes do executável e do launcher

**Files:**
- Modify: `CMakeLists.txt:165-173` (definições globais WIN32)
- Modify: `source/creator/CMakeLists.txt` (OUTPUT_NAME dos dois alvos; `BLENDER_BIN`)
- Modify: `source/creator/blender_launcher_win32.c:100-104`
- Modify: `intern/ghost/intern/GHOST_WindowWin32.cc:1227-1262` (`registerWindowAppUserModelProperties`)
- Modify: `source/blender/blenlib/intern/system_win32.cc:546-560` (`bli_executable_path_get`), `:398-402` (pdb)
- Modify: `scripts/modules/_bpy_internal/system_info/url_prefill_startup.py:47,73`
- Modify: `tests/CMakeLists.txt:14`, `tests/performance/api/environment.py:178`, `build_files/windows/find_blender.cmd`
- Modify (fora do repo): `D:\Atom Sec Labs\ATOM GameForge\_gerar_instalador.ps1:19`, `compilar-gameforge-studio.bat:3,20`

**Interfaces:**
- Produces: macros `BLENDER_WIN_EXE_NAME` (`"GameForge3DStudio.exe"`), `BLENDER_WIN_LAUNCHER_NAME` (`"GameForge3DStudio-launcher.exe"`), CMake var `BLENDER_WIN_EXE_STEM`.

- [ ] **Step 1:** em `CMakeLists.txt`, dentro de `if(WIN32)` das definições globais:

```cmake
  # Names of the executables on Windows (the CMake targets stay `blender` / `blender-launcher`).
  set(BLENDER_WIN_EXE_STEM "GameForge3DStudio")
  add_definitions(
    -DBLENDER_WIN_APPID="GameForge3DStudio.${BLENDER_VERSION_MAJOR}.${BLENDER_VERSION_MINOR}"
    -DBLENDER_WIN_APPID_FRIENDLY_NAME="GameForge 3D Studio ${BLENDER_VERSION_MAJOR}.${BLENDER_VERSION_MINOR}"
    -DBLENDER_WIN_EXE_NAME="${BLENDER_WIN_EXE_STEM}.exe"
    -DBLENDER_WIN_LAUNCHER_NAME="${BLENDER_WIN_EXE_STEM}-launcher.exe"
  )
```

- [ ] **Step 2:** em `source/creator/CMakeLists.txt`, após criar os alvos no ramo WIN32:
  `set_target_properties(blender PROPERTIES OUTPUT_NAME ${BLENDER_WIN_EXE_STEM})` e
  `set_target_properties(blender-launcher PROPERTIES OUTPUT_NAME ${BLENDER_WIN_EXE_STEM}-launcher)`;
  `set(BLENDER_BIN "${BLENDER_WIN_EXE_STEM}.exe")`.
- [ ] **Step 3:** launcher: `PathCchCombine(path, MAX_PATH, path, WIDEN(BLENDER_WIN_EXE_NAME))` com
  `#define WIDEN2(x) L##x` / `#define WIDEN(x) WIDEN2(x)`; comentários atualizados.
- [ ] **Step 4:** GHOST: `strstr(blender_path, BLENDER_WIN_EXE_NAME)`, checagem de tamanho e
  `strcpy(blender_app, BLENDER_WIN_LAUNCHER_NAME)`.
- [ ] **Step 5:** `system_win32.cc`: `PathAppendW(executable_path, WIDEN(BLENDER_WIN_LAUNCHER_NAME))`;
  o pdb continua instalado como `blender.pdb` (nome interno, não visível).
- [ ] **Step 6:** `url_prefill_startup.py`: executável `GameForge3DStudio.exe` e regex
  `^GameForge 3D Studio (.*)`; testes/scripts de build com o nome novo.
- [ ] **Step 7 (verificação, após build da Task 10):** `tasklist /FI "IMAGENAME eq GameForge3DStudio.exe"`
  mostra o processo; abrir pelo launcher funciona; `--background --python-expr` retorna 0 pelo launcher.

### Task 2: Recursos de versão, ícone de arquivo e miniaturas

**Files:**
- Modify: `release/windows/icons/winblender.rc:27-31`
- Modify: `source/blender/blendthumb/src/blendthumb_win32.rc`, `blendthumb_win32_dll.cc:20`
- Modify: `release/windows/icons/winblenderfile.ico` (gerado por `tools/gameforge/branding/make_branding.py`)

- [ ] **Step 1:** `.rc`: `CompanyName "ATOM GameForge"`, `FileDescription "GameForge 3D Studio"`,
  `LegalCopyright "GPLv3 (ATOM GameForge, Blender Foundation)"`, `OriginalFilename "GameForge3DStudio.exe"`,
  `ProductName "GameForge 3D Studio"`.
- [ ] **Step 2:** BlendThumb: textos "GameForge 3D Studio Thumbnail Handler"/"GameForge 3D Studio" e bloco de
  tradução `040904B0` coerente com `VarFileInfo`.
- [ ] **Step 3:** ícone de documento `.blend` com o brasão GameForge (script de branding).
- [ ] **Step 4 (verificação):** `(Get-Item GameForge3DStudio.exe).VersionInfo` mostra os textos novos.

### Task 3: Registro (ProgID/AppID) sem conflito com o Blender oficial

**Files:**
- Modify: `source/blender/blenlib/intern/winstuff.cc:183-261` (`BLI_windows_register_blend_extension`), mensagens `:118,154,175,216-253`
- Modify: `source/blender/editors/space_userpref/userpref_ops.cc:1027`

- [ ] **Step 1:** caminho do launcher montado como `<pasta do executável>\` + `BLENDER_WIN_LAUNCHER_NAME`
  (sem `strstr("blender.exe")`), com checagem de tamanho.
- [ ] **Step 2:** remover o ProgID legado `blender.<maj>.<min>` (e seu valor em `.blend\OpenWithProgids`)
  somente se o comando dele apontar para a pasta desta instalação.
- [ ] **Step 3:** mensagens "Unable to register GameForge 3D Studio ..." e caixa de erro com título do produto.
- [ ] **Step 4 (verificação):** `GameForge3DStudio.exe --register` (usuário atual) cria
  `HKCU\Software\Classes\GameForge3DStudio.5.4` com comando para o launcher novo.

### Task 4: Pastas de usuário próprias + migração

**Files:**
- Modify: `intern/ghost/intern/GHOST_SystemPathsWin32.cc:35,56`
- Modify: `source/blender/blenkernel/intern/appdir.cc:220-226` (cache) e nova função de migração
- Modify: `source/blender/blenkernel/BKE_appdir.hh` (declaração)
- Modify: `source/creator/creator.cc:529` (chamada após `BKE_appdir_init()`)
- Modify: `source/blender/blenkernel/intern/preferences.cc:229`

**Interfaces:**
- Produces: `void BKE_appdir_legacy_user_folder_migrate();`

- [ ] **Step 1:** GHOST usa `\ATOM GameForge\GameForge 3D Studio\` para ProgramData e AppData.
- [ ] **Step 2:** cache em `%LOCALAPPDATA%\ATOM GameForge\GameForge 3D Studio\Cache`; biblioteca padrão em
  `Documentos\GameForge 3D Studio\Assets`.
- [ ] **Step 3:** migração: se o caminho de usuário calculado termina em
  `\ATOM GameForge\GameForge 3D Studio\<ver>`, não existe, e a pasta legada existe → `std::filesystem::copy`
  recursivo para `<novo>.migrando`, depois renomeia para `<novo>`; em erro apaga o temporário e segue.
- [ ] **Step 4 (verificação):** com a pasta nova ausente, abrir o Studio cria a pasta nova com
  `config\userpref.blend` e o login continua válido; a pasta antiga continua intacta.

### Task 5: Diálogos nativos, console e linha de comando

**Files:**
- Modify: `source/blender/blenlib/intern/system_win32.cc:600-690` (diálogo de crash)
- Modify: `intern/ghost/intern/GHOST_SystemWin32.cc:2873`, `GHOST_WindowWin32.cc:134,145`
- Modify: `source/blender/cpucheck/cpu_check.cc:75`
- Modify: `source/blender/blenkernel/intern/blender.cc:251` (`blender.crash.txt`)
- Modify: `source/creator/creator_intern.h:95`, `creator_signals.cc:79,82,102-106`, `creator_args.cc` (`--version`/`--help`), `creator.cc:620`
- Modify: `source/blender/windowmanager/intern/wm_init_exit.cc:783`, `wm_platform_support.cc:146-224`

- [ ] **Step 1:** títulos/textos com "GameForge 3D Studio" (diálogo de crash em português; "Reportar
  problema" abre `https://atomgameforge.com`); log de crash `gameforge3dstudio.crash.txt`.
- [ ] **Step 2:** `--version` imprime `GameForge 3D Studio <ver>`; `--help` com `Usage: GameForge3DStudio [args ...]`.
- [ ] **Step 3 (verificação):** `GameForge3DStudio.exe --version` e `--help` sem "Blender".

### Task 6: Textos C/C++ da interface

**Files:** `wm_operators.cc`, `wm_splash_screen.cc`, `wm_files.cc`, `wm_event_system.cc:2897`,
`render_view.cc:178`, `screen_ops.cc:6302,7475,7532`, `interface_template_status.cc:387`, `text_ops.cc`,
`eyedropper_*.cc`, `screendump.cc`, `info_ops.cc`, `io_usd.cc`, `file_draw.cc`, mensagens de nós, `makesrna/intern/rna_*.cc`.

- [ ] **Step 1:** trocar "Blender" por "GameForge 3D Studio" em strings de interface (nomes/descrições de
  operadores, títulos de janelas, relatórios, `RNA_def_*_ui_text`, enums), mantendo identificadores,
  comentários e textos de licença.
- [ ] **Step 2 (verificação):** `grep` de literais com `Blender` restantes em `source/` revisado à mão.

### Task 7: Interface Python e add-ons embutidos

**Files:** `scripts/startup/bl_ui/space_topbar.py`, `space_userpref.py`, `space_filebrowser.py`,
`scripts/startup/bl_operators/wm.py`, `userpref.py`, `image_as_planes.py`, `file.py`, `image.py`, `assets.py`,
`project.py`, `presets.py`, `scripts/modules/_bpy_internal/system_info/text_generate_runtime.py`,
`scripts/presets/interface_theme/Blender_Light.xml` (renomear), `scripts/addons_core/**` (rótulos/descrições).

- [ ] **Step 1:** menu do app "GameForge 3D Studio"; Ajuda com site/suporte ATOM GameForge (manual e API
  continuam disponíveis, sem o nome); splash/Sobre sem doação/loja; Quick Setup "Importar preferências {}.{}".
- [ ] **Step 2:** nome exibido do keymap padrão "GameForge 3D Studio" (nome interno continua `Blender`);
  tema claro renomeado `GameForge_Light.xml`.
- [ ] **Step 3 (verificação):** script em segundo plano lista rótulos de menus/operadores sem "Blender".

### Task 8: Arquivos de release e instalador

**Files:** `release/windows/batch/*.cmd` (renomear para `gameforge_*.cmd`), `source/creator/CMakeLists.txt:1755-1792`,
`release/text/readme.html`, `build_files/cmake/packaging.cmake:9,130-131`, `release/windows/installer_wix/WIX.template:69,75`.

- [ ] **Step 1:** scripts `.cmd` chamam `GameForge3DStudio`, textos em português, logs em `%temp%\gameforge`.
- [ ] **Step 2:** `readme.html` reescrito para o GameForge 3D Studio.
- [ ] **Step 3:** contato do instalador `ATOM GameForge`; chave de arquivo do launcher no WiX.

### Task 9: Traduções

**Files:** `locale/po/*.po`, script `tools/gameforge/rebrand_po.py`

- [ ] **Step 1:** script: para cada entrada com "Blender" no `msgid`, se o texto com "GameForge 3D Studio"
  existir como literal no código, renomeia o `msgid`; em todo `msgstr`, troca "Blender" pelo produto.
- [ ] **Step 2 (verificação):** build compila os `.mo` sem erro; `pt_BR` mostra "Sair do GameForge 3D Studio".

### Task 10: Build, instalação e verificação final

- [ ] **Step 1:** `_rebuild.bat`; `EXIT 0` no log.
- [ ] **Step 2:** copiar `gf-build\bin` para a pasta instalada (removendo `blender.exe`/`blender-launcher.exe`
  antigos), `GameForge3DStudio.exe --register`.
- [ ] **Step 3:** `tasklist`, propriedades do exe, título/ícone da janela, menus em pt_BR, associação `.blend`,
  login GameForge após migração.
