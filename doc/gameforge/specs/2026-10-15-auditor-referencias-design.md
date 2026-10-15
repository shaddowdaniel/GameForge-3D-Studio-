# Auditor de referências do cliente — design

Data: 2026-10-15. Pedido do usuário: checar o `elements.data` contra os pacotes (modelos, texturas,
surfaces/ícones, sfx, gfx...), mostrar o que falta num sino de notificações com link direto para cada
objeto e deixar criar o que falta clonando um arquivo existente com o nome que falta (e editar o clone).
Exemplo dado: as "Ashen Wings" do PWU, cujo registro aponta para um `.ecm` que não existe e um `.gfx`
que existe.

## Objetivo

Achar, num cliente aberto na Biblioteca do Cliente, as referências que apontam para arquivos que não
existem nos pacotes — as que causam "reportbug" (crash), modelos invisíveis, efeitos e ícones faltando —
e resolver cada uma a partir do próprio Studio, com o fluxo de salvamento de sempre (com desfazer).

Escopo decidido: os arquivos citados nos registros do `elements.data` **e** o que cada modelo citado usa
(o `.ecm` e o que ele chama: `.smd`, `.ski`, `.bon`, `.stck`, texturas, `.gfx`, `.att`, `.sfx`...).

## Medições (protótipo descartável, 2026-10-15)

- Varredura dos campos de arquivo do `elements.data` com `data.LinkRules` + `_angelica.locate` + atlas de
  ícones (`icons.IconAtlas`): 17 s no 1.7.8 (57.497 ícones, 33.590 `file_matter`, 29.837 `file_model`...),
  8 s no PWU, 13 s no PWNH.
- Faltando no 1.7.8: 463 `file_model`, 387 `file_matter`, 147 `file_gfx_short`, 112/55 modelos de arma
  (direita/esquerda), 57 cabelos, 96+44 texturas de rosto; no PWU: 56 `file_model`, 212 `file_matter`, 11
  `file_gfx_short_hit`. Exemplos reais: o "Armamento de Festival 2017" aponta para
  `2015糖果单手短.ecm`, que não existe; o "Police baton" para `trees\weapons_2022\...\baton.ecm`;
  tinturas com `file_icon` = um `.ecm`.
