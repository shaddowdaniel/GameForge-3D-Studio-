Roadmap do GameForge 3D Studio
==============================

Status: ⬜ não iniciado · 🟨 em andamento · ✅ feito

## 1. Base do fork

- ✅ README e roadmap próprios do GameForge.
- 🟨 Sincronização automática com o `blender/blender` por PR semanal (`.github/workflows/sync-upstream.yml`).

## 2. Suporte nativo à Angelica Engine

Tudo implementado no núcleo (C++), sem add-on, aparecendo no menu **File › Import** e na **Asset Library**.

- 🟨 Leitura de pacotes `.pck`: listar os arquivos do pacote e extrair sob demanda.
- ⬜ Importação em lote: escolher arquivos da lista do `.pck` e importar todos de uma vez como assets.
- ⬜ Modelos e esqueletos (`.ski`, `.smd`, `.bon`, `.ecm`) com materiais e texturas (`.dds`).
- ⬜ Terreno e mapa de altura (`.hmap`, `.trn2` e blocos de terreno).
- ⬜ Importação de mapa pelo código: selecionar a pasta do mapa (ex.: `maps/a26`) importa terreno,
  objetos e vegetação como uma coleção de assets.
- ⬜ Compatibilidade com as versões de cliente 1.5.5 e 1.7.8 (e outras, conforme surgirem diferenças de formato).

## 3. Exportação de volta para o cliente

- ⬜ Exportar modelos e terrenos editados para os formatos da Angelica Engine.
- ⬜ Regravar arquivos dentro de um `.pck`.

## 4. Controle por agentes de IA

- ⬜ Servidor MCP para pilotar o GameForge (modelar assets, mapas e interfaces) a partir de Claude e outros agentes.
