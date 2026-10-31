"""The achievements editor: a window to make, change and order the achievements of an install folder.

    pythonw achievements_editor.py --dest <install folder>

It edits <dest>/achievements/achievements.json (tools/achievements.py describes the files), draws each icon (an
imported picture in the rarity's frame, or one of the emblems), shows the notice as the game draws it, and "Salvar e
aplicar" writes everything and compiles it for the game, which reads it at its next start. The setup puts a shortcut to
it in the install folder and the Start menu. Texts are Brazilian Portuguese, as the achievements are.

Pictures: whatever Tk opens (PNG, GIF, PPM, SVG); JPEG and BMP (the game's album photos are JPEG) go through Windows'
own decoder first (System.Drawing, through PowerShell). A picture is kept at up to 512 pixels in
achievements/sources/<id>.png, so its zoom and framing can change later.

Standard library only (tkinter).
"""
import argparse
import copy
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import threading
import tkinter as tk
import unicodedata
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, str(Path(__file__).resolve().parent))
import achievements as A   # noqa: E402

BG, PANEL, GOLD, ZONAI, CREAM, MUTED = '#0b1311', '#121c19', '#d8bf7e', '#5ef2c8', '#ece6d6', '#a9a594'
RARITY_HEX = {'common': '#c79e66', 'rare': '#78ccd6', 'epic': '#b88af2', 'legendary': '#f2c962', 'platinum': '#edf2fa'}
KINDS = [('preset:' + k, v['label']) for k, v in A.PRESETS.items()] + [
    ('quest', 'Missão concluída'), ('flag', 'Marco da história'), ('all', 'Todas as outras conquistas (platina)')]
SOURCE_SIDE = 512
PREVIEW_DELAY_MS = 120


def slug(text):
    s = unicodedata.normalize('NFKD', text).encode('ascii', 'ignore').decode().lower()
    return re.sub(r'[^a-z0-9]+', '_', s).strip('_')[:40] or 'conquista'


def to_png(src, dst):
    """Any picture to a PNG of at most SOURCE_SIDE pixels on its long side, through Tk or Windows' decoder."""
    tmp = None
    try:
        img = tk.PhotoImage(file=str(src))
    except tk.TclError:
        if os.name != 'nt':
            raise
        tmp = Path(tempfile.mkdtemp()) / 'in.png'
        script = ('Add-Type -AssemblyName System.Drawing; $i = [System.Drawing.Image]::FromFile($env:TOTK_SRC); '
                  '$i.Save($env:TOTK_DST, [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose()')
        subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', script], check=True,
                       env=dict(os.environ, TOTK_SRC=str(src), TOTK_DST=str(tmp)),
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0), timeout=60)
        img = tk.PhotoImage(file=str(tmp))
    k = max(1, math.ceil(max(img.width(), img.height()) / SOURCE_SIDE))
    if k > 1:
        img = img.subsample(k, k)
    Path(dst).parent.mkdir(parents=True, exist_ok=True)
    img.write(str(dst), format='png')
    if tmp:
        tmp.unlink(missing_ok=True)


