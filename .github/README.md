<!--
Mantenha este documento curto. Detalhes de planejamento ficam em ROADMAP.md.
O read-me para usuários finais do Blender está em 'release/text/readme.html'.
-->

GameForge 3D Studio
===================

O GameForge 3D Studio é um fork do [Blender](https://www.blender.org) voltado à criação e edição de conteúdo
para jogos feitos na **Angelica Engine** (Perfect World e derivados).

A meta é abrir os arquivos do cliente diretamente, sem plugins e sem conversores externos:
pacotes `.pck`, modelos, terrenos e mapas entram nativamente na biblioteca de assets, prontos para editar.

O que muda em relação ao Blender
--------------------------------

- **Suporte nativo à Angelica Engine.** Importação de `.pck`, `.hmap`, terrenos e demais formatos do cliente
  direto para a Asset Library, com leitura da lista de arquivos e importação em lote.
- **Importação de mapas pelo código.** Selecionar a pasta de um mapa (por exemplo `world`, `a26`) importa o mapa
  inteiro como assets.
- **Acompanha o Blender.** Um workflow semanal traz as mudanças do `blender/blender` e abre um PR,
  para que as correções do upstream continuem chegando.

O plano completo e o status de cada item estão em [ROADMAP.md](ROADMAP.md).

Referências de formato
----------------------

Os formatos são estudados a partir dos sources dos clientes 1.5.5 e 1.7.8 mantidos pelo autor do projeto.

Desenvolvimento
---------------

O código-base é o do Blender, então as instruções de build são as mesmas:

- [Como compilar](https://developer.blender.org/docs/handbook/building_blender/)
- [Documentação para desenvolvedores do Blender](https://developer.blender.org/docs/)

Mudanças específicas do GameForge são feitas por pull request neste repositório.
A sincronização com o Blender é feita pelo workflow `.github/workflows/sync-upstream.yml`.

Licença
-------

Como o Blender, o GameForge 3D Studio é licenciado sob a GNU General Public License, versão 3.
Arquivos individuais podem ter licenças diferentes, porém compatíveis.

Veja [blender.org/about/license](https://www.blender.org/about/license) para detalhes.