- Falsos positivos a tratar: `tag_path` (não é arquivo), o texto `原模板未配置该项` ("template não
  configurado"), `realname` do `FASHION_NEW_ESSENCE` (conjunto de skins, como em `EQUIPMENT_LISTS`) e
  ícones com caminho completo em `surfaces\` (imagens soltas, não entradas do atlas).
- Dependências dos modelos (`_angelica.dependencies`): 24 ms por modelo; 13.465 modelos únicos no 1.7.8
  ≈ 5,5 min. Numa amostra de 400, 132 modelos usam 477 arquivos que não existem (`.att` de golpes,
  `.mphy` de física, `.gfx` de montarias, `.ski` de NPCs).

## O que é um problema

| Tipo | Exemplo | Gravidade |
|------|---------|-----------|
| Modelo citado não existe (`.ecm`, `.smd`, `.bmd`) | `file_model`, `file_matter`, `file_model_left/right` | Erro |
| Arquivo que um modelo usa não existe: malha, esqueleto ou animação (`.ski`, `.smd`, `.bon`, `.stck`) | `侏儒卫兵.ski` | Erro |
| Campo com o tipo de arquivo errado | `file_icon` com um `.ecm` | Erro |
| Efeito, som ou golpe não existe (`.gfx`, `.sfx`/`.wav`, `.att`) | `file_gfx_short`, `.gfx` dentro do `.ecm` | Aviso |
| Textura não existe (`.dds`, `.tga`, `.bmp`) | `file_base_tex`, textura de uma skin | Aviso |
| Ícone não está no atlas nem como imagem solta | `file_icon` | Aviso |
| Conjunto de skins de armadura/fashion sem as skins (`realname`) | `transfer.equipment_skins` vazio | Aviso |
| Arquivo opcional ausente (`.mphy`) | física de um modelo | Informação (escondido por padrão) |

A gravidade de cada extensão é conferida no código do cliente durante a implementação (o que o loader
faz quando não acha o arquivo); a tabela é o ponto de partida.

Um problema guarda: tipo, gravidade, caminho como está escrito, caminho resolvido, lista + id + nome do
registro e campo (ou o modelo que usa o arquivo, para dependências), e quantos registros citam o mesmo
caminho (um `.ecm` faltando pode afetar dezenas de registros: aparece uma vez, com a lista deles).

## Como o Studio acha

- `gameforge_angelica/audit.py` (sem `bpy`): `scan(client_root, progress)` percorre as listas do
  `elements.data` com `data.ClientData` e `data.LinkRules` (campos de arquivo), resolve cada valor com uma
  função única `resolve(lista, campo, valor)` (prefixos `gfx\`, `sfx\`, `surfaces\` conforme o campo;
  marcadores e campos que não são arquivo ficam de fora), confere ícones nos atlas (`icons.IconAtlas`) e
  imagens soltas em `surfaces\`, os conjuntos de skins com `transfer.equipment_skins`, e cada modelo único
  com `_angelica.dependencies`.
- Roda numa thread (o Studio continua livre), com progresso no sino; os modelos são verificados uma vez
  cada (o mesmo `.ecm` citado por 50 registros custa um `dependencies`).
- Cache por cliente no diretório de configuração do Studio: o resultado e as dependências por modelo,
  invalidados quando o `elements.data` ou um pacote muda (tamanho + data). A segunda abertura do mesmo
  cliente mostra o sino na hora; depois de salvar no cliente, só os caminhos tocados são conferidos de novo.
- Os falsos positivos medidos viram regras de `resolve` com teste; se um campo novo aparecer só com
  "faltando", o teste dos três clientes acusa.

## Sino e lista

- Sino na barra superior, à direita (ao lado da conta), com o número de erros + avisos do cliente ativo e
  "verificando 37%" enquanto roda. Ícone próprio (PNG nos `datafiles` do Studio); vermelho com erros,
  laranja só com avisos.
- Clique abre um popover: filtros por gravidade e tipo, busca, e a lista agrupada por caminho faltante
  (com os registros que o citam). Botão "Verificar de novo" e "Exportar relatório" (CSV), útil para quem
  mantém o servidor.
- Cada item tem links diretos:
  - **Abrir registro**: o editor de registros já existente (`gameforge.record_open` com lista e id), no
    campo que aponta para o arquivo;
  - **Abrir o modelo**: para dependências, abre o modelo que usa o arquivo (`wm.angelica_import_asset`);
  - **Mostrar na biblioteca**: abre a Biblioteca do Cliente na pasta onde o arquivo deveria estar (a mais
    próxima que existe);
  - **Criar a partir de um existente...** (abaixo);
  - **Ignorar**: guardado por cliente (para referências que o servidor sabe que não usa).

## Criar o que falta

- "Criar a partir de um existente..." abre a busca da biblioteca filtrada pelo tipo do arquivo que falta
  (modelos para `.ecm`, efeitos para `.gfx`...), já procurando pelo nome; o usuário escolhe a origem.
- Modelos (`.ecm`):
  - **Clone simples** (padrão): copia o `.ecm` escolhido com o nome que falta; ele continua usando as
    malhas, skins e animações da origem (o jogo já carrega). Editar malha ou textura do clone mexeria na
    origem: o Studio avisa e oferece o clone completo;
  - **Clone completo**: copia o `.ecm` e tudo o que ele usa para a pasta do nome que falta, com os
    caminhos internos reescritos (o mesmo plano de dependências da cópia entre clientes,
    `transfer.plan`, com outra pasta de destino). Assim o clone é editável sem tocar na origem.
- `.gfx`, `.att`, sons e texturas: cópia do arquivo com o nome que falta.
- Ícones: a imagem do ícone escolhido entra nos atlas com o nome que falta (`icons.IconAtlas.add`, já usado
  na cópia entre clientes).
- Tudo vira alteração pendente do "Salvar no cliente do jogo" (os três modos: pacote de patch, no lugar,
  cópia completa; com desfazer) e o clone abre no Studio para editar. Depois de salvar, o problema some
  do sino (conferência só dos caminhos tocados).

## Alternativas consideradas

1. **Python sobre os bindings que já existem** (escolhida): `locate`, `dependencies`, `icons`, `data` e o
   salvamento já estão prontos e testados; 17 s + 5,5 min medidos, com cache e thread.
2. **Motor de auditoria em C++** (`_angelica.audit`): mais rápido, mas duplica as regras de campo e de
   ícone que estão em Python. Só se a medição real ficar ruim; a fronteira (`audit.scan`) permite trocar
   depois.
3. **Relatório fora do Studio** (HTML/CSV por linha de comando): sem links diretos e sem criar o que
   falta; fica como o "Exportar relatório" do popover.

## Testes

- Mini cliente sintético (como os dos testes de gravação) com um registro que aponta para um `.ecm` que
  não existe e um `.gfx` que existe (o caso "Ashen Wings"), um ícone fora do atlas e um modelo que usa um
  `.ski` ausente: `scan` acha exatamente esses problemas, com as gravidades certas.
- Clone simples e completo: depois de "Criar a partir de um existente" e salvar numa cópia, o problema
  some; no completo, os caminhos internos apontam para a pasta nova e a origem não muda (sha1).
- Falsos positivos: nos três clientes de teste (só leitura), nenhum problema de campos que não são
  arquivo (`tag_path`, marcadores, `realname` de conjuntos) e o número de problemas por campo fica
  registrado no teste (uma mudança grande acusa regra quebrada).
- Interface: o verificador de painéis (`ui_check.py`) desenha o sino e o popover com problemas.

## Fora do escopo (por agora)

- Referências do `tasks.data`, `gshop.data`, `npcgen.data` e da cena dos mapas (vêm depois, com a mesma
  estrutura de problema).
- Correção automática sem o usuário escolher a origem.
- Notificações de outros assuntos no sino (atualizações, salvamentos).
