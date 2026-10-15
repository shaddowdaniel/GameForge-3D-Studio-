# GameForge 3D Studio como editor completo da Angelica Engine — design

Aprovado em 2026-10-14. Este documento guia as próximas entregas; detalhes de formato citados aqui
vêm dos sources da engine (Angelica 2.2.7, cliente 1.5.5 `EvolvedPW1.5.5`, cliente 1.7.8 `Api178`),
do Rody's Angelica Editor, do GameForge Estudio web e das ferramentas WinPCK / sPCK / autoangel-rs.

## Objetivo

O GameForge 3D Studio é o editor 3D completo de Perfect World / Angelica Engine, usando as ferramentas
do Blender e as do GameForge. O usuário não precisa saber nada sobre os arquivos do jogo: o Studio decide
sozinho qual pacote, caminho, id e formato ler ou gravar, e só grava quando o usuário salva.

## Frentes (ordem de entrega)

1. **Rebranding completo** — nada de "Blender" visível.
2. **Fundação** — leitura correta de todos os clientes, índice do cliente, coletor de vínculos.
3. **Biblioteca do Cliente** — no Navegador de Assets; abrir um asset com todos os vínculos.
4. **Editar + Salvar no cliente** — texturas, malhas `.ski`, textos, esqueleto/animação; três modos de salvar.
5. **Copiar entre clientes** — fecho completo (arquivos + `elements.data` + textos de `configs` + ícones).
6. **Editor de mundo** — terreno, objetos `.ecwld`, spawns `npcgen`, serviços de NPC, missões.
7. **Ponte Unreal** — enviar assets/mapas para a Unreal e trazer de volta o que foi editado lá.

Cada frente termina verificada (build + teste no Studio real) antes da próxima.

## 1. Rebranding

- Executável `GameForge3DStudio.exe` e launcher `GameForge3DStudio-launcher.exe` (o alvo CMake continua
  `blender`; muda só o `OUTPUT_NAME` no Windows). Pontos que precisam mudar juntos: launcher
  (`blender_launcher_win32.c`), registro de `.blend` (`winstuff.cc`), propriedades de janela da barra de
  tarefas (`GHOST_WindowWin32.cc`), reinício após crash (`system_win32.cc`), `url_prefill_startup.py`,
  scripts `.cmd`, testes, empacotamento WiX.
- Recurso de versão (`winblender.rc`): GameForge 3D Studio / ATOM GameForge.
- AppUserModelID e ProgID próprios (`GameForge3DStudio.<major>.<minor>`), para não dividir o grupo da barra
  de tarefas nem a associação `.blend` com um Blender oficial. O registro remove o ProgID antigo
  `blender.<ver>` apenas quando ele aponta para a pasta desta instalação.
- Pasta de usuário própria: `%APPDATA%\ATOM GameForge\GameForge 3D Studio\<ver>` (e equivalentes em
  ProgramData / LocalAppData / Documentos). Na primeira execução, se ela não existir e a antiga
  (`Blender Foundation\Blender\<ver>`) existir, a antiga é copiada (nunca apagada).
- Textos: menus, splash, Sobre, Ajuda (atomgameforge.com), títulos de janelas, diálogos, crash,
  `--help`/`--version`, readme, add-ons embutidos, traduções (`locale/po`). Regra para traduções: um
  `msgid` com "Blender" é renomeado quando o texto em inglês correspondente mudou no código; todo `msgstr`
  troca "Blender" por "GameForge 3D Studio".
- Mantidos de propósito (internos ou legais): extensão `.blend`, assemblies `blender.crt`/`blender.shared`,
  nome de aplicativo informado à Vulkan/OpenXR, nome interno do keymap padrão, textos de licença.

## 2. Fundação

- **Chaves de pacote**: suportar a variante do 1.7.8 (guards do id 0 com `MASK 0xA7F88E3B`,
  `CHECK 0x4B813BDD`) e detectar a variante pelo teste da primeira entrada da tabela, como o WinPCK.
- **Ordem de busca igual à da engine**: pacote primeiro, arquivo solto só quando o nome não existe no pacote
  (`AFileImage.cpp:38-75`). O pacote de um caminho é o primeiro cujo prefixo casa (`help\script` tem dois
  níveis).
