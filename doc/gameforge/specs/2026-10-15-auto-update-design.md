# Atualização automática do GameForge 3D Studio — design

Data: 2026-10-15. Pedido do usuário: o Studio se atualiza pelo mesmo endpoint do PWOps Desktop
(`plugin-download` do portal atomgameforge.com), bastando subir no /admin um pacote reduzido, só com o que
mudou, sem reinstalar tudo. Depois disso, um MSI completo novo para baixar no card do Studio na página
"Minha conta".

## Decisões do usuário (2026-10-15)

- Instalação **por máquina, como hoje** (Arquivos de Programas): aplicar uma atualização pede a permissão do
  Windows (UAC).
- **Perguntar antes**: o Studio avisa que há versão nova (com o changelog) e só baixa e aplica quando o
  usuário confirma.
- O plugin do portal é criado **por mim, pelo banco** (conector do Supabase, mostrando o SQL antes); os
  arquivos são publicados pelo usuário no /admin › Versões.

## O que já existe e é reaproveitado

- `plugin-download` (v30): modo lista `{plugin_id, list: true}` → versões ativas com changelog; modo download
  `{plugin_id}` → `version`, URL assinada (10 min) do bucket `plugin-files`, `file_hash` (sha256),
  `signature` e `min_helper_version`. Autentica pela sessão do aparelho (`x-session-token`, a mesma do login
  do Studio em `gameforge_account`); o plano do usuário tem de alcançar o `required_plan` do plugin.
- /admin › Versões (`admin-sign › publish_release`): o admin sobe um zip **STORED** com `pwops-release.json`
  na raiz e um `payload` (um arquivo da lista). O servidor confere o sha256 de cada arquivo, grava só o
  payload em `<plugin_id>/<versão>/<nome>` e assina com Ed25519 o envelope
  `JSON.stringify({plugin_id, version, file_path, file_hash, kid})`. Guarda o `min_helper_version` do
  manifesto na versão.
- A conferência do launcher (`PWOps/155dll/launcher/updater.hpp`, `upd__sig_ok`), copiada tal e qual:
  `file_path` sai da URL assinada (`.../object/sign/plugin-files/<file_path>?token=...`, decodificada),
  `plugin_id` é o primeiro segmento dele, e cada chave pinada é testada com o seu `kid`. A assinatura é
  conferida **antes** de baixar e o sha256 **depois**.
- As chaves pinadas são as mesmas do launcher e da DLL (`PWOps/155dll/main/joias.hpp`, `g_joiaPins`:
  `key_ec46b5ca` e `key_7717f668`).
- `gameforge_account`: cliente do portal (`portal.py`), sessão do aparelho (`session.py`) e o cofre DPAPI
  (`vault.py`).
- O Python do Studio tem `zipfile` com LZMA (`_lzma.pyd`) e um `python.exe` próprio
  (`5.4\python\bin\python.exe`), que roda o aplicador sem depender do Studio aberto.

## Limites do portal (medidos em 2026-10-15)

- O projeto Supabase está no **plano Free**:
  - até **50 MB por arquivo** no Storage;
  - 1 GB de Storage no total (225,8 MB já usados);
  - 5 GB de tráfego de saída por mês.
- O `publish_release` lê o zip inteiro na memória da função (limite de 256 MB).
- O executável tem 105,9 MB. Comprimido, fica com 38,4 MB em deflate e 28,6 MB em LZMA.
  - Um pacote de atualização com o executável e os scripts cabe em 50 MB.
  - **O MSI completo, com 257 MB, não cabe.**

## Onde fica o MSI completo (decidido em 2026-10-15)

**GitHub Releases do repositório público** `shaddowdaniel/GameForge-3D-Studio-`, de graça. O usuário
escolheu esta opção em vez do Supabase Pro.

- **Arquivo e link:** cada release tem a tag `v<versão>` e o instalador com o nome fixo
  `GameForge-3D-Studio-Setup.msi`. Assim o link `releases/latest/download/GameForge-3D-Studio-Setup.msi` aponta
  sempre para o último. O card da "Minha conta" usa esse link.
- **Plano do usuário:** o link não passa pelo plano. Mesmo assim, o arquivo solto não libera nada, porque o
  Studio só abre com login e plano.
- **Código-fonte (GPL):** o Studio deriva do Blender (GPL). O release precisa do código correspondente
  publicado: commit, push e a mesma tag no repositório antes do release.
- **MSI pelo próprio Studio:** o Studio **nunca** roda um MSI sozinho. Quando a atualização não serve para a
  versão instalada, ele abre a "Minha conta" para baixar o instalador completo.