class Editor:
    def __init__(self, root, dest):
        self.root, self.dest = root, Path(dest)
        self.folder = self.dest / 'achievements'
        self.path = self.folder / 'achievements.json'
        if not self.path.is_file():
            A.write_defaults(self.dest)
        self.defs = json.loads(self.path.read_text(encoding='utf-8'))
        self.items = self.defs['achievements']
        self.dirty_icons = set()
        self.thumbs, self.sel, self.loading = {}, None, False
        self.pending = None
        self.preview_img = None
        root.title('Editor de Conquistas — TotK Recomp')
        root.configure(bg=BG)
        root.geometry('1180x760')
        root.minsize(980, 640)
        self.style()
        self.build()
        self.refresh_list()
        if self.items:
            self.tree.selection_set(self.items[0]['id'])

    # ---- look

    def style(self):
        s = ttk.Style(self.root)
        s.theme_use('clam')
        s.configure('.', background=BG, foreground=CREAM, fieldbackground=PANEL, bordercolor='#2b3a35',
                    font=('Segoe UI', 10))
        s.configure('TFrame', background=BG)
        s.configure('Panel.TFrame', background=PANEL)
        s.configure('TLabel', background=BG, foreground=CREAM)
        s.configure('Panel.TLabel', background=PANEL)
        s.configure('Muted.TLabel', background=PANEL, foreground=MUTED, font=('Segoe UI', 9))
        s.configure('Title.TLabel', background=BG, foreground=GOLD, font=('Segoe UI Semibold', 16))
        s.configure('TButton', background='#1b2925', foreground=CREAM, padding=(10, 5))
        s.map('TButton', background=[('active', '#24453c')])
        s.configure('Accent.TButton', background='#1f5a4b', foreground='#e9fff8')
        s.map('Accent.TButton', background=[('active', '#2a7a66')])
        s.configure('TCheckbutton', background=PANEL, foreground=CREAM)
        s.configure('Treeview', background=PANEL, fieldbackground=PANEL, foreground=CREAM, rowheight=40,
                    font=('Segoe UI', 10))
        s.map('Treeview', background=[('selected', '#24453c')])
        s.configure('TCombobox', fieldbackground=PANEL, background='#1b2925', foreground=CREAM, arrowcolor=CREAM)
        s.map('TCombobox', fieldbackground=[('readonly', PANEL)], foreground=[('readonly', CREAM)],
              selectbackground=[('readonly', PANEL)], selectforeground=[('readonly', CREAM)])
        s.configure('TSpinbox', fieldbackground=PANEL, foreground=CREAM, arrowcolor=CREAM, background='#1b2925')
        s.configure('TEntry', fieldbackground=PANEL, foreground=CREAM, insertcolor=CREAM)
        s.configure('Horizontal.TScale', background='#24453c', troughcolor='#1b2925', bordercolor='#2b3a35',
                    lightcolor='#2a7a66', darkcolor='#1f5a4b')
        self.root.option_add('*TCombobox*Listbox.background', PANEL)
        self.root.option_add('*TCombobox*Listbox.foreground', CREAM)

    # ---- layout

    def build(self):
        top = ttk.Frame(self.root, padding=(16, 12, 16, 4))
        top.pack(fill='x')
        ttk.Label(top, text='Conquistas', style='Title.TLabel').pack(side='left')
        self.status = ttk.Label(top, text='', foreground=MUTED)
        self.status.pack(side='left', padx=16)
        ttk.Button(top, text='Salvar e aplicar', style='Accent.TButton', command=self.save).pack(side='right')
        ttk.Button(top, text='Restaurar padrão', command=self.restore).pack(side='right', padx=8)

        body = ttk.Frame(self.root, padding=(16, 8, 16, 16))
        body.pack(fill='both', expand=True)
        left = ttk.Frame(body)
        left.pack(side='left', fill='y')
        self.tree = ttk.Treeview(left, columns=('rarity',), show='tree headings', selectmode='browse', height=14)
        self.tree.heading('#0', text='Conquista')
        self.tree.heading('rarity', text='Raridade')
        self.tree.column('#0', width=300)
        self.tree.column('rarity', width=90, anchor='center')
        bar = ttk.Frame(left)
        bar.pack(side='bottom', fill='x', pady=(8, 0))
        scroll = ttk.Scrollbar(left, orient='vertical', command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)
        scroll.pack(side='right', fill='y')
        self.tree.pack(side='left', fill='y', expand=True)
        self.tree.bind('<<TreeviewSelect>>', lambda e: self.select())
        for text, cmd in (('Nova', self.new), ('Duplicar', self.duplicate), ('Excluir', self.delete),
                          ('↑', lambda: self.move(-1)), ('↓', lambda: self.move(1))):
            ttk.Button(bar, text=text, command=cmd, width=8 if len(text) > 1 else 3).pack(side='left', padx=(0, 4))

        right = ttk.Frame(body, style='Panel.TFrame', padding=16)
        right.pack(side='left', fill='both', expand=True, padx=(16, 0))
        side = ttk.Frame(right, style='Panel.TFrame')   # the icon column first, so the form takes what is left
        side.pack(side='right', fill='y', padx=(20, 0))
        form = ttk.Frame(right, style='Panel.TFrame')
        form.pack(side='left', fill='both', expand=True)
        self.v_name, self.v_rarity, self.v_hidden = tk.StringVar(), tk.StringVar(), tk.BooleanVar()
        self.v_kind, self.v_min, self.v_quest, self.v_flag = tk.StringVar(), tk.StringVar(), tk.StringVar(), tk.StringVar()
        self.v_emblem, self.v_zoom, self.v_dx, self.v_dy = tk.StringVar(), tk.DoubleVar(), tk.DoubleVar(), tk.DoubleVar()

        def row(label):
            ttk.Label(form, text=label, style='Muted.TLabel').pack(anchor='w', pady=(10, 2))
        row('Nome')
        ttk.Entry(form, textvariable=self.v_name, font=('Segoe UI', 11)).pack(fill='x')
        row('Descrição')
        self.t_desc = tk.Text(form, height=3, wrap='word', bg=PANEL, fg=CREAM, insertbackground=CREAM,
                              relief='flat', highlightthickness=1, highlightbackground='#2b3a35', font=('Segoe UI', 10))
        self.t_desc.pack(fill='x')
        line = ttk.Frame(form, style='Panel.TFrame')
        line.pack(fill='x', pady=(10, 0))
        ttk.Label(line, text='Raridade', style='Muted.TLabel').pack(side='left')
        self.c_rarity = ttk.Combobox(line, state='readonly', width=14,
                                     values=[A.RARITY_LABEL[r] for r in A.RARITIES], textvariable=self.v_rarity)
        self.c_rarity.pack(side='left', padx=8)
        ttk.Checkbutton(line, text='Secreta (esconde nome e descrição até desbloquear)',
                        variable=self.v_hidden).pack(side='left', padx=12)
        row('Condição')
        self.c_kind = ttk.Combobox(form, state='readonly', values=[label for _, label in KINDS],
                                   textvariable=self.v_kind)
        self.c_kind.pack(fill='x')
        self.c_kind.bind('<<ComboboxSelected>>', lambda e: self.kind_changed())
        self.param = ttk.Frame(form, style='Panel.TFrame')
        self.param.pack(fill='x', pady=(6, 0))
        self.l_param = ttk.Label(self.param, text='', style='Muted.TLabel')
        self.s_min = ttk.Spinbox(self.param, from_=1, to=999999, textvariable=self.v_min, width=10)
        self.c_quest = ttk.Combobox(self.param, values=[f'{v}  [{k}]' for k, v in A.QUESTS.items()],
                                    textvariable=self.v_quest)
        self.c_flag = ttk.Combobox(self.param, state='readonly', values=list(A.FLAGS.values()), textvariable=self.v_flag)
        self.l_hint = ttk.Label(form, text='', style='Muted.TLabel', wraplength=420, justify='left')
        self.l_hint.pack(anchor='w', pady=(4, 0))

        # the icon and the notice
        ttk.Label(side, text='Ícone', style='Muted.TLabel').pack(anchor='w')
        self.cv_icon = tk.Canvas(side, width=160, height=160, bg=PANEL, highlightthickness=0)
        self.cv_icon.pack(pady=4)
        ib = ttk.Frame(side, style='Panel.TFrame')
        ib.pack(fill='x')
        ttk.Button(ib, text='Importar imagem…', command=self.import_picture).pack(side='left')
        ttk.Button(ib, text='Usar emblema', command=self.use_emblem).pack(side='left', padx=6)
        em = ttk.Frame(side, style='Panel.TFrame')
        em.pack(fill='x', pady=(8, 0))
        ttk.Label(em, text='Emblema', style='Muted.TLabel').pack(side='left')
        self.c_emblem = ttk.Combobox(em, state='readonly', values=A.EMBLEMS, textvariable=self.v_emblem, width=12)
        self.c_emblem.pack(side='left', padx=8)
        self.c_emblem.bind('<<ComboboxSelected>>', lambda e: self.icon_changed())
        for label, var, lo, hi in (('Zoom', self.v_zoom, 1.0, 4.0), ('Horizontal', self.v_dx, -1.0, 1.0),
                                   ('Vertical', self.v_dy, -1.0, 1.0)):
            f = ttk.Frame(side, style='Panel.TFrame')
            f.pack(fill='x', pady=(6, 0))
            ttk.Label(f, text=label, style='Muted.TLabel', width=10).pack(side='left')
            ttk.Scale(f, from_=lo, to=hi, variable=var, command=lambda _v: self.icon_changed()).pack(side='left',
                                                                                                   fill='x', expand=True)
        ttk.Label(side, text='Aviso no jogo', style='Muted.TLabel').pack(anchor='w', pady=(18, 4))
        self.cv_note = tk.Canvas(side, width=420, height=96, bg='#3a4a44', highlightthickness=0)
        self.cv_note.pack()
        for v in (self.v_name, self.v_rarity, self.v_hidden, self.v_min, self.v_quest, self.v_flag):
            v.trace_add('write', lambda *a: self.form_changed())
        self.t_desc.bind('<<Modified>>', self.desc_changed)

    # ---- list

    def thumb(self, a):
        png = self.folder / 'icons' / f'{a["id"]}.png'
        if a['id'] in self.dirty_icons or not png.is_file():
            self.render_icon(a)
        try:
            img = tk.PhotoImage(file=str(png)).subsample(4, 4)
        except tk.TclError:
            img = None
        self.thumbs[a['id']] = img
        return img

    def refresh_list(self, keep=None):
        self.tree.delete(*self.tree.get_children())
        for a in self.items:
            img = self.thumbs.get(a['id']) or self.thumb(a)
            self.tree.insert('', 'end', iid=a['id'], text='  ' + a['name'], values=(A.RARITY_LABEL[a['rarity']],),
                             image=img or '')
        if keep and self.tree.exists(keep):
            self.tree.selection_set(keep)
            self.tree.see(keep)
        self.status.config(text=f'{len(self.items)} conquistas')

    def current(self):
        return next((a for a in self.items if a['id'] == self.sel), None)

    def select(self):
        s = self.tree.selection()
        if not s:
            return
        self.sel = s[0]
        a = self.current()
        self.loading = True
        self.v_name.set(a['name'])
        self.t_desc.delete('1.0', 'end')
        self.t_desc.insert('1.0', a['description'])
        self.t_desc.edit_modified(False)
        self.v_rarity.set(A.RARITY_LABEL[a['rarity']])
        self.v_hidden.set(bool(a.get('hidden')))
        c = a['condition']
        if c['kind'] == 'all':
            key = 'all'
        elif c.get('preset'):
            key = 'preset:' + c['preset']
        elif c['kind'] in ('quest', 'flag'):
            key = c['kind']
        else:
            key = None
        self.v_kind.set(dict(KINDS).get(key, 'Condição avançada (só no arquivo)'))
        unit = A.PRESETS.get(c.get('preset'), {}).get('unit', 1)
        self.v_min.set(str(int(c.get('min', 1) // unit)) if 'min' in c else '1')
        q = c.get('quest', '')
        self.v_quest.set(f'{A.QUESTS[q]}  [{q}]' if q in A.QUESTS else q)
        self.v_flag.set(A.FLAGS.get(c.get('name', ''), ''))
        self.v_emblem.set(a.get('emblem') or 'triforce')
        icon = a.get('icon') or {}
        self.v_zoom.set(icon.get('zoom', 1.0))
        self.v_dx.set(icon.get('dx', 0.0))
        self.v_dy.set(icon.get('dy', 0.0))
        self.loading = False
        self.show_params(key)
        self.draw_icon()

    # ---- editing

    def show_params(self, key):
        for w in (self.l_param, self.s_min, self.c_quest, self.c_flag):
            w.pack_forget()
        hint = ''
        if key and key.startswith('preset:'):
            p = A.PRESETS[key[7:]]
            self.l_param.config(text='Quantidade necessária')
            self.l_param.pack(side='left')
            self.s_min.config(to=p['max'])
            self.s_min.pack(side='left', padx=8)
            hint = f'Máximo no jogo: {p["max"]}.'
        elif key == 'quest':
            self.l_param.config(text='Missão')
            self.l_param.pack(side='left')
            self.c_quest.pack(side='left', fill='x', expand=True, padx=8)
            hint = ('Escolha uma missão da lista, ou digite o nome interno de outra missão do jogo. '
                    '"Salvar e aplicar" confere se ela existe.')
        elif key == 'flag':
            self.l_param.config(text='Marco')
            self.l_param.pack(side='left')
            self.c_flag.pack(side='left', fill='x', expand=True, padx=8)
        elif key == 'all':
            hint = 'Desbloqueia quando todas as outras conquistas estiverem desbloqueadas.'
        elif key is None:
            hint = 'Esta condição foi escrita direto no arquivo achievements.json; o editor a mantém como está.'
        self.l_hint.config(text=hint)

    def kind_changed(self):
        key = next(k for k, label in KINDS if label == self.v_kind.get())
        a = self.current()
        if key.startswith('preset:'):
            p = key[7:]
            a['condition'] = dict(A.PRESETS[p]['cond'], min=A.PRESETS[p].get('unit', 1), preset=p)
            self.v_min.set('1')
        elif key == 'quest':
            q = next(iter(A.QUESTS))
            a['condition'] = {'kind': 'quest', 'quest': q}
            self.v_quest.set(f'{A.QUESTS[q]}  [{q}]')
        elif key == 'flag':
            f = next(iter(A.FLAGS))
            a['condition'] = {'kind': 'flag', 'name': f}
            self.v_flag.set(A.FLAGS[f])
        else:
            a['condition'] = {'kind': 'all'}
        self.show_params(key)
        self.form_changed()

    def desc_changed(self, _e):
        if self.t_desc.edit_modified():
            self.t_desc.edit_modified(False)
            self.form_changed()

    def form_changed(self):
        if self.loading or not self.sel:
            return
        a = self.current()
        a['name'] = self.v_name.get().strip() or 'Sem nome'
        a['description'] = self.t_desc.get('1.0', 'end').strip()
        rarity = next((r for r in A.RARITIES if A.RARITY_LABEL[r] == self.v_rarity.get()), a['rarity'])
        if rarity != a['rarity']:
            a['rarity'] = rarity
            self.dirty_icons.add(a['id'])
            self.icon_changed()
        a['hidden'] = bool(self.v_hidden.get())
        c = a['condition']
        if c.get('preset'):
            try:
                c['min'] = max(1, int(float(self.v_min.get()))) * A.PRESETS[c['preset']].get('unit', 1)
            except ValueError:
                pass
        elif c['kind'] == 'quest':
            m = re.search(r'\[(\w+)\]\s*$', self.v_quest.get())
            c['quest'] = m.group(1) if m else self.v_quest.get().strip()
        elif c['kind'] == 'flag':
            c['name'] = next((k for k, v in A.FLAGS.items() if v == self.v_flag.get()), c['name'])
        self.tree.item(a['id'], text='  ' + a['name'], values=(A.RARITY_LABEL[a['rarity']],))
        self.draw_note()

    # ---- icons

    def render_icon(self, a):
        icon = a.get('icon') or {}
        src = self.folder / icon['source'] if icon.get('source') else None
        if src and src.is_file():
            w, h, rgba = A.png_read(src)
            c = A.picture_icon(A.Canvas.from_rgba8(w, h, rgba), a['rarity'], icon.get('zoom', 1.0), icon.get('dx', 0.0),
                               icon.get('dy', 0.0))
        else:
            c = A.emblem_icon(a.get('emblem') or 'triforce', a['rarity'])
        A.png_write(self.folder / 'icons' / f'{a["id"]}.png', c.w, c.h, c.to_rgba8())
        self.dirty_icons.discard(a['id'])
        self.thumbs.pop(a['id'], None)

    def icon_changed(self):
        if self.loading or not self.sel:
            return
        a = self.current()
        a['emblem'] = self.v_emblem.get() or a.get('emblem')
        if a.get('icon', {}).get('source'):
            a['icon'].update(zoom=round(self.v_zoom.get(), 3), dx=round(self.v_dx.get(), 3), dy=round(self.v_dy.get(), 3))
        self.dirty_icons.add(a['id'])
        if self.pending:
            self.root.after_cancel(self.pending)
        self.pending = self.root.after(PREVIEW_DELAY_MS, self.redraw_icon)

    def redraw_icon(self):
        self.pending = None
        a = self.current()
        if not a:
            return
        self.render_icon(a)
        self.tree.item(a['id'], image=self.thumb(a) or '')
        self.draw_icon()

    def draw_icon(self):
        a = self.current()
        png = self.folder / 'icons' / f'{a["id"]}.png'
        if a['id'] in self.dirty_icons or not png.is_file():
            self.render_icon(a)
        self.preview_img = tk.PhotoImage(file=str(png))
        self.cv_icon.delete('all')
        self.cv_icon.create_image(80, 80, image=self.preview_img)
        self.draw_note()

    def draw_note(self):
        a = self.current()
        if not a or not self.preview_img:
            return
        cv = self.cv_note
        cv.delete('all')
        plat = a['rarity'] == 'platinum'
        col = '#d8bf7e' if plat else RARITY_HEX[a['rarity']]
        cv.create_rectangle(10, 8, 410, 88, fill='#0a100e', outline=col, width=2)
        self.note_img = self.preview_img.subsample(2, 2)
        cv.create_image(48, 48, image=self.note_img)
        cv.create_text(92, 32, anchor='w', fill=col, font=('Segoe UI', 9),
                       text='Troféu de platina!' if plat else 'Conquista desbloqueada')
        cv.create_text(92, 56, anchor='w', fill=CREAM, font=('Segoe UI Semibold', 12), text=a['name'][:40])

    def import_picture(self):
        a = self.current()
        if not a:
            return
        path = filedialog.askopenfilename(
            title='Escolha uma imagem',
            initialdir=str(self.album_dir() or Path.home()),
            filetypes=[('Imagens', '*.png *.jpg *.jpeg *.gif *.bmp *.svg *.ppm'), ('Todos os arquivos', '*.*')])
        if not path:
            return
        src = Path('sources') / f'{a["id"]}.png'
        try:
            to_png(path, self.folder / src)
        except Exception as e:   # a picture neither Tk nor Windows reads
            messagebox.showerror('Imagem', f'Não consegui abrir esta imagem:\n{e}')
            return
        a['icon'] = {'source': src.as_posix(), 'zoom': 1.0, 'dx': 0.0, 'dy': 0.0}
        self.loading = True
        self.v_zoom.set(1.0), self.v_dx.set(0.0), self.v_dy.set(0.0)
        self.loading = False
        self.dirty_icons.add(a['id'])
        self.redraw_icon()

    def use_emblem(self):
        a = self.current()
        if a:
            a.pop('icon', None)
            self.dirty_icons.add(a['id'])
            self.redraw_icon()

    def album_dir(self):
        """The game's photo album of the newest save, where the in-game photos are."""
        base = self.dest / 'profile' / 'save'
        albums = sorted(base.glob('*/album'), key=lambda p: p.stat().st_mtime, reverse=True) if base.is_dir() else []
        return albums[0] if albums else None

    # ---- list operations

    def unique_id(self, base):
        ids = {a['id'] for a in self.items}
        i, cand = 2, base
        while cand in ids:
            cand = f'{base[:36]}_{i}'
            i += 1
        return cand

    def new(self):
        a = {'id': self.unique_id('nova_conquista'), 'name': 'Nova conquista', 'description': 'Descreva o desafio.',
             'rarity': 'common', 'emblem': 'triforce', 'hidden': False,
             'condition': dict(A.PRESETS['shrines']['cond'], min=1, preset='shrines')}
        self.insert(a)

    def duplicate(self):
        a = self.current()
        if not a:
            return
        b = copy.deepcopy(a)
        b['id'] = self.unique_id(slug(a['name']))
        b['name'] = a['name'] + ' (cópia)'
        if b.get('icon', {}).get('source'):
            src = Path('sources') / f'{b["id"]}.png'
            (self.folder / src).write_bytes((self.folder / a['icon']['source']).read_bytes())
            b['icon']['source'] = src.as_posix()
        self.insert(b)

    def insert(self, a):
        at = self.items.index(self.current()) + 1 if self.current() else len(self.items)
        self.items.insert(at, a)
        self.dirty_icons.add(a['id'])
        self.refresh_list(keep=a['id'])

    def delete(self):
        a = self.current()
        if a and messagebox.askyesno('Excluir', f'Excluir a conquista "{a["name"]}"?'):
            i = self.items.index(a)
            self.items.remove(a)
            nxt = self.items[min(i, len(self.items) - 1)]['id'] if self.items else None
            self.sel = None
            self.refresh_list(keep=nxt)

    def move(self, step):
        a = self.current()
        if not a:
            return
        i = self.items.index(a)
        j = i + step
        if 0 <= j < len(self.items):
            self.items[i], self.items[j] = self.items[j], self.items[i]
            self.refresh_list(keep=a['id'])

    def restore(self):
        if not messagebox.askyesno('Restaurar padrão', 'Trocar todas as conquistas pelo conjunto padrão? '
                                   'As suas mudanças e ícones importados deixam de ser usados.'):
            return
        A.write_defaults(self.dest, force=True)
        self.defs = json.loads(self.path.read_text(encoding='utf-8'))
        self.items = self.defs['achievements']
        self.thumbs.clear()
        self.dirty_icons.clear()
        self.sel = None
        self.refresh_list(keep=self.items[0]['id'])
        self.status.config(text='Conjunto padrão restaurado; clique em "Salvar e aplicar"')

    # ---- save

    def save(self):
        for a in self.items:
            if a['id'] in self.dirty_icons:
                self.render_icon(a)
        self.defs['format'] = A.FORMAT
        tmp = self.path.with_suffix('.json.tmp')
        tmp.write_text(json.dumps(self.defs, ensure_ascii=False, indent=1), encoding='utf-8')
        os.replace(tmp, self.path)
        self.status.config(text='Aplicando…')
        self.root.config(cursor='watch')

        result = []

        def work():   # the compile reads the RomFS for a few seconds; Tk is only touched from the main thread
            try:
                n = A.compile_definitions(self.dest, log=lambda m: None)
                result.append((f'{n} conquistas salvas. Reinicie o jogo para ver as mudanças.', None))
            except A.CompileError as e:
                result.append(('Não foi aplicado: corrija o erro e salve de novo.', str(e)))
            except Exception as e:   # a RomFS that cannot be read, a file in use
                result.append(('Não foi aplicado.', f'{type(e).__name__}: {e}'))

        def poll():
            if result:
                self.saved(*result[0])
            else:
                self.root.after(100, poll)
        threading.Thread(target=work, daemon=True).start()
        self.root.after(100, poll)

    def saved(self, msg, err):
        self.root.config(cursor='')
        self.status.config(text=msg)
        if err:
            messagebox.showerror('Conquistas', err)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--dest', type=Path, required=True, help='the install folder (with totk.exe and romfs/)')
    args = ap.parse_args()
    root = tk.Tk()
    if not (args.dest / 'romfs' / 'index.json').is_file():
        messagebox.showerror('Conquistas', f'{args.dest} não é uma pasta de instalação do jogo (falta romfs).')
        return
    Editor(root, args.dest)
    root.mainloop()


if __name__ == '__main__':
    main()