- **Sessão de cliente**: cada cliente é aberto uma vez; pacotes ficam abertos e indexados em memória.
- **Coletor de vínculos** portado do coletor oficial `GfxEditorUtil.cpp` (cliente 1.5.5), com as regras:
  - `.ecm` (texto): `SkinModelPath`, `AddiSkinPath` (prefixo `Models\`), `ChildPath`, `PhysFileName`,
    eventos de efeito (`Gfx\`), som (`Sfx\`), ataque (`.att`), shaders, `.ecp` implícito em
    `grasses\ECModelHull\`.
  - `.smd`: `.bon`, `.ski`, pasta de trilhas `tcks_*` → `.stck`, ganchos de efeito (`Gfx\`) e modelos.
  - `.ski`: texturas pelo nome em `<pasta>\Textures\`, depois `<pasta>\Tex_<título>\`; `_normal` opcional.
  - `.gfx` (texto `MOXT`): `TexFile`/`ShaderTex` (`Gfx\Textures\`), `GfxPath`, `ModelPath`
    (`gfx\Models\`), `ECMPath`, sons (`Sfx\`), `RefModel`.
  - `.att`/`.sgc` (texto): três `Path:` de `.gfx`.
  - `.bmd`: textura de 256 bytes por malha, `nrf_` e `_normal` implícitos.
  - `elements.data`: campos `file_*` são caminhos GBK de 128 bytes embutidos no registro (não usam
    `path.data` no PW); armaduras e modas usam modelos de caminho do executável.
- Correções no leitor: comentário errado de `path.data`, prefixos `Models\`/`Gfx\`.

## 3. Biblioteca do Cliente

- Seção "Cliente do jogo" no painel lateral do Navegador de Assets.
- Escolha da pasta do cliente em Arquivo › Importar › "Cliente do jogo (pasta)..." e na própria seção.
- Árvore agrupada por arquivo contêiner: cada `.pck` (com suas partes `.pkx`) e os mapas soltos
  (`maps\<código>` com `.trn2`, `.ecwld`, ...). Busca, filtro por tipo, contagem por grupo, expansão sob
  demanda (a árvore visível é pequena mesmo com centenas de milhares de entradas).
- "Abrir" importa o asset e tudo o que ele usa, sem extrair nada em disco. Cada dado importado guarda sua
  origem (cliente, contêiner, caminho, hash original) nas propriedades do ID.

## 4. Editar + Salvar no cliente

- **Alterações pendentes**: lista no estilo do Update Planner (antes → depois, arquivo, pacote, cliente).
  "Mudou" é decidido por hash do estado atual no Studio (geometria, pixels, texto) contra o instantâneo da
  abertura, então funciona depois de fechar e reabrir o `.blend`.
- **Gravadores**:
  - Texturas: DDS no mesmo formato do original (DXT1/DXT3/DXT5 ou sem compressão, com mipmaps); TGA.
  - `.ski`: reescreve só as malhas editadas; bytes não modelados são preservados.
  - Texto: mesma codificação (GBK ou UTF-16 com BOM) e quebras CRLF.
  - `.bon`/`.stck`: a partir dos `Save()` da engine.
- **Salvar no cliente** (menu Arquivo e painel), três modos:
  - *Dentro do original*: dados novos anexados após o fim lógico (partes `.pkx` respeitando os limites),
    tabela e rodapé completos, e por último o cabeçalho seguro no início do arquivo (ponto de confirmação
    da engine). Uma queda antes disso deixa o pacote antigo intacto. O cabeçalho anterior fica num diário
    ao lado do pacote para "Desfazer último salvamento".
  - *Pacote novo só com o que mudou*: pacote padrão (mesma versão e chaves) com os caminhos completos. O
    jogo não carrega pacotes extras, então ele é um patch que o Studio aplica em qualquer cliente.
  - *Cópia completa*: copia os bytes armazenados das entradas sem recompressão, troca as editadas e grava
    em outra pasta.
- Toda gravação termina relendo o pacote e conferindo todas as entradas. Nunca grava com o jogo aberto.

### Estado (2026-10-14)

- Feito e testado só em cópias: gravador `.pck` (`angelica_pck_writer.cc`: no lugar com diário,
  verificação e desfazer; patch; cópia completa; partes `.pkx`; recusa pacote sem cabeçalho seguro,
  criptografado ou aberto por outro programa), texturas DDS (DXT1/3/5 por `angelica_dds.cc`, sem
  compressão por máscaras) e TGA, textos, `.ski` v9 (malhas skin e rígidas, só os registros editados),
  `.stck` v2 (trilhas densas sem compressão) e `.bon` (pose de repouso, só as matrizes afetadas).
- Falta: `.ski` de outras versões, em modo texto e malhas morph/supple/muscle; `.stck` v1; ganchos do
  `.bon`; compactar o pacote (cada gravação no lugar deixa a tabela anterior como bytes mortos, que o
  desfazer usa).
- Testes: `tests/python/gameforge_pck_writer_test.py`, `gameforge_client_save_test.py`,
  `gameforge_skin_save_test.py`, `gameforge_anim_save_test.py`.

### Regras dos gravadores (fontes: `A3DSkin.cpp`, `A3DSkinModel.cpp`, `A3DTrackMan.cpp`, `AFile.cpp`)

- Binários levam o cabeçalho `MOXB` (`.ski`, `.smd`, `.bon`, `.mox`, `.bmd`); sem ele o carregador lê como
  texto e falha. Arquivos de mapa (`.ecwld`, `.trn2`, `.t2bk`, `.t2mk`, `.t2lm`) não têm cabeçalho.
- Versões que todos os clientes aceitam (1.5.5 é o mais restrito): `.ski` v9, `.smd` v8, `.stck` v1, `.bon` v6.
  Arquivos existentes mantêm a própria versão quando ela já é aceita.
- Strings binárias: `int32` tamanho + bytes GBK, sem NUL; texto: `MOXT`, linhas `%s\n` com CRLF, até 2048 bytes.
- `.ski`: até 4 ossos por vértice, 3 pesos gravados (o 4º é `1 - soma`), índices na lista de nomes de ossos da
  própria skin, vértice sem peso aponta para "o modelo" (índice = número de ossos); índices `WORD` (≤ 65535
  vértices por malha).
- DDS: mesmo formato do original; cadeia completa de mipmaps; `ddpf.dwFlags == DDPF_FOURCC` exato.
- Arquivos de referência oficiais: `C:\Users\danie\Documents\PW Server\Tools\Editors\models\半龙骑士.*`.

### Layout do `.pck` (referência)

- Cabeçalho seguro no offset 0: v2 `u32 0x4DCA23EF, u32 fim, u32 0x56A089B7`; v3 `u32 tag, u64 fim`
  (+ `u32 0x56A089B7` no 1.7.8).
- Dados: zlib somente quando `armazenado < tamanho` (a engine usa nível 1 e grava cru se não diminuir).
- Tabela: por arquivo `u32 len^MASK, u32 len^MASK^CHECK` e `zlib(FILEENTRY)` (cru quando `len` é o
  tamanho da estrutura). FILEENTRY v2 276 B, v3 288 B (`nome[260]` GBK com `\`, offset, tamanhos).
- Rodapé: v2 272 B em `fim-280` + `count`, `version`; v3 280 B em `fim-288`, offset da tabela em 64 bits
  com máscara estendida por sinal.
- Partes: `.pck` até 0x7FFFFF00; clientes 64 bits usam `.pkx`, `.pkx1`, ... de 0xFFFFFE00 (a última menor).
  Pacotes v2 têm teto lógico de 4 GiB.
- Chaves: `GUARD0 0xFDFDFEEE + id·0x072341F2`, `GUARD1 0xF00DBEEF + id·0x01237A73`,
  `MASK 0xA8937462 + id·0x0AB2321F`, `CHECK 0x59374231 + id·0x0987A223`, mais a variante do 1.7.8.

## 5. Copiar entre clientes

- Fecho de arquivos pelo coletor de vínculos; fecho de dados pelo esquema de vínculos do Rody's
  (`configs\PW\PW_types.csv`: lista + regex de campo → lista alvo) e pelo `collect_refs` do Estudio.
- Ids novos a partir de 200001 (sem colisão no 1.7.8); remapeia todas as referências do fecho; leva
  `item_ext_desc`, `item_color`, `item_ext_prop`; habilidades não são copiáveis (vivem na DLL).
- Ícones: acrescenta o nome em minúsculas e a imagem na próxima célula dos atlas `IconList_*` (M e F),
  regravando o DDS no formato original.
- Armaduras/modas: renomear o conjunto de `.ski` para o modelo de caminho do cliente de destino.

### Estado (2026-10-14)

- Parte A feita (`gameforge_angelica/transfer.py`, botão "Copiar para outro cliente..."): fecho de arquivos
  pelo coletor da engine; cada arquivo vai para o pacote da sua pasta no destino (ou solto); iguais são
  pulados, diferentes ficam a menos que o usuário peça para substituir; grava pelos 3 modos do salvamento
  (no destino com desfazer, patch ou cópia).
- Versões que cada cliente lê, amostradas dos próprios arquivos: PWU v156 `.stck` 1, `.ski` 9, `.smd` 8,
  `.ecm` 67; 1.7.x e PWNH `.stck` 2, `.ski` 101, `.smd` 9, `.ecm` 71/72. A cópia converte `.stck` 2→1
  (trilhas densas, um segmento), `.ski` 100/101→9 (sem tangentes) e `.smd` 9→8 (ações sem fps próprio);
  o que não sabe converter é listado e só vai se o usuário pedir.
- `.ecm` 68–71→67 pelos gates do `EC_Model.cpp`/`A3DCombinedAction.cpp` do 1.7.8 (grupos de AudioEvent,
  ParticleBones, `GfxUseFixedPoint`, `Group`/`FadeOut` de áudio, removidos por posição); para clientes com a
  GfxCommon do 1.5.5 (`SoundVer` ≤ 5) os eventos de som perdem `PercentStart`/`Group`. A v72 (PWNH) não existe
  em nenhum fonte e fica como não suportada.
- Parte B feita para registros (`gameforge_angelica/data.py`, painel "Dados do jogo"): fecho pelas regras de
  nome de campo e pela tabela `PW_types.csv` do Rody's quando presente; tipos compartilhados quando o destino
  os tem; registros iguais reaproveitados; os demais com ids novos a partir de 200001 no espaço de ids certo;
  referências remapeadas; campos mapeados por nome entre versões (testado 1.7.8 v402 → PWU v156); citações a
  itens que o destino não tem são zeradas e listadas. O `elements.data` do destino é gravado junto com os
  arquivos num único salvamento.
- `elements.data` compactado (v350+): o bloco "org" depois do cabeçalho passa de 1 MB nos arquivos reais
  (1.7.6 v386, 1.7.8 v402, PWNH v600 agora abrem com ida e volta idêntica).
- Junto com os registros vão: as linhas `id<TAB>valor` de `configs\item_ext_desc.txt` e `item_color.txt` com
  o id novo; o ícone nos dois atlas (`iconlist_ivtrm`/`ivtrf`), na próxima célula livre, trocando só os
  blocos DXT da célula em cada mip onde ela cobre blocos inteiros (nome próprio `gf<id>_<nome>` quando o
  destino usa o mesmo nome para outra imagem); e, se o cliente tiver a pasta do servidor registrada, o
  `elements.data` do servidor (o mesmo arquivo do cliente, ou os mesmos registros com os mesmos ids).
  Um salvamento que toca cliente e servidor é desfeito por inteiro.
- Armaduras e modas: as skins vêm de `realname` como no `EC_Player.cpp` do 1.7.8 (`<raiz>\男|女\R\男通用R|妖兽R|
  女通用R|妖精R` + `一级/二级/三级.ski`, variantes pelo `character_combo_id` da armadura ou pelo `gender` da moda).
  A raiz de cada cliente é sondada (`models\players\装备` no PWU e 1.7.8, `models2\players\装备` no PWNH,
  `styles\装备` e `evolved\players\装备` em builds próprios) e o conjunto vai para a raiz do destino com as
  texturas da pasta (`Textures\`, `Tex_<skin>\`).

## 6. Editor de mundo

- Formatos `.ecwld` (v9–12), `.ecbsd`, `BSData\N.dat`, `.bmd`, `npcgen.data` (por versão), `.hmap`,
  `region/precinct .clt/.sev`. Regras certas (diferentes do web): precinto de menor prioridade vence;
  `BSData` usa `names[iSubTerrain]`; grama começa no canto noroeste; `vExts` é o tamanho total da área.
- `.ecwld` v16/v17 e `.ecbsd` v17/v18 (mapas novos do 1.7.8 e do PWNH) não têm fonte; as diferenças foram
  medidas no PWOps ("Mapa 1.7.8: cena", `PWOps\docs-site\docs\mapa-178-cena.md`): v16 = v12; v17 põe um
  float depois de `up` no modelo de cena; `.ecbsd` v17/v18 só aumenta a área de ambiente (364/368 bytes,
  os 224 primeiros como na v11, e 20 textos).

### Estado (2026-10-14)

- Spawns feitos (`gameforge_angelica/npcgen.py`, `spawns.py`, aba "GameForge" da Vista 3D): o `npcgen.data`
  do servidor (`<servidor>\<mapa>\npcgen.data`; a pasta do servidor fica no registro do cliente) abre como
  empties (caixa ou círculo do tamanho da área, virados pela direção dela; `vExts` é o tamanho total, como
  em `base_spawner::SetRegion`); a lista de NPCs/monstros de cada área (com o nome do `elements.data` e
  busca por nome), quantidades e tempos ficam no painel "Spawn", e o modelo do primeiro NPC/monstro aparece
  em cada área. Mover, girar, redimensionar, duplicar, apagar e criar áreas edita o arquivo, que é salvo
  pelo "Salvar no Cliente do Jogo" com desfazer. Valores que não mudaram mantêm os números do arquivo
  (por eixo, no tamanho); os 104 `npcgen.data` do servidor 1.7.8 (v4, 5, 10, 11) fazem ida e volta byte a
  byte. Áreas de recurso e objetos dinâmicos: só posição, por enquanto.
- Cena do cliente feita (`ecwld.py`, `scene.py`, "Abrir objetos do mapa"): todas as cenas dos clientes
  1.7.8, PWNH e PWU leem (v12/16/17 com `.ecbsd` v12/17/18). Prédios aparecem onde o cliente os desenha
  (a posição vem do próprio `.bmd`: `CECOrnament::SetPos` não move o prédio), modelos `.ecm` de cena,
  efeitos, sons (alto-falantes), árvores, áreas de ambiente, criaturas e rotas. Efeitos, modelos e sons
  podem ser movidos e girados e são salvos no `.ecwld` (só os números deles mudam). Prédios ficam travados:
  a colisão deles está no `.bmd` e no servidor. Mapas grandes abrem por região (em volta do cursor 3D).
- Prédios `.bmd` das versões novas (sem fonte, medidas em 1.459 arquivos do 1.7.8 e do PWNH): `0x10000301`
  = EX com blocos extra maiores; `0x10000302` = índices de 32 bits; `0x10000303` = `int32` depois da
  transformação; `0x10000304` = `int32` por malha antes do material; `0x10000305` = 4 floats no lugar dele.
- Editor de registros do `elements.data` feito (`records.py`, painel "Registro do jogo" na aba GameForge e
  no Navegador de Assets, botão no template do spawn): abre qualquer registro, mostra o nome do que cada id
  indica (serviços do NPC em português, itens das lojas), segue os vínculos e volta, escolhe pelo nome.
  As edições ficam na cena (desfazer, `.blend`) e o "Salvar no Cliente do Jogo" grava o `elements.data` do
  cliente e o do servidor juntos, com desfazer (o Estudio web só gravava o do servidor e não editava os
  itens das lojas).
- Editor de missões (`tasks.py`, `bl_operators/gameforge_tasks.py`, painel "Missão do jogo"): o leitor e
  gravador C++ do `tasks.data` (`angelica_tasks.cc`) ganhou bindings `_angelica.tasks_*`; abre a missão pela
  busca de nome, navega nas subtarefas, edita textos e números (id e vínculos de subtarefa travados) e salva
  cliente + servidor com desfazer. Os serviços de NPC mostram o nome das missões e abrem a missão. Todas as
  missões leem até o último byte nas três versões dos clientes (v128 PWU 16.001, v194 1.7.8 22.880, v237
  PWNH 26.451): a tabela de `CombinedSwitches` foi conferida com o `ECombinedSwitch` dos servidores 1.7.6
  (v191, 20 bytes) e 1.7.8 (v194, 21 bytes: faltavam `PremRank`..`RecordDeliverTime`) e completada com as
  chaves 198–236 do sTASKedit (v237, 24 bytes). Referência da v194: `cgame\gs\task\` do servidor 1.7.8.
- Editor de registros: "Duplicar registro" cria a cópia com id novo (de 200001 em diante, como a cópia entre
  clientes) e a salva na hora no cliente e no servidor, com desfazer.
- "Duplicar missão" copia a missão com as subtarefas para ids novos (depois do maior id do arquivo; os
  textos são cifrados de novo e os vínculos entre as subtarefas acompanham) e salva cliente + servidor com
  desfazer. Os campos de missão dos serviços de NPC escolhem a missão pelo nome.
- Cena: água (`A3DWaterArea` dentro de `BSData\N.dat`, nas posições que o `.ecbsd` indica) aparece como
  malha das células com água, na altura e na cor do arquivo. Efeitos, modelos e sons também podem ser
  duplicados (Shift+D: ganham id novo ao salvar) e apagados; o `.ecwld` é remontado bloco a bloco na ordem
  original (as 303 cenas dos três clientes saem idênticas sem edição), e um objeto movido para outro bloco
  passa para a lista desse bloco. Só se apaga o que foi aberto; depois de um desfazer, salvar de novo
  refaz as cópias e as exclusões.
- Correções de 2026-10-15 (achadas nos renders de divulgação):
  - Malhas (`.ski`/`.smd`/`.bmd`): a troca Y/Z espelha o modelo e mantém o sentido horário visto de fora,
    então os triângulos trocam os dois últimos cantos ao abrir e destrocam ao salvar o `.ski` (antes as faces
    ficavam para dentro e a luz vinha de trás: modelos escuros). Triângulos sem área são descartados (não
    desenham nada no jogo e quebravam as normais personalizadas em volta deles).
  - Terreno: as texturas de camada e o lightmap usam o alfa para outra coisa (alfa "Channel Packed": antes
    a cor sumia e o chão ficava preto ou transparente), e o lightmap modula ×2 com saturação, como o shader do
    cliente (`t2_3layer_batch_14`, `mul_x2`).
- Falta: árvores SpeedTree (`.spt`) e grama (bitmaps em `BSData`), criar subtarefas e itens de lista nas
  missões (o C++ já sabe inserir itens), copiar missões entre clientes de versões diferentes, e investigar
  as peças que aparecem soltas em alguns modelos `.ecm` (ex.: chefe "Marechal: Duque Sombrio",
  `攻城总BOSS.ecm`; suspeita: peças presas em ganchos que o importador não liga aos ossos).

## 7. GameForge Engine e ponte Unreal

Decidido em 2026-10-14: a Unreal do usuário é o fork `shaddowdaniel/GameForgeEngine` (privado, do
`EpicGames/UnrealEngine`, branch `release`), que será rebatizado "GameForge Engine". Studio e Engine formam
uma suíte única: um instalador, a mesma conta, o mesmo registro de clientes, abertura de um no outro.

- Licenças: Studio (GPL) e Engine (EULA da Epic) são programas separados que conversam por socket e
  arquivos; nenhum código de um entra no binário do outro. A biblioteca `formats/` (C++ + zlib, sem
  dependências do Blender) é o código compartilhável, desde que licenciada de forma compatível pelo autor.
- Ordem: (a) plugin "GameForgeLink" sobre a UE 5.7 instalada, com a ligação ao vivo e o "Portar para Unreal
  Engine 5" de conteúdo (assets, mapas, dados); (b) rebranding e build do fork quando houver disco
  (~250 GB) e o instalador único; (c) port da lógica do jogo (cliente UE falando com o servidor PW), usando
  os códigos-fonte 1.5.5/1.7.8 como referência, em fases.
- "Importar o client e o server": os dados do jogo (pasta `element`, dados do servidor) para edição nativa
  na Engine, e os códigos-fonte como base do port da lógica.

### Ponte

- A Unreal é editor espacial/visual; o Studio continua dono dos arquivos do jogo e só grava ao salvar.
- Transporte: protocolo de Remote Execution do plugin Python da Unreal (multicast `239.0.0.1:6766`, TCP
  `127.0.0.1:6776`), com cliente próprio no Studio (a cópia MIT das BlenderTools, com laço de `recv`; o
  `remote_execution.py` da Unreal é EULA e não entra no fork). Dados grandes vão por arquivos (glTF/FBX, PNG16,
  JSON) numa pasta de troca; o socket só leva comandos curtos.
- Lado Unreal: plugin próprio "GameForgeLink" (MIT), `init_unreal.py` + módulo C++ só para Landscape.
- Coordenadas: Unreal = `100 · (x, −z, y)` a partir do espaço da engine (rotação, sem espelho); terreno
  centrado (`x = −W/2 + col·g`, `z = +H/2 − row·g`).
- Volta: atores marcados `gf:<tipo>:<mapa>:<id>` e metadados `gf.source` são lidos, convertidos e mostrados
  em "Alterações pendentes" antes de gravar. O Send to Unreal original não roda no Blender 5.4
  (`action.fcurves` removido); só o padrão dele é reaproveitado.

## Verificação

- Leitura: todos os pacotes dos clientes 1.5.5, 1.7.6, 1.7.8 e PWNH abrem; ida e volta byte a byte dos
  formatos com gravador em todos os arquivos de um cliente.
- Gravação: só em cópias (o cliente PWU está modificado pelo PWOps); pacotes sintéticos com limite de
  parte reduzido para testar `.pkx`; reabertura com o leitor e com as regras da engine.
- Interface: o Studio real (build de release), não só testes.