## Versão e manifesto da instalação

- Dois arquivos na raiz da instalação:
  - `gameforge-version.json`: `{version, base_version, build}`;
  - `gameforge-manifest.json`: `{caminho relativo: sha256}` de cada arquivo instalado.
- Quem os grava:
  - um script de instalação do CMake, ao fim do `install`, com o Python do build. Assim o MSI (o CPack instala
    numa pasta própria antes de empacotar), o `gf-build\bin` e o deploy de desenvolvimento têm os dois
    arquivos, conferindo com o que foi instalado;
  - o aplicador, depois de cada atualização.
- `version` é a versão do produto e a mesma do executável (`BKE_blender_version.h`). O próximo MSI sai como 5.4.0
  com o ciclo `release`: o título da janela deixa de dizer "5.4.0 Alpha". Cada atualização sobe o
  `BLENDER_VERSION_PATCH` (5.4.1, 5.4.2...), e assim título, tela inicial e versão do MSI acompanham. A
  comparação usa a mesma regra do launcher (`verCmp`: até 4 números, sufixo como no semver).
- `base_version` é a versão do último MSI instalado.

## O pacote de atualização (cumulativo)

O pacote vale para qualquer instalação entre o último MSI e a versão nova. Assim, o Studio baixa **um**
arquivo, sem cadeia de versões.

