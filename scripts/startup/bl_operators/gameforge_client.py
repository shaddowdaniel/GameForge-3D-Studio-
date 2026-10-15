# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The client library: the files of a game client grouped by package and folder, shown in the Asset
Browser (``FILEBROWSER_PT_gameforge_client_library``), opened in the Studio with everything they
use.
"""

__all__ = (
    "GameForgeLibraryRow",
    "GameForgeLibrarySettings",
    "GAMEFORGE_OT_client_add",
    "GAMEFORGE_OT_client_remove",
    "GAMEFORGE_OT_library_refresh",
    "GAMEFORGE_OT_library_toggle",
    "GAMEFORGE_OT_library_open",
    "GAMEFORGE_OT_library_links",
    "GAMEFORGE_OT_client_save",
    "GAMEFORGE_OT_client_save_undo",
    "GAMEFORGE_OT_library_copy",
)

import os

import bpy
from bpy.props import (
    BoolProperty,
    CollectionProperty,
    EnumProperty,
    IntProperty,
    PointerProperty,
    StringProperty,
)
from bpy.types import (
    Operator,
    PropertyGroup,
)

from gameforge_angelica import anims, clients, library, save, skins, sources

# Rows shown at most, so a huge folder or search does not freeze the interface.
MAX_ROWS = 3000
# Expanded containers and folders, per client folder: {root: {key, ...}}.
_expanded = {}
# Enum items must stay referenced while Blender shows them.
_client_items_cache = []

OPEN_AS_MODEL = {".ecm", ".smd", ".ski", ".bon", ".bmd", ".mox"}
OPEN_AS_TEXT = {".gfx", ".att", ".sgc", ".txt", ".xml", ".ini", ".cfg", ".lua", ".stf", ".dtf", ".htf"}
OPEN_AS_IMAGE = {".dds", ".tga", ".bmp", ".jpg", ".jpeg", ".png"}
OPEN_AS_MAP = {".trn2", ".hmap", ".ecwld"}


def _client_items(_self, _context):
    items = [(c.root, c.name, c.root) for c in clients.load()]
    _client_items_cache[:] = items or [("", "Nenhum cliente", "Adicione a pasta do cliente do jogo")]
    return _client_items_cache


def _settings(context):
    return context.window_manager.gameforge_library


def _active_root(context):
    root = _settings(context).client
    return root if root and os.path.isdir(root) else ""


def _kind_items(_self, _context):
    items = [('ALL', "Todos os tipos", "Mostrar todos os arquivos", 'FILE', 0)]
    for i, (kind, (label, icon, _exts)) in enumerate(library.KINDS.items(), 1):
        items.append((kind, label, "", icon, i))
    return items


def rebuild_rows(context):
    """Fill the visible rows from the expanded tree, or from the search results."""
    settings = _settings(context)
    rows = settings.rows
    rows.clear()
    settings.message = ""
    root = _active_root(context)
    if not root:
        return
    lib = library.get(root)
    kind = None if settings.kind == 'ALL' else settings.kind

    def add(row_type, name, depth, key="", icon='NONE', info="", path="", container=-1, expanded=False):
        row = rows.add()
        row.name = name
        row.row_type = row_type
        row.depth = depth
        row.key = key
        row.icon = icon
        row.info = info
        row.path = path
        row.container = container
        row.expanded = expanded
        return row

    if settings.search.strip():
        results, truncated = lib.search(settings.search.strip(), kind=kind, limit=MAX_ROWS)
        index = {id(c): i for i, c in enumerate(lib.containers)}
        for container, path, size in results:
            add('FILE', path, 0, icon=_file_icon(path), info=library.human_size(size), path=path,
                container=index[id(container)])
        if truncated:
            settings.message = "Mostrando os primeiros {:d} resultados".format(MAX_ROWS)
        elif not results:
            settings.message = "Nada encontrado"
        return

    expanded = _expanded.setdefault(root, set())
    for ci, container in enumerate(lib.containers):
        key = "{:d}:".format(ci)
        is_open = key in expanded
        info = "{:d}".format(len(container.files())) if container.loaded else ""
        add('CONTAINER', container.name, 0, key=key,
            icon='PACKAGE' if container.kind == 'PACKAGE' else 'FILE_FOLDER',
            info=info, container=ci, expanded=is_open)
        if is_open:
            _add_folder(add, expanded, ci, container, container.root_prefix, 1, kind)
        if len(rows) >= MAX_ROWS:
            settings.message = "Lista longa: use a busca para achar um arquivo"
            break


def _add_folder(add, expanded, ci, container, prefix, depth, kind):
    folders, files = container.children(prefix)
    for name, sub_prefix, count in folders:
        key = "{:d}:{:s}".format(ci, sub_prefix)
        is_open = key in expanded
        add('FOLDER', name, depth, key=key, icon='FILE_FOLDER', info="{:d}".format(count),
            container=ci, expanded=is_open)
        if is_open:
            _add_folder(add, expanded, ci, container, sub_prefix, depth + 1, kind)
    for name, path, size in files:
        if kind is not None and library.file_kind(path) != kind:
            continue
        add('FILE', name, depth, icon=_file_icon(path), info=library.human_size(size), path=path, container=ci)


def _file_icon(path):
    kind = library.file_kind(path)
    return library.KINDS[kind][1] if kind in library.KINDS else 'FILE'


def _on_client_changed(_self, context):
    rebuild_rows(context)


def _on_filter_changed(_self, context):
    rebuild_rows(context)


class GameForgeLibraryRow(PropertyGroup):
    row_type: StringProperty()
    key: StringProperty()
    depth: IntProperty()
    icon: StringProperty(default='NONE')
    info: StringProperty()
    path: StringProperty()
    container: IntProperty(default=-1)
    expanded: BoolProperty()


class GameForgeLibrarySettings(PropertyGroup):
    client: EnumProperty(
        name="Cliente",
        description="Cliente do jogo mostrado na biblioteca",
        items=_client_items,
        update=_on_client_changed,
    )
    search: StringProperty(
        name="Buscar",
        description="Buscar arquivos do cliente pelo nome ou caminho",
        update=_on_filter_changed,
    )
    kind: EnumProperty(
        name="Tipo",
        description="Mostrar só um tipo de arquivo",
        items=_kind_items,
        update=_on_filter_changed,
    )
    rows: CollectionProperty(type=GameForgeLibraryRow)
    row_index: IntProperty(name="Arquivo ativo")
    message: StringProperty()


def _active_row(context):
    settings = _settings(context)
    if 0 <= settings.row_index < len(settings.rows):
        return settings.rows[settings.row_index]
    return None


class GAMEFORGE_OT_client_add(Operator):
    """Adicionar um cliente do jogo à biblioteca (a pasta do jogo ou a pasta element)"""
    bl_idname = "gameforge.client_add"
    bl_label = "Adicionar cliente do jogo"

    directory: StringProperty(subtype='DIR_PATH')
    filter_folder: BoolProperty(default=True, options={'HIDDEN'})

    def invoke(self, context, _event):
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        client = clients.add(bpy.path.abspath(self.directory))
        if client is None:
            self.report({'ERROR'}, "Nenhum pacote .pck nesta pasta: escolha a pasta do jogo ou a pasta element")
            return {'CANCELLED'}
        _settings(context).client = client.root
        library.get(client.root, refresh=True)
        rebuild_rows(context)
        self.report({'INFO'}, "Cliente '{:s}' adicionado: veja a biblioteca no Navegador de Assets".format(client.name))
        return {'FINISHED'}


class GAMEFORGE_OT_client_remove(Operator):
    """Tirar o cliente da biblioteca (os arquivos do jogo não são apagados)"""
    bl_idname = "gameforge.client_remove"
    bl_label = "Remover cliente da biblioteca"

    @classmethod
    def poll(cls, context):
        return bool(_active_root(context))

    def execute(self, context):
        clients.remove(_active_root(context))
        settings = _settings(context)
        remaining = clients.load()
        if remaining:
            settings.client = remaining[0].root
        rebuild_rows(context)
        return {'FINISHED'}


class GAMEFORGE_OT_library_refresh(Operator):
    """Ler de novo os pacotes e pastas do cliente"""
    bl_idname = "gameforge.library_refresh"
    bl_label = "Atualizar biblioteca"

    @classmethod
    def poll(cls, context):
        return bool(_active_root(context))

    def execute(self, context):
        import _angelica
        _angelica.release()
        library.get(_active_root(context), refresh=True)
        rebuild_rows(context)
        return {'FINISHED'}


class GAMEFORGE_OT_library_toggle(Operator):
    """Abrir ou fechar esta pasta"""
    bl_idname = "gameforge.library_toggle"
    bl_label = "Abrir/fechar pasta"
    bl_options = {'INTERNAL'}

    index: IntProperty()

    def execute(self, context):
        settings = _settings(context)
        if not 0 <= self.index < len(settings.rows):
            return {'CANCELLED'}
        key = settings.rows[self.index].key
        expanded = _expanded.setdefault(_active_root(context), set())
        if key in expanded:
            expanded.discard(key)
        else:
            expanded.add(key)
        settings.row_index = self.index
        context.window.cursor_set('WAIT')
        try:
            rebuild_rows(context)
        finally:
            context.window.cursor_set('DEFAULT')
        return {'FINISHED'}


def _frame_selected(context):
    for area in context.screen.areas:
        if area.type != 'VIEW_3D':
            continue
        region = next((r for r in area.regions if r.type == 'WINDOW'), None)
        if region is None:
            continue
        with context.temp_override(area=area, region=region):
            bpy.ops.view3d.view_selected()


def _show_in_editor(context, space_type, attribute, data):
    for area in context.screen.areas:
        if area.type == space_type:
            setattr(area.spaces.active, attribute, data)
            return True
    return False


class GAMEFORGE_OT_library_open(Operator):
    """Abrir o arquivo no Studio com tudo o que ele usa (malhas, esqueleto, animações, texturas), \
lido de dentro do pacote, sem extrair nada"""
    bl_idname = "gameforge.library_open"
    bl_label = "Abrir para editar"
    bl_options = {'REGISTER', 'UNDO'}

    path: StringProperty(name="Caminho", description="Caminho do arquivo no cliente (vazio = arquivo ativo)")
    as_text: BoolProperty(
        name="Como texto",
        description="Abrir o próprio arquivo de texto (por exemplo um .ecm) no Editor de Texto",
    )

    @classmethod
    def poll(cls, context):
        row = _active_row(context)
        return bool(_active_root(context)) and row is not None and row.row_type == 'FILE'

    def execute(self, context):
        root = _active_root(context)
        path = self.path or _active_row(context).path
        ext = os.path.splitext(path)[1].lower()
        try:
            if self.as_text or ext in OPEN_AS_TEXT:
                text = sources.open_text(root, path)
                if not _show_in_editor(context, 'TEXT_EDITOR', "text", text):
                    self.report({'INFO'}, "Aberto como texto '{:s}' (veja no Editor de Texto)".format(text.name))
            elif ext in OPEN_AS_MODEL:
                result = bpy.ops.wm.angelica_import_asset(client_root=root, engine_path=path)
                if 'FINISHED' not in result:
                    return {'CANCELLED'}
                # What the meshes look like as imported: saving compares with it to find edits.
                for ob in bpy.data.objects:
                    if skins.PROP_SKIN_FILE in ob and skins.PROP_GEOMETRY not in ob:
                        ob[skins.PROP_GEOMETRY] = skins.mesh_fingerprint(ob)
                for action in bpy.data.actions:
                    if anims.PROP_FILE in action and anims.PROP_KEYS not in action:
                        action[anims.PROP_KEYS] = anims.action_fingerprint(action)
                for ob in bpy.data.objects:
                    if ob.type == 'ARMATURE' and anims.PROP_SKELETON in ob and anims.PROP_REST not in ob:
                        ob[anims.PROP_REST] = anims.rest_fingerprint(ob)
                _frame_selected(context)
            elif ext in OPEN_AS_IMAGE:
                image = sources.open_image(root, path)
                if not _show_in_editor(context, 'IMAGE_EDITOR', "image", image):
                    self.report({'INFO'}, "Aberto como imagem '{:s}' (veja no Editor de Imagem)".format(image.name))
            elif ext in OPEN_AS_MAP:
                parts = path.split("\\")
                if len(parts) < 3 or parts[0].lower() != "maps":
                    self.report({'ERROR'}, "Abra mapas pela pasta maps do cliente")
                    return {'CANCELLED'}
                directory = os.path.join(root, *parts[:2])
                result = bpy.ops.wm.angelica_import_map(directory=directory, client_root=root)
                if 'FINISHED' not in result:
                    return {'CANCELLED'}
            else:
                self.report({'WARNING'}, "Este tipo de arquivo ainda não abre no Studio: " + ext)
                return {'CANCELLED'}
        except (OSError, RuntimeError) as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


# Last list of links, kept for the popup (Python must own the strings).
_links = []


class GAMEFORGE_OT_library_links(Operator):
    """Ver todos os arquivos que este asset usa no cliente (e os que estão faltando)"""
    bl_idname = "gameforge.library_links"
    bl_label = "Vínculos do asset"

    @classmethod
    def poll(cls, context):
        row = _active_row(context)
        return bool(_active_root(context)) and row is not None and row.row_type == 'FILE'

    def invoke(self, context, _event):
        import _angelica
        root = _active_root(context)
        context.window.cursor_set('WAIT')
        try:
            _links[:] = _angelica.dependencies(root, _active_row(context).path)
        finally:
            context.window.cursor_set('DEFAULT')
        return context.window_manager.invoke_popup(self, width=520)

    def draw(self, _context):
        layout = self.layout
        if not _links:
            layout.label(text="Nenhum vínculo encontrado", icon='INFO')
            return
        counts = {}
        missing = []
        for link in _links[1:]:
            counts[link["kind"]] = counts.get(link["kind"], 0) + 1
            if link["location"] is None:
                missing.append(link["path"])
        layout.label(text="{:d} arquivos vinculados".format(len(_links) - 1), icon='LINKED')
        col = layout.column(align=True)
        names = {
            "model": "Modelos", "skin_model": "Modelos de skin", "skin": "Malhas", "skeleton": "Esqueletos",
            "tracks": "Animações", "texture": "Texturas", "effect": "Efeitos", "sound": "Sons",
            "attack": "Ataques", "physics": "Física", "building": "Construções", "other": "Outros",
        }
        for kind, count in sorted(counts.items()):
            col.label(text="{:s}: {:d}".format(names.get(kind, kind), count))
        if missing:
            layout.separator()
            layout.label(text="Faltando no cliente ({:d}):".format(len(missing)), icon='ERROR')
            col = layout.column(align=True)
            for path in missing[:12]:
                col.label(text=path, translate=False)
            if len(missing) > 12:
                col.label(text="... e mais {:d}".format(len(missing) - 12))

    def execute(self, _context):
        return {'FINISHED'}


# Edited files shown by the save dialog (Python must own the strings while it is open).
_pending = []

_KIND_ICONS = {'TEXT': 'TEXT', 'IMAGE': 'IMAGE_DATA', 'SKIN': 'MESH_DATA', 'ANIM': 'ACTION',
               'BON': 'ARMATURE_DATA'}


def _destination(change):
    """Where a change is written: the package (and its client), or "arquivo solto"."""
    kind, container, _mount = save.target(change.root, change.path)
    client = os.path.basename(os.path.dirname(change.root)) or change.root
    return "{:s} ({:s})".format(os.path.basename(container) if kind == 'PACKAGE' else "arquivo solto", client)


def _refresh_libraries(context, roots):
    for root in roots:
        library.get(root, refresh=True)
    rebuild_rows(context)


class GAMEFORGE_OT_client_save(Operator):
    """Gravar no cliente do jogo os arquivos editados no Studio (texturas, textos, ...): dentro dos \