- `release/windows/gameforge/pack_update.py` (Python do sistema, sem `bpy`) recebe:
  - o build novo (`gf-build\bin` com o manifesto);
  - a versão e o changelog;
  - a pasta dos manifestos publicados (`instalador\manifests\`: o do MSI base e o de cada atualização desde
    ele).
- Entra no pacote todo arquivo cujo sha256 na versão nova difere do MSI base **ou de alguma atualização
  intermediária**. Esse "ou" cobre o caso de um arquivo mudado numa versão e desfeito na seguinte.
- Removidos: tudo o que algum desses manifestos tinha e a versão nova não tem.
- `.pdb` fica de fora (símbolos de depuração, 147 MB que mudam a cada build). O Windows ignora um `.pdb` que
  não confere com o executável.
- Saída:
  - `gameforge-3d-studio-<versão>.zip`: o pacote, com entradas LZMA, mais o `gameforge-update.json`
    `{schema: 1, version, base_version, files: [{path, sha256}], removed: [path], manifest: {...}}`;
  - `gameforge-3d-studio-<versão>-release.zip` (STORED, o que se sobe no /admin): o `pwops-release.json`
    `{schema: 1, plugin: "gameforge-3d-studio", version, changelog, files: [o pacote], payload: o pacote,
    min_helper_version: base_version}`;
  - `instalador\manifests\<versão>.json`.
- Recusa:
  - pacote acima de 45 MB, com a mensagem "gere um MSI novo";
  - versão que não é maior que a última;
  - changelog vazio ou com mais de 600 caracteres.
  O zip de release passa pela mesma validação do /admin (`parseRelease`).

## No Studio

1. **Procura.** Depois do login, uma thread consulta o modo lista de `gameforge-3d-studio`. Havendo versão
   maior que a instalada, a barra superior mostra um botão "Atualização 5.4.2" à direita, ao lado da conta.
   O menu do Studio ganha "Procurar atualizações".
2. **Confirmação.** O botão abre um diálogo com o changelog de cada versão nova e "Baixar e instalar" /
   "Agora não".
3. **Download.** "Baixar e instalar" pede o modo download e confere a assinatura com as chaves pinadas, antes
   de baixar.
   - Se `min_helper_version` (a base) for maior que a versão instalada, o diálogo explica e oferece
     "Baixar o instalador completo", que abre a "Minha conta".
   - Se não, baixa com progresso para `%LOCALAPPDATA%\ATOM GameForge\GameForge 3D Studio\updates\` e confere o
     sha256.
4. **Instalação.** "Fechar e instalar" inicia o vigia e pede para fechar o Studio, com o "salvar alterações?"
   de sempre. Se o usuário cancelar o fechamento, a instalação acontece quando o Studio fechar.

## O aplicador

`gameforge_update/apply.py`, só biblioteca padrão mais `ed25519.py` (RFC 8032 em Python puro, a mesma
conferência do Studio). Os dois ficam na instalação, em Arquivos de Programas, onde só o administrador
escreve. Roda com o `python.exe` do Studio, em dois papéis.

- **Vigia (sem elevação):**
  - espera não haver nenhum `GameForge3DStudio.exe` aberto, o que cobre várias janelas;
  - pede a elevação (UAC) para o papel "aplicar" e espera ele terminar;
  - reabre o Studio como o usuário (`GameForge3DStudio-launcher.exe`).
  Se o UAC for recusado, o pacote fica guardado e o Studio oferece de novo na próxima abertura.
- **Aplicar (elevado):**
  1. Copia o pacote para uma pasta da instalação. Ali o usuário comum não troca o arquivo entre a
     conferência e a cópia.
  2. Confere de novo a assinatura (o envelope chega na linha de comando), o sha256 e se a `base_version` não
     passa da instalada.
  3. Renomeia cada arquivo que vai ser trocado ou removido para `<nome>.gfold`. O Windows deixa renomear
     executáveis em uso, inclusive o `python.exe` do próprio aplicador.
  4. Extrai os novos e grava o manifesto e a versão.
  5. Apaga os `.gfold`. Os que estiverem em uso ficam para o reinício (`MoveFileEx`, `DELAY_UNTIL_REBOOT`).
  Qualquer erro devolve os `.gfold` aos nomes originais. O resultado vai para
  `updates\update.log`, que o Studio mostra na abertura seguinte ("Atualizado para 5.4.2" ou o erro).

## Portal e site

- Plugin `gameforge-3d-studio` ("GameForge 3D Studio"), `required_plan = 'pro'`: é o menor plano que inclui a
  ferramenta `estudio` em `plan_tools`. Eu crio a linha pelo conector, depois de mostrar o SQL.
- Card da ferramenta `estudio` na "Minha conta": ganha "Baixar o GameForge 3D Studio (Windows)", com o link
  da decisão acima, e a versão atual.
- O MSI completo novo é gerado com o `_gerar_instalador.ps1` (CPack + WiX) depois do auto-update **e do painel
  de agentes** prontos (`2026-10-15-agente-ia-design.md`): é o primeiro com os dois arquivos de versão, com o
  aplicador e com os 41 MB do MCP for Blender dentro. O manifesto dele vira o
  `instalador\manifests\5.4.0.json`, a base dos pacotes seguintes.

## Testes

- **`pack_update`**, com três builds falsos (pastas pequenas):
  - o pacote tem só os arquivos mudados;
  - o arquivo mudado na 5.4.1 e desfeito na 5.4.2 entra no pacote da 5.4.2;
  - os removidos e o `.pdb` são tratados como descrito;
  - o zip de release é STORED e tem o payload e o `min_helper_version`;
  - pacote grande demais é recusado.
- **Ed25519:** os vetores do RFC 8032. Um envelope assinado no teste com uma chave de teste passa, e o
  envelope alterado falha. A URL do Storage com caracteres codificados dá o `file_path` certo, e as chaves de
  produção não aceitam a assinatura de teste.
- **Aplicador (papel "aplicar" sem elevação, numa pasta temporária):**
  - instalação 5.4.0 e instalação 5.4.1 → as duas viram 5.4.2, com o manifesto certo;
  - um arquivo aberto com trava de escrita → tudo volta como estava e o log diz qual;
  - assinatura ou base erradas → nada muda.
- **Portal falso** (o padrão do `probe_remote_fake.py`): a lista, o download, `min_helper_version` acima da
  instalada → o diálogo oferece o instalador completo.
- **Interface:** `ui_check.py` com o botão de atualização e o diálogo.
- **Manual, uma vez, numa máquina com o MSI novo instalado:** pacote real publicado → UAC → Studio reaberto na
  versão nova. Abrir pelo atalho do menu Iniciar não pode disparar o "reparo" do Windows Installer, que
  devolveria os arquivos antigos.

## Riscos

- **Reparo do Windows Installer:** atalhos "anunciados" fazem o Windows conferir os arquivos do MSI e
  restaurá-los. Os do CPack não são anunciados; o teste manual acima confirma.
- **Tráfego do plano Free:** 5 GB/mês ≈ 150 downloads de um pacote de 30 MB, somados aos do PWOps.
- **Troca da chave de assinatura:** antes de assinar com uma chave nova, publicar uma atualização (assinada
  com a atual) que já pina a nova, como no launcher.
- **Desinstalar pelo Windows** deixa os arquivos que só as atualizações trouxeram; o MSI seguinte os
  sobrescreve.

## Fora do escopo

- Atualização silenciosa sem perguntar (decidido: perguntar antes).
- Instalação por usuário (decidido: por máquina).
- MSI pelo canal de atualização: só depois de um plano que comporte o arquivo.
- Canal beta separado.