pacotes do próprio cliente, num pacote novo só com as edições, ou numa cópia completa dos pacotes"""
    bl_idname = "gameforge.client_save"
    bl_label = "Salvar no Cliente do Jogo"
    bl_options = {'REGISTER'}

    mode: EnumProperty(
        name="Como salvar",
        items=(
            ('IN_PLACE', "No próprio cliente",
             "Grava dentro dos pacotes e arquivos do cliente; pode ser desfeito depois"),
            ('PATCH', "Pacote só com as edições",
             "Cria numa pasta pacotes novos (.pck) com apenas os arquivos editados"),
            ('COPY', "Cópia completa dos pacotes",
             "Cria numa pasta cópias completas dos pacotes tocados, com os arquivos editados"),
        ),
    )
    directory: StringProperty(
        name="Pasta",
        description="Pasta onde os pacotes novos são criados (com as pastas do cliente)",
        subtype='DIR_PATH',
    )

    def invoke(self, context, _event):
        _pending[:] = save.pending_changes()
        if not _pending:
            self.report({'INFO'}, "Nenhum arquivo do cliente foi editado")
            return {'CANCELLED'}
        return context.window_manager.invoke_props_dialog(self, width=600, confirm_text="Salvar")

    def draw(self, _context):
        layout = self.layout
        layout.use_property_split = False
        layout.prop(self, "mode", expand=True)
        if self.mode == 'IN_PLACE':
            layout.label(text="Feche o jogo e as ferramentas que usam o cliente antes de salvar.", icon='INFO')
        else:
            layout.prop(self, "directory")
        box = layout.box()
        box.label(text="{:d} arquivo(s) editado(s):".format(len(_pending)), icon='FILE_TICK')
        col = box.column(align=True)
        for change in _pending[:16]:
            row = col.row()
            row.label(text=change.path, icon=_KIND_ICONS.get(change.kind, 'FILE'), translate=False)
            sub = row.row()
            sub.alignment = 'RIGHT'
            sub.enabled = False
            sub.label(text=_destination(change), translate=False)
            if change.warning:
                col.label(text="    " + change.warning, icon='ERROR', translate=False)
        if len(_pending) > 16:
            col.label(text="... e mais {:d}".format(len(_pending) - 16), translate=False)

    def execute(self, context):
        changes = list(_pending) or save.pending_changes()
        _pending.clear()
        if not changes:
            self.report({'INFO'}, "Nenhum arquivo do cliente foi editado")
            return {'CANCELLED'}
        context.window.cursor_set('WAIT')
        try:
            written = save.save(changes, self.mode, bpy.path.abspath(self.directory))
        except save.SaveError as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        finally:
            context.window.cursor_set('DEFAULT')
        if self.mode == 'IN_PLACE':
            _refresh_libraries(context, {change.root for change in changes})
            self.report({'INFO'}, "{:d} arquivo(s) salvo(s) no cliente ({:d} pacote(s)/arquivo(s) gravados)".format(
                len(changes), len(written)))
        else:
            self.report({'INFO'}, "{:d} arquivo(s) salvo(s) em {:s}".format(len(written), self.directory))
        return {'FINISHED'}


class GAMEFORGE_OT_client_save_undo(Operator):
    """Restaurar os pacotes e arquivos do cliente como estavam antes do último salvamento no cliente"""
    bl_idname = "gameforge.client_save_undo"
    bl_label = "Desfazer Último Salvamento no Cliente"
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, _context):
        return save.can_undo()

    def invoke(self, context, event):
        return context.window_manager.invoke_confirm(
            self, event,
            title="Desfazer último salvamento no cliente",
            message="Os pacotes e arquivos voltam a ser como eram antes do último salvamento.",
            confirm_text="Desfazer",
        )

    def execute(self, context):
        try:
            problems = save.undo_last_save()
        except save.SaveError as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        _refresh_libraries(context, {client.root for client in clients.load()})
        if problems:
            self.report({'WARNING'}, "Desfeito em parte: " + "; ".join(problems))
        else:
            self.report({'INFO'}, "Último salvamento no cliente desfeito")
        return {'FINISHED'}


# Plan of the copy shown by the dialog: (source root, path, target root, items).
_copy_plan = [None]
_target_items_cache = []


def _target_items(_self, context):
    active = _active_root(context)
    items = [(c.root, c.name, c.root) for c in clients.load()
             if os.path.normcase(c.root) != os.path.normcase(active)]
    _target_items_cache[:] = items or [("", "Nenhum outro cliente", "Adicione outro cliente do jogo")]
    return _target_items_cache


def _plan_copy(op, context):
    from gameforge_angelica import transfer
    row = _active_row(context)
    if row is None or not op.target:
        _copy_plan[0] = None
        return
    context.window.cursor_set('WAIT')
    try:
        root = _active_root(context)
        _copy_plan[0] = (root, row.path, op.target, transfer.plan(root, row.path, op.target))
    finally:
        context.window.cursor_set('DEFAULT')


class GAMEFORGE_OT_library_copy(Operator):
    """Copiar este asset, com todos os arquivos que ele usa (malhas, esqueleto, animações, \
texturas, efeitos, sons), para outro cliente do jogo"""
    bl_idname = "gameforge.library_copy"
    bl_label = "Copiar para Outro Cliente"
    bl_options = {'REGISTER'}

    target: EnumProperty(
        name="Cliente de destino",
        items=_target_items,
        update=lambda self, context: _plan_copy(self, context),
    )
    replace_different: BoolProperty(
        name="Substituir os arquivos diferentes",
        description="Quando o destino já tem um arquivo com o mesmo caminho e outro conteúdo, "
        "gravar o da origem por cima (senão o do destino fica)",
    )
    include_unsupported: BoolProperty(
        name="Copiar também versões mais novas",
        description="Copiar mesmo os arquivos numa versão mais nova do que o cliente de destino usa "
        "(ele pode não conseguir abri-los)",
    )
    mode: EnumProperty(
        name="Como salvar",
        items=(
            ('IN_PLACE', "No cliente de destino", "Grava dentro dos pacotes do cliente de destino; pode ser desfeito"),
            ('PATCH', "Pacote só com a cópia", "Cria numa pasta pacotes novos só com os arquivos copiados"),
            ('COPY', "Cópia completa dos pacotes", "Cria numa pasta cópias completas dos pacotes do destino com a cópia"),
        ),
    )
    directory: StringProperty(name="Pasta", subtype='DIR_PATH')

    @classmethod
    def poll(cls, context):
        row = _active_row(context)
        return bool(_active_root(context)) and row is not None and row.row_type == 'FILE' and len(clients.load()) > 1

    def invoke(self, context, _event):
        _plan_copy(self, context)
        return context.window_manager.invoke_props_dialog(self, width=620, confirm_text="Copiar")

    def draw(self, _context):
        layout = self.layout
        layout.prop(self, "target")
        plan = _copy_plan[0]
        if plan is None:
            layout.label(text="Escolha o cliente de destino", icon='INFO')
            return
        items = plan[3]
        by_status = {}
        for item in items:
            by_status.setdefault(item.status, []).append(item)
        box = layout.box()
        col = box.column(align=True)
        new = by_status.get('NEW', [])
        col.label(text="{:d} arquivo(s) novos para o destino ({:s})".format(
            len(new), library.human_size(sum(i.size for i in new))), icon='ADD')
        col.label(text="{:d} já estão iguais no destino".format(len(by_status.get('SAME', []))), icon='CHECKMARK')
        different = by_status.get('DIFFERENT', [])
        if different:
            col.label(text="{:d} existem no destino com outro conteúdo".format(len(different)), icon='ERROR')
            for item in different[:4]:
                col.label(text="    " + item.path, translate=False)
        converted = [i for i in items if i.convert]
        if converted:
            col.label(text="{:d} convertidos para versões que o destino lê".format(len(converted)), icon='FILE_REFRESH')
        unsupported = [i for i in items if i.unsupported]
        if unsupported:
            col.label(text="{:d} numa versão mais nova do que o destino usa:".format(len(unsupported)), icon='CANCEL')
            for item in unsupported[:4]:
                col.label(text="    {:s} ({:s})".format(item.path, item.note), translate=False)
        missing = by_status.get('MISSING', [])
        if missing:
            col.label(text="{:d} faltam no cliente de origem (não vão)".format(len(missing)), icon='QUESTION')
        if different:
            layout.prop(self, "replace_different")
        if unsupported:
            layout.prop(self, "include_unsupported")
        layout.prop(self, "mode", expand=True)
        if self.mode == 'IN_PLACE':
            layout.label(text="Feche o jogo de destino antes de copiar.", icon='INFO')
        else:
            layout.prop(self, "directory")

    def execute(self, context):
        from gameforge_angelica import transfer
        plan = _copy_plan[0]
        if plan is None:
            self.report({'ERROR'}, "Escolha o cliente de destino")
            return {'CANCELLED'}
        src_root, path, dst_root, items = plan
        context.window.cursor_set('WAIT')
        try:
            written = transfer.copy(src_root, dst_root, items, self.replace_different,
                                    self.include_unsupported, self.mode, bpy.path.abspath(self.directory))
        except save.SaveError as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        finally:
            context.window.cursor_set('DEFAULT')
        if not written:
            self.report({'INFO'}, "Nada para copiar: o destino já tem tudo")
            return {'FINISHED'}
        if self.mode == 'IN_PLACE':
            _refresh_libraries(context, {dst_root})
        self.report({'INFO'}, "'{:s}' copiado ({:d} pacote(s)/arquivo(s) gravados)".format(
            os.path.basename(path), len(written)))
        return {'FINISHED'}


classes = (
    GameForgeLibraryRow,
    GameForgeLibrarySettings,
    GAMEFORGE_OT_client_add,
    GAMEFORGE_OT_client_remove,
    GAMEFORGE_OT_library_refresh,
    GAMEFORGE_OT_library_toggle,
    GAMEFORGE_OT_library_open,
    GAMEFORGE_OT_library_links,
    GAMEFORGE_OT_client_save,
    GAMEFORGE_OT_client_save_undo,
    GAMEFORGE_OT_library_copy,
)


def _show_library_on_start():
    wm = bpy.context.window_manager
    if wm is not None and not wm.gameforge_library.rows and _active_root(bpy.context):
        rebuild_rows(bpy.context)
    return None


def register():
    # The classes are registered by `bl_operators` (from `classes`), before this runs.
    bpy.types.WindowManager.gameforge_library = PointerProperty(type=GameForgeLibrarySettings)
    bpy.app.timers.register(_show_library_on_start, first_interval=1.0, persistent=True)


def unregister():
    del bpy.types.WindowManager.gameforge_library
