import './style.css';
import { invoke } from '@tauri-apps/api/core';
import { listen } from '@tauri-apps/api/event';
import { getCurrentWindow } from '@tauri-apps/api/window';
import { open as openDialog } from '@tauri-apps/plugin-dialog';
import { setLang, currentLang } from './i18n.js';
import { check as checkLauncherUpdate } from '@tauri-apps/plugin-updater';
import { relaunch } from '@tauri-apps/plugin-process';
import { getVersion } from '@tauri-apps/api/app';

const win = getCurrentWindow();
const $ = (s) => document.querySelector(s);
const $$ = (s) => [...document.querySelectorAll(s)];
const esc = (t) => String(t).replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);

// ---------------------------------------------------------------- launcher-only preferences (this PC)
const prefs = Object.assign({ music: true, volume: 35, closeOnPlay: false, inputs: {} },
  JSON.parse(localStorage.getItem('prefs') || '{}'));
const savePrefs = () => localStorage.setItem('prefs', JSON.stringify(prefs));

// ---------------------------------------------------------------- artwork
// The launcher's own neutral art (index.html, style.css), or the player's from launcher-art\ (main.rs custom_art):
// bg*.jpg/png as the slideshow, logo.png, loading.jpg as the transition, music.mp3.
let art = { backgrounds: [], logo: null, loading: null, music: null };
let SLIDES = ['n0', 'n1', 'n2', 'n3'];   // neutral slides: CSS classes
async function loadArt() {
  try { art = await invoke('custom_art'); } catch { /* neutral art */ }
  if (art.backgrounds.length) SLIDES = art.backgrounds;
  if (art.logo) $('#logo').innerHTML = `<img src="${art.logo}" alt="" />`;
  if (art.loading) {
    $('#t-img').src = art.loading;
    $('#t-img').hidden = false;
    $('#t-neutral').hidden = true;
  } else $('#transition').classList.add('neutral');
  if (art.music) bgm.src = art.music;
  else $('#music').classList.add('none');
}

// ---------------------------------------------------------------- background slideshow
let slide = 0, slideTimer;
function buildSlides() {
  const bg = $('#bg'), dots = $('#dots');
  SLIDES.forEach((src, i) => {
    const d = document.createElement('div');
    d.className = 'slide';
    if (src.startsWith('data:')) d.style.backgroundImage = `url(${src})`;
    else { d.classList.add(src); d.innerHTML = '<div class="pattern"></div>'; }
    d.style.animationDelay = `${-i * 6}s`;
    bg.append(d);
    const b = document.createElement('button');
    b.title = `Imagem ${i + 1}`;
    b.onclick = () => showSlide(i, true);
    dots.append(b);
  });
  showSlide(0);
}
function showSlide(i, user) {
  slide = (i + SLIDES.length) % SLIDES.length;
  $$('#bg .slide').forEach((s, k) => s.classList.toggle('on', k === slide));
  $$('#dots button').forEach((b, k) => b.classList.toggle('on', k === slide));
  clearTimeout(slideTimer);
  slideTimer = setTimeout(() => showSlide(slide + 1), user ? 14000 : 9000);
}

// ---------------------------------------------------------------- music
const bgm = $('#bgm');
let fadeTimer;
function fadeMusic(to, ms = 800) {
  clearInterval(fadeTimer);
  const from = bgm.volume, t0 = performance.now();
  if (to > 0 && bgm.paused) bgm.play().catch(() => {});
  fadeTimer = setInterval(() => {
    const k = Math.min(1, (performance.now() - t0) / ms);
    bgm.volume = from + (to - from) * k;
    if (k >= 1) { clearInterval(fadeTimer); if (to === 0) bgm.pause(); }
  }, 30);
}
function musicOn() { return prefs.music && !!art.music && !document.body.classList.contains('in-game'); }
function applyMusic() {
  $('#music').classList.toggle('muted', !prefs.music);
  if (musicOn()) fadeMusic(prefs.volume / 100); else fadeMusic(0, 400);
}
// autoplay may wait for the first click
document.addEventListener('pointerdown', () => { if (musicOn() && bgm.paused) applyMusic(); }, { once: true });

// ---------------------------------------------------------------- window buttons and tabs
$('#min').onclick = () => win.minimize();
$('#max').onclick = () => win.toggleMaximize();
$('#close').onclick = () => win.close();
$('#music').onclick = () => { prefs.music = !prefs.music; savePrefs(); $('#set-music').checked = prefs.music; applyMusic(); };

function showView(name) {
  $$('.tab').forEach((t) => t.classList.toggle('active', t.dataset.view === name));
  $$('.view').forEach((v) => v.classList.toggle('active', v.id === `view-${name}`));
  document.body.classList.toggle('panel-open', name !== 'home');
}
$$('.tab').forEach((t) => (t.onclick = () => { showView(t.dataset.view); if (t.dataset.view === 'mods') loadMods(); }));
$$('[data-set]').forEach((b) => (b.onclick = () => {
  $$('[data-set]').forEach((x) => x.classList.toggle('active', x === b));
  $$('.set-page').forEach((pg) => pg.classList.toggle('active', pg.id === `set-${b.dataset.set}`));
}));

// ---------------------------------------------------------------- status
let status = {};
async function refresh() {
  status = await invoke('status');
  $('#version').textContent = status.installed_release ? `v${status.installed_release.split('+')[0]}` : '';
  $('#home-path').value = status.home;
  $('#release-path').value = status.release || '';
  $('#release-version').textContent = status.release_version ? `· versão ${status.release_version}${status.dev ? ' (repositório)' : ''}` : '';
  const play = $('#play'), msg = $('#home-status');
  play.disabled = status.building || status.running || status.downloading;
  play.classList.toggle('install', !status.installed);
  play.querySelector('.play-label').textContent = status.installed ? 'Jogar' : 'Instalar';
  if (status.building) msg.innerHTML = 'Instalação em andamento…';
  else if (status.downloading) msg.innerHTML = 'Baixando o instalador…';
  else if (status.running) msg.innerHTML = 'O jogo está aberto';
  else if (status.installed) msg.innerHTML = `Pronto para jogar · <b>Tears of the Kingdom ${esc(status.game_version || '')}</b>`;
  else if (!status.release) msg.innerHTML = 'Baixe o instalador em <b>Instalação</b>';
  else msg.innerHTML = 'Jogo ainda não instalado · leva cerca de 30 min';
  $('#ini-note').textContent = status.running
    ? 'O jogo está aberto: as mudanças marcadas "ao abrir" valem na próxima vez; as outras o jogo lê na hora.'
    : status.installed ? '' : 'O jogo ainda não está instalado: estas escolhas ficam no totk.ini da pasta de instalação.';
  renderInstallForm();
  renderRelease();
  renderUpdates();
}

// The game files: a first install needs all of them; a folder with totk-install.json resumes or updates with only
// the folder (the installer asks for the files again when its extraction must run again). Remembered per PC.
// Peak space of a first install on the install disk: about 36 GB (docs/installer.md); the installer checks the
// exact figure itself.
function renderInstallForm() {
  const need = status.installed ? 0 : status.has_state ? 20 : 36;
  const free = status.free_gb;
  const el = $('#home-free');
  el.textContent = free == null ? '' : `· ${free.toFixed(0)} GB livres${need ? ` (precisa de ~${need} GB)` : ''}`;
  el.classList.toggle('low', free != null && free < need);
  for (const k of ['base', 'update', 'keys']) $(`#in-${k}`).value = prefs.inputs[k] || '';
  $('#in-lang').value = prefs.inputs.lang || status.lang || defaultGameLang();
  $('#in-lang-field').classList.toggle('hidden', !!status.has_state);   // afterwards: Configurações > Jogo
  const known = $('#inputs-known');
  known.classList.toggle('hidden', !status.has_state);
  if (status.has_state) {
    const files = [status.base_name, status.update_name].filter(Boolean).map((n) => `<code>${esc(n)}</code>`).join(' + ');
    known.innerHTML = 'Esta pasta já tem uma instalação: Instalar continua ou atualiza, e os arquivos do jogo só são ' +
      `pedidos de novo se a extração precisar rodar outra vez.${files ? `<br>${files}` : ''}`;
  }
}

// ---------------------------------------------------------------- installer source (main.rs get_source / set_source)
// Oficial: valentderah/totk-pc-recomp. Alternativo: a repository of the player's choice (this launcher's own by
// default), with a GitHub token when it is private.
let source = {};
async function loadSource() {
  source = await invoke('get_source');
  $$('#src-seg button').forEach((b) => b.classList.toggle('on', b.dataset.src === source.source));
  $('#src-alt').classList.toggle('hidden', source.source !== 'alt');
  $('#src-repo').value = source.alt;
  $('#src-token').value = '';
  $('#src-token').placeholder = source.has_token ? 'token salvo (deixe vazio para manter)' : 'token do GitHub (só para repositório privado)';
}
async function saveSource(which, withToken) {
  try {
    await invoke('set_source', { source: which, alt: $('#src-repo').value, token: withToken && $('#src-token').value ? $('#src-token').value : null });
    await loadSource();
    $('#rel-list').innerHTML = '<option value="">Buscando versões…</option>';
    await loadReleases();
  } catch (e) { showError('Não foi possível salvar', String(e)); }
}
$$('#src-seg button').forEach((b) => (b.onclick = () => saveSource(b.dataset.src, false)));
$('#src-save').onclick = () => saveSource('alt', true);

// ---------------------------------------------------------------- installer releases (main.rs releases / download_release)
// TotK-Setup-<version>-windows.zip from the GitHub releases of status.repo, checked against its SHA-256 and unpacked
// into the launcher's data folder. The newest stable release is picked unless a pre-release is newer than it.
let releases = [], releasesError = null;
const vkey = (v) => String(v || '').replace(/^v/i, '').split(/[+\- ]/)[0].split('.').map((n) => +n || 0);
const newer = (a, b) => { const x = vkey(a), y = vkey(b); for (let i = 0; i < Math.max(x.length, y.length); i++) { if ((x[i] || 0) !== (y[i] || 0)) return (x[i] || 0) > (y[i] || 0); } return false; };
async function loadReleases() {
  try { releases = await invoke('releases'); releasesError = null; } catch (e) { releases = []; releasesError = String(e); }
  const sel = $('#rel-list');
  sel.innerHTML = releases.length
    ? releases.map((r, i) => `<option value="${i}">${esc(r.tag)}${r.prerelease ? ' · pré-lançamento' : ''} · ${(r.size / 1e6).toFixed(0)} MB</option>`).join('')
    : `<option value="">${releasesError ? 'Sem conexão com o GitHub' : 'Nenhuma versão publicada'}</option>`;
  if (releases.length) sel.value = '0';
  renderRelease();
  renderUpdates();
}
function selectedRelease() { return releases[+$('#rel-list').value] || null; }
function renderRelease() {
  const r = selectedRelease(), btn = $('#rel-get');
  const current = !status.dev && status.release_version && r && !newer(r.tag, status.release_version) && !newer(status.release_version, r.tag);
  btn.disabled = !r || status.downloading || status.building || current;
  btn.textContent = current ? 'Em uso' : status.release && !status.dev ? 'Trocar' : 'Baixar';
  if (!status.downloading && !dlError) $('#dl-status').textContent = releasesError || '';
}
let dlError = null;
async function downloadRelease(r) {
  dlError = null;
  $('#dl-bar').classList.remove('hidden', 'error', 'idle');
  $('#dl-fill').style.width = '0%';
  $('#dl-status').textContent = `Baixando ${r.asset}…`;
  try { await invoke('download_release', { rel: r }); } catch (e) { $('#dl-bar').classList.add('hidden'); return showError('Não foi possível baixar', String(e)); }
  refresh();
}
$('#rel-get').onclick = () => { const r = selectedRelease(); if (r) downloadRelease(r); };
$('#rel-list').onchange = renderRelease;
listen('dl-progress', ({ payload: p }) => {
  if (p.phase === 'unpack') { $('#dl-status').textContent = 'Descompactando…'; $('#dl-fill').style.width = '100%'; return; }
  const pct = p.total ? (p.done / p.total) * 100 : 0;
  $('#dl-fill').style.width = `${pct}%`;
  $('#dl-status').textContent = `Baixando · ${(p.done / 1e6).toFixed(0)} de ${(p.total / 1e6).toFixed(0)} MB`;
});
listen('dl-done', async ({ payload: err }) => {
  $('#dl-bar').classList.add('idle');
  if (err) {
    dlError = err;
    $('#dl-bar').classList.add('error');
    $('#dl-status').textContent = '';
    showError('Não foi possível baixar o instalador', err);
  } else {
    $('#dl-bar').classList.add('hidden');
    $('#dl-status').textContent = 'Instalador pronto.';
  }
  await refresh();
});

// ---------------------------------------------------------------- settings (totk.ini, main.rs read_ini / write_ini)
// Controls with data-ini="<key>" are bound to that totk.ini key: a select or range writes its value (the data-default
// one clears the key, so the game's own default applies), a checkbox writes data-on or data-off (empty clears).
let ini = {};
const managed = () => new Set([...$$('[data-ini]').map((el) => el.dataset.ini), ...BINDS.map(([id]) => `bind_${id.toLowerCase()}`)]);
function showValue(el) {
  if (el.dataset.label) $(el.dataset.label).textContent = `${(+el.value).toLocaleString(currentLang() === 'pt' ? 'pt-BR' : 'en')}${el.dataset.unit || ''}`;
}
function iniToControls() {
  for (const el of $$('[data-ini]')) {
    const v = ini[el.dataset.ini];
    if (el.type === 'checkbox') el.checked = el.dataset.on ? v === el.dataset.on : v !== el.dataset.off;
    else {
      const want = v ?? el.dataset.default ?? '';
      if (el.tagName === 'SELECT' && want && ![...el.options].some((o) => o.value === want)) el.add(new Option(want, want));
      el.value = want;
      showValue(el);
    }
  }
  const m = managed();
  $('#set-extra').value = Object.entries(ini).filter(([k]) => !m.has(k)).map(([k, v]) => `${k} = ${v}`).join('\n');
  loadBinds();
  renderQuick();
}
function controlValue(el) {
  if (el.type === 'checkbox') return el.checked ? el.dataset.on : el.dataset.off;
  return el.value === (el.dataset.default ?? null) ? '' : el.value;
}
async function loadSettings() {
  try { ini = await invoke('read_ini'); } catch { ini = {}; }
  iniToControls();
}
let savedTimer;
async function saveIni(changes) {
  try {
    await invoke('write_ini', { changes });
    for (const [k, v] of Object.entries(changes)) { if (v) ini[k] = v; else delete ini[k]; }
    $('#saved').classList.add('on');
    clearTimeout(savedTimer);
    savedTimer = setTimeout(() => $('#saved').classList.remove('on'), 1400);
    renderQuick();
  } catch (e) { showError('Não foi possível salvar', String(e)); }
}
$$('[data-ini]').forEach((el) => {
  if (el.type === 'range') el.oninput = () => showValue(el);
  el.onchange = () => saveIni({ [el.dataset.ini]: controlValue(el) });
});
$('#set-extra').onchange = () => {
  const m = managed(), changes = {}, seen = new Set();
  for (const line of $('#set-extra').value.split('\n')) {
    const t = line.trim().match(/^(?:totk_)?([a-z0-9_]+)\s*=\s*(.*)$/i);
    if (!t) continue;
    const k = t[1].toLowerCase();
    if (m.has(k)) continue;
    changes[k] = t[2].trim();
    seen.add(k);
  }
  for (const k of Object.keys(ini)) if (!m.has(k) && !seen.has(k)) changes[k] = '';   // removed lines go back to the default
  saveIni(changes);
};
// The game language (runtime/cfg.c LANG): the installer's list; the default follows the Windows language.
const LANGS = [['en-US', 'English (US)'], ['en-GB', 'English (UK)'], ['ja', '日本語'], ['fr', 'Français'],
  ['fr-CA', 'Français (Canada)'], ['de', 'Deutsch'], ['it', 'Italiano'], ['es', 'Español'],
  ['es-419', 'Español (Latinoamérica)'], ['nl', 'Nederlands'], ['ru', 'Русский'], ['ko', '한국어'],
  ['zh-CN', '简体中文'], ['zh-TW', '繁體中文']];
function defaultGameLang() {
  const n = navigator.language;
  if (/^es-(?!ES)/.test(n)) return 'es-419';
  if (n === 'fr-CA' || n === 'en-GB' || n === 'zh-TW') return n;
  const base = n.split('-')[0];
  return (LANGS.find(([v]) => v.split('-')[0] === base) || ['en-US'])[0];
}
$('#in-lang').innerHTML = LANGS.map(([v, n]) => `<option value="${v}">${n}</option>`).join('');
$('#in-lang').onchange = (e) => { prefs.inputs.lang = e.target.value; savePrefs(); };
$('#set-game-lang').innerHTML = `<option value="">Do Windows</option>` + LANGS.map(([v, n]) => `<option value="${v}">${n}</option>`).join('');
$('#set-game-lang').dataset.default = '';

// Keyboard and mouse bindings (runtime/input_map.c): bind_<button> = pad:..., key:... entries. The launcher edits the
// key part only and keeps the line's pad entries (pad:default when the line is new; the stick directions take keys only).
// The defaults are input_map.c DEFAULT_KEYS.
const BINDS = [   // [action, group, name, what it does, default keys]
  ['LSTICK_UP', 'Movimento', 'Frente', 'Analógico esquerdo', ['w']],
  ['LSTICK_DOWN', 'Movimento', 'Trás', '', ['s']],
  ['LSTICK_LEFT', 'Movimento', 'Esquerda', '', ['a']],
  ['LSTICK_RIGHT', 'Movimento', 'Direita', '', ['d']],
  ['RSTICK_UP', 'Câmera', 'Olhar para cima', 'Analógico direito (o mouse também gira)', ['up']],
  ['RSTICK_DOWN', 'Câmera', 'Olhar para baixo', '', ['down']],
  ['RSTICK_LEFT', 'Câmera', 'Olhar à esquerda', '', ['left']],
  ['RSTICK_RIGHT', 'Câmera', 'Olhar à direita', '', ['right']],
  ['A', 'Ações', 'A · Interagir', 'Pegar, falar, confirmar', ['e']],
  ['B', 'Ações', 'B · Correr / cancelar', 'Correr (segurar), guardar a arma', ['lshift']],
  ['X', 'Ações', 'X · Pular', 'Parapente no ar', ['space']],
  ['Y', 'Ações', 'Y · Atacar', 'Ataque carregado (segurar)', ['mouse_l']],
  ['L', 'Ações', 'L · Habilidade', 'Roda de habilidades (segurar)', ['q']],
  ['R', 'Ações', 'R · Arremessar', 'Mira o arremesso, solta para jogar', ['r']],
  ['ZL', 'Ações', 'ZL · Focar / escudo', 'Travar no alvo, erguer o escudo', ['mouse_r', 'z']],
  ['ZR', 'Ações', 'ZR · Arco', 'Puxar (segurar), soltar dispara', ['f', 'x']],
  ['LCLICK', 'Ações', 'Agachar', 'Clique do analógico esquerdo', ['lctrl']],
  ['RCLICK', 'Ações', 'Luneta', 'Clique do analógico direito', ['c']],
  ['PLUS', 'Menus', '+ · Inventário', 'Bolsa e pausa', ['tab']],
  ['MINUS', 'Menus', '− · Purah Pad', 'Mapa e diário', ['m']],
  ['UP', 'Menus rápidos', 'Materiais', 'Direcional para cima (segurar)', ['2']],
  ['DOWN', 'Menus rápidos', 'Assobiar', 'Chama o cavalo', ['4']],
  ['LEFT', 'Menus rápidos', 'Escudos', 'Direcional para a esquerda (segurar)', ['1']],
  ['RIGHT', 'Menus rápidos', 'Armas / arcos', 'Direcional para a direita (segurar)', ['3']],
];
const KEY_OF_CODE = (() => {   // KeyboardEvent.code -> input_map.c key_names
  const m = { Space: 'space', Tab: 'tab', ShiftLeft: 'lshift', ShiftRight: 'rshift', ControlLeft: 'lctrl', ControlRight: 'rctrl',
    AltLeft: 'lalt', CapsLock: 'capslock', Enter: 'enter', Backspace: 'backspace', Insert: 'insert', Delete: 'delete', Home: 'home',
    End: 'end', PageUp: 'pageup', PageDown: 'pagedown', ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right',
    NumpadAdd: 'num_add', NumpadSubtract: 'num_sub', NumpadMultiply: 'num_mul', NumpadDivide: 'num_div', NumpadDecimal: 'num_decimal',
    Minus: 'minus', Equal: 'equals', BracketLeft: 'lbracket', BracketRight: 'rbracket', Semicolon: 'semicolon', Quote: 'apostrophe',
    Comma: 'comma', Period: 'period', Slash: 'slash', Backslash: 'backslash', Backquote: 'grave',
    F2: 'f2', F3: 'f3', F4: 'f4', F5: 'f5', F6: 'f6', F7: 'f7', F9: 'f9', F12: 'f12' };
  for (let i = 0; i < 26; i++) m['Key' + String.fromCharCode(65 + i)] = String.fromCharCode(97 + i);
  for (let i = 0; i <= 9; i++) { m['Digit' + i] = String(i); m['Numpad' + i] = 'num' + i; }
  return m;
})();
const MOUSE_KEYS = ['mouse_l', 'mouse_m', 'mouse_r', 'mouse_x1', 'mouse_x2'];   // MouseEvent.button 0-4
function keyName(k) {
  if (!k) return '—';
  const named = { space: 'Espaço', tab: 'Tab', lshift: 'Shift esq.', rshift: 'Shift dir.', lctrl: 'Ctrl esq.', rctrl: 'Ctrl dir.',
    lalt: 'Alt esq.', capslock: 'Caps Lock', enter: 'Enter', backspace: 'Backspace', up: '↑', down: '↓', left: '←', right: '→',
    mouse_l: 'Mouse esq.', mouse_r: 'Mouse dir.', mouse_m: 'Mouse meio', mouse_x1: 'Mouse 4', mouse_x2: 'Mouse 5',
    minus: '-', equals: '=', lbracket: '[', rbracket: ']', semicolon: ';', apostrophe: "'", comma: ',', period: '.', slash: '/',
    backslash: '\\', grave: '`', num_add: 'Num +', num_sub: 'Num -', num_mul: 'Num *', num_div: 'Num /', num_decimal: 'Num ,' };
  if (named[k]) return named[k];
  if (/^num\d$/.test(k)) return `Num ${k[3]}`;
  return k.toUpperCase();
}
let binds = {};   // action -> {keys: [k1, k2], pads: [raw pad entries]}
let listening = null;   // {id, slot, el}
function loadBinds() {
  binds = {};
  for (const [id, , , , def] of BINDS) {
    const line = ini[`bind_${id.toLowerCase()}`];
    const stick = id.includes('STICK_');
    if (line === undefined) { binds[id] = { keys: [def[0] || '', def[1] || ''], pads: stick ? [] : ['pad:default'] }; continue; }
    const parts = line.split(',').map((t) => t.trim()).filter(Boolean);
    let keys = [];
    const pads = parts.filter((t) => t.toLowerCase().startsWith('pad:'));
    for (const t of parts) {
      const m = t.match(/^key:(.+)$/i);
      if (!m) continue;
      if (m[1].toLowerCase() === 'default') keys.push(...def); else keys.push(m[1].toLowerCase());
    }
    keys = keys.slice(0, 2);
    binds[id] = { keys: [keys[0] || '', keys[1] || ''], pads };
  }
  showBinds();
}
function bindLine(id) {
  const def = BINDS.find((b) => b[0] === id)[4];
  const { keys, pads } = binds[id];
  const ks = keys.filter(Boolean);
  const stick = id.includes('STICK_');
  const sameKeys = ks.length === def.length && ks.every((k, i) => k === def[i]);
  const defaultPads = stick ? pads.length === 0 : pads.length === 1 && pads[0].toLowerCase() === 'pad:default';
  if (sameKeys && defaultPads) return '';   // the defaults: no line
  const entries = [...pads, ...ks.map((k) => `key:${k}`)];
  return entries.length ? entries.join(', ') : 'none';
}
function saveBinds(ids) {
  const changes = {};
  for (const id of ids) changes[`bind_${id.toLowerCase()}`] = bindLine(id);
  saveIni(changes);
}
function showBinds() {
  const box = $('#bindings');
  box.textContent = '';
  let group = '';
  for (const [id, g, name, what] of BINDS) {
    if (g !== group) { group = g; const h = document.createElement('h4'); h.textContent = g; box.append(h); }
    const row = document.createElement('div');
    row.className = 'bind-row';
    const label = document.createElement('div');
    label.className = 'what';
    label.textContent = name;
    if (what) { const sm = document.createElement('small'); sm.textContent = what; label.append(sm); }
    row.append(label);
    binds[id].keys.forEach((key, slot) => {
      const el = document.createElement('div');
      el.className = 'bind-slot' + (key ? '' : ' empty');
      el.tabIndex = 0;
      el.textContent = keyName(key);
      const x = document.createElement('button');
      x.className = 'clear';
      x.title = 'Apagar';
      x.textContent = '×';
      x.onmousedown = (e) => e.stopPropagation();
      x.onclick = (e) => { e.stopPropagation(); stopListening(); binds[id].keys[slot] = ''; showBinds(); saveBinds([id]); };
      el.append(x);
      el.onmousedown = (e) => {
        if (listening && listening.el === el) { e.preventDefault(); assign(MOUSE_KEYS[e.button]); return; }
        stopListening();
        listening = { id, slot, el };
        el.classList.add('listening');
        el.firstChild.textContent = 'Aperte…';
        e.preventDefault();
      };
      el.oncontextmenu = (e) => e.preventDefault();
      row.append(el);
    });
    box.append(row);
  }
}
function stopListening() {
  if (!listening) return;
  listening.el.classList.remove('listening');
  listening = null;
  showBinds();
}
function assign(key) {
  if (!listening || !key) return;
  const { id, slot } = listening;
  const changed = [id];
  for (const k in binds) {   // one action per key (as the game's Controls tab does)
    if (k !== id && binds[k].keys.includes(key)) { binds[k].keys = binds[k].keys.map((c) => (c === key ? '' : c)); changed.push(k); }
  }
  binds[id].keys[slot] = key;
  if (binds[id].keys[1 - slot] === key) binds[id].keys[1 - slot] = '';
  listening = null;
  showBinds();
  saveBinds(changed);
}
addEventListener('keydown', (e) => {
  if (!listening) return;
  e.preventDefault();
  e.stopPropagation();
  if (e.code === 'Escape') return stopListening();
  assign(KEY_OF_CODE[e.code]);   // F1, F8, F10, F11 and Esc have no name: the game keeps them
}, true);
addEventListener('mousedown', (e) => { if (listening && !listening.el.contains(e.target)) stopListening(); });
$('#keys-reset').onclick = () => {
  stopListening();
  for (const [id, , , , def] of BINDS) binds[id].keys = [def[0] || '', def[1] || ''];
  showBinds();
  saveBinds(BINDS.map(([id]) => id));
};

// ---------------------------------------------------------------- quality presets (Configurações > Exibição)
const PRESETS = {
  perf: { output_res: '', render_scale: '67' },
  balanced: { output_res: '', render_scale: '' },
  quality: { output_res: '2560x1440', render_scale: '100' },
  ultra: { output_res: '3840x2160', render_scale: '100' },
};
function renderQuick() {
  for (const b of $$('[data-preset]')) {
    const p = PRESETS[b.dataset.preset];
    b.classList.toggle('on', Object.entries(p).every(([k, v]) => (ini[k] || '') === v));
  }
}
$$('[data-preset]').forEach((b) => (b.onclick = async () => { await saveIni(PRESETS[b.dataset.preset]); iniToControls(); }));

// ---------------------------------------------------------------- mods (main.rs mods_list / mods_apply, mods.py)
// <install>/mods/<name>/ with a romfs/ folder. The enabled ones, in priority order (the first wins a file two mods
// replace), are merged into mods/.merged, which totk.ini's romfs_mods points at. A mod whose versioned files name
// another game version (Product.121 on a 1.4.3 install) cannot be enabled.
let mods = { mods: [], enabled: [], version: null, stale: false }, modsBusy = false;
const AREAS = { Voice: 'vozes', Sound: 'sons', Mals: 'textos', UI: 'interface', Font: 'fontes', TexToGo: 'texturas',
  Model: 'modelos', System: 'sistema', Event: 'cenas', Movie: 'vídeos', Shader: 'shaders', Pack: 'pacotes' };
function modProblem(m) {
  if (!m.romfs) return m.exefs ? 'Só tem exefs (código): o recomp não aceita' : 'Sem pasta romfs';
  const other = (m.made_for || []).filter((v) => v !== mods.version);
  if (other.length) return `Feito para o jogo ${other.join(' / ')}; esta instalação é ${mods.version || '?'}`;
  return null;
}
async function loadMods() {
  if (!status.installed) {
    $('#mods-list').textContent = '';
    $('#mods-status').textContent = 'Instale o jogo para usar mods.';
    return;
  }
  try { mods = await invoke('mods_list'); }
  catch (e) { $('#mods-status').textContent = String(e); return; }
  renderMods();
}
function renderMods() {
  const box = $('#mods-list');
  box.textContent = '';
  const order = [...mods.enabled.filter((n) => mods.mods.some((m) => m.name === n)), ...mods.mods.map((m) => m.name).filter((n) => !mods.enabled.includes(n))];
  if (!modsBusy) {
    $('#mods-status').textContent = !mods.mods.length ? 'Nenhum mod ainda: adicione uma pasta ou um .zip, ou coloque-os na pasta de mods.'
      : mods.stale ? 'Um mod ligado ou os arquivos do jogo mudaram: os mods são preparados de novo ao jogar.' : '';
  }
  for (const name of order) {
    const m = mods.mods.find((x) => x.name === name), on = mods.enabled.includes(name), problem = modProblem(m);
    const a = document.createElement('article');
    a.className = 'mod' + (on ? ' soon' : '');
    const areas = Object.entries(m.areas || {}).sort((x, y) => y[1] - x[1]).map(([k]) => AREAS[k] || k);
    a.innerHTML = `<div class="mod-art"><span class="mod-num">${m.files ? (m.files > 999 ? `${(m.files / 1000).toFixed(1)}k` : m.files) : '–'}</span><small>ARQUIVOS</small></div>
      <div class="mod-body"><div class="mod-head"><h3></h3>${problem ? '<span class="badge warn"></span>' : on ? '<span class="badge">Ligado</span>' : ''}</div>
      <p class="mod-info"></p><ul class="mod-tags"></ul></div>
      <div class="mod-side"><button class="ghost small" data-up title="Subir">▲</button><button class="ghost small" data-down title="Descer">▼</button><button class="ghost small" data-del title="Apagar">×</button></div>
      <label class="toggle mod-toggle" title="Ligar / desligar"><input type="checkbox" /><span class="knob"></span></label>`;
    a.querySelector('h3').textContent = name;
    if (problem) a.querySelector('.badge').textContent = problem;
    a.querySelector('.mod-info').textContent = m.files ? `${(m.size / 1e6).toFixed(0)} MB · substitui ${m.replaces} arquivos do jogo, adiciona ${m.adds}` : '';
    a.querySelector('.mod-tags').innerHTML = areas.slice(0, 5).map((x) => `<li>${esc(x)}</li>`).join('');
    const box2 = a.querySelector('input');
    box2.checked = on;
    box2.disabled = (!!problem && !on) || modsBusy;
    box2.onchange = () => applyMods(box2.checked ? [...mods.enabled, name] : mods.enabled.filter((n) => n !== name));
    const move = (d) => {
      const i = mods.enabled.indexOf(name), j = i + d;
      if (i < 0 || j < 0 || j >= mods.enabled.length) return;
      const e = [...mods.enabled];
      [e[i], e[j]] = [e[j], e[i]];
      applyMods(e);
    };
    a.querySelector('[data-up]').onclick = () => move(-1);
    a.querySelector('[data-down]').onclick = () => move(1);
    a.querySelector('[data-up]').disabled = a.querySelector('[data-down]').disabled = !on || mods.enabled.length < 2 || modsBusy;
    a.querySelector('[data-del]').onclick = () => confirmBox(`Apagar o mod "${name}"?`, 'A pasta dele é apagada da pasta de mods.', async () => {
      try {
        if (on) await applyMods(mods.enabled.filter((n) => n !== name));
        await invoke('mods_remove', { name });
      } catch (e) { showError('Não foi possível apagar', String(e)); }
      loadMods();
    });
    box.append(a);
  }
}
const MOD_WARN = {
  common_unpatched: (w) => `${w.mod}: o arquivo de interface do mod não recebeu o ajuste de 60 FPS; os menus rápidos podem abrir com fundo preto.`,
  no_romfs: (w) => `${w.mod}: sem pasta romfs, ignorado.`,
};
async function applyMods(enabled) {
  modsBusy = true;
  mods.enabled = enabled;
  renderMods();
  $('#mods-status').textContent = enabled.length ? 'Preparando os mods…' : 'Desligando os mods…';
  $('#mods-bar').classList.remove('hidden', 'idle', 'error');
  $('#mods-fill').style.width = '0%';
  try {
    const r = await invoke('mods_apply', { enabled });
    const notes = (r.warnings || []).map((w) => (MOD_WARN[w.id] || ((x) => `${x.mod}: ${x.id}`))(w));
    if (r.conflicts && r.conflicts.length) notes.push(`${r.conflicts.length} arquivos estavam em mais de um mod; valeu o de cima.`);
    $('#mods-status').textContent = enabled.length ? `Pronto: ${r.files} arquivos para o jogo.${notes.length ? ' ' + notes.join(' ') : ''}` : 'Mods desligados.';
    await loadSettings();
  } catch (e) {
    $('#mods-bar').classList.add('error');
    $('#mods-status').textContent = '';
    showError('Não foi possível preparar os mods', String(e));
  }
  modsBusy = false;
  $('#mods-bar').classList.add('hidden');
  await loadMods();
}
listen('mods-progress', ({ payload: p }) => {
  if (p.total) $('#mods-fill').style.width = `${(p.done / p.total) * 100}%`;
  $('#mods-status').textContent = `Preparando os mods · ${p.done} de ${p.total} arquivos`;
});
async function addMod(directory) {
  const path = await openDialog(directory ? { directory: true, title: 'Pasta do mod (com romfs dentro)' }
    : { title: 'Mod em .zip', filters: [{ name: 'Zip', extensions: ['zip'] }] });
  if (!path) return;
  $('#mods-status').textContent = 'Copiando o mod…';
  try { await invoke('mods_add', { path }); } catch (e) { showError('Não foi possível adicionar o mod', String(e)); }
  loadMods();
}
$('#mod-add-dir').onclick = () => addMod(true);
$('#mod-add-zip').onclick = () => addMod(false);
$('#mod-refresh').onclick = () => loadMods();

// ---------------------------------------------------------------- launcher settings
$('#set-music').onchange = (e) => { prefs.music = e.target.checked; savePrefs(); applyMusic(); };
$('#set-volume').oninput = (e) => { prefs.volume = +e.target.value; savePrefs(); if (musicOn()) bgm.volume = prefs.volume / 100; };
$('#set-close').onchange = (e) => { prefs.closeOnPlay = e.target.checked; savePrefs(); };
$$('[data-url]').forEach((b) => (b.onclick = () => invoke('open_path', { path: b.dataset.url })));
$$('[data-open]').forEach((b) => (b.onclick = () => invoke('open_path', { path: b.dataset.open }).catch((e) => showError('Não foi possível abrir', String(e)))));

// ---------------------------------------------------------------- dialog
let dialogLog = null;   // the log the dialog's button opens: the installer's, else the game's
let dialogConfirm = null;   // the action of a confirmation (confirmBox), run by OK
function showError(title, body, log = null) {
  $('#dialog-title').textContent = title;
  $('#dialog-body').textContent = body;
  dialogLog = log;
  dialogConfirm = null;
  $('#dialog-log').classList.remove('hidden');
  $('#dialog-cancel').classList.add('hidden');
  $('#dialog').classList.remove('hidden');
}
function confirmBox(title, body, action) {
  showError(title, body);
  dialogConfirm = action;
  $('#dialog-log').classList.add('hidden');
  $('#dialog-cancel').classList.remove('hidden');
}
$('#dialog-ok').onclick = () => { $('#dialog').classList.add('hidden'); const a = dialogConfirm; dialogConfirm = null; if (a) a(); };
$('#dialog-cancel').onclick = () => { dialogConfirm = null; $('#dialog').classList.add('hidden'); };
$('#dialog-log').onclick = () => invoke('open_path', { path: dialogLog || 'logs/totk.log' });

// ---------------------------------------------------------------- install
// `python -m installer` (main.rs start_build) prints one JSON event per line (installer/events.py): start (the step
// ids), step (started / done / skipped), progress (phase, done/total/unit), space, warning, error, end. Messages are
// ids; installer/text/en.json (installer_text) holds their English text, PT the launcher's Portuguese for steps and
// phases. Each step has its usual share of the time (24-core PC, docs/installer.md) for the bar and the estimate.
const STEP_INFO = {
  check: ['Verificar', 'PC e arquivos', 1],
  extract: ['Extrair', 'NSP / XCI', 6],
  lift: ['Traduzir', 'código → C', 2],
  compile: ['Compilar', 'gerar o totk.exe', 8],
  prepare: ['Shaders', 'para Vulkan', 5],
  cleanup: ['Concluir', 'limpeza', 0.5],
};
const PT = {
  'phase.check.host': 'Verificando o processador e a placa de vídeo',
  'phase.check.verify': 'Verificando os arquivos já instalados',
  'phase.check.verify_romfs': 'Verificando os dados do jogo instalados',
  'phase.check.inputs': 'Abrindo os arquivos do jogo com as chaves',
  'phase.check.versions': 'Verificando as versões do jogo',
  'phase.extract.hash_base': 'Calculando o checksum do jogo base',
  'phase.extract.hash_update': 'Calculando o checksum da atualização',
  'phase.extract.exefs': 'Extraindo o código do jogo',
  'phase.extract.copy': 'Copiando os dados do jogo',
  'phase.extract.merge': 'Aplicando a atualização aos dados do jogo',
  'phase.extract.diagnose': 'Procurando qual arquivo do jogo está danificado',
  'phase.extract.index': 'Verificando os dados do jogo',
  'phase.lift.discover': 'Encontrando as funções',
  'phase.lift.emit': 'Escrevendo o código C',
  'phase.compile.compile': 'Compilando',
  'phase.compile.link': 'Ligando o totk.exe',
  'phase.prepare.romfs_patches': 'Gerando os patches de 60 fps dos dados do jogo',
  'phase.prepare.seed': 'Lendo os shaders dos dados do jogo',
  'phase.prepare.translate': 'Traduzindo os shaders',
  'phase.prepare.libraries': 'Compilando os shaders para o driver de vídeo',
  'phase.cleanup.ini': 'Escrevendo o totk.ini',
  'phase.cleanup.temp': 'Apagando os arquivos temporários',
};
const UNITS = { chunks: 'blocos', objects: 'objetos', shaders: 'shaders' };
let catalog = {};
function message(id, params = {}) {
  const text = (currentLang() === 'pt' && PT[id]) || catalog[id] || id;
  return text.replace(/\{(\w+)\}/g, (m, k) => (k in params ? String(params[k]) : m));
}
const gb = (b) => `${(b / 1e9).toFixed(1)} GB`;

let STEPS = Object.keys(STEP_INFO), TOTAL = 0;
const build = { step: -1, sub: 0, start: 0, state: [], running: false, error: null, log: null };
function useSteps(ids) {
  STEPS = ids.filter((id) => id in STEP_INFO).concat(ids.filter((id) => !(id in STEP_INFO)));
  TOTAL = STEPS.reduce((a, id) => a + (STEP_INFO[id] ? STEP_INFO[id][2] : 1), 0);
  build.state = STEPS.map(() => '');
  $('#steps').innerHTML = STEPS.map((id) => {
    const [name, what] = STEP_INFO[id] || [message(`step.${id}`), ''];
    return `<li data-step="${esc(id)}"><b>${esc(name)}</b>${esc(what)}<span class="sub"><i></i></span></li>`;
  }).join('');
}
const weight = (id) => (STEP_INFO[id] ? STEP_INFO[id][2] : 1);
function renderBuild() {
  STEPS.forEach((id, i) => {
    const li = $(`#steps li[data-step="${id}"]`);
    li.className = build.state[i];
    li.querySelector('.sub i').style.width = i === build.step && build.state[i] === 'run' ? `${build.sub * 100}%` : '';
  });
  let done = 0;
  STEPS.forEach((id, i) => { if (build.state[i] === 'done') done += weight(id); });
  if (build.step >= 0 && build.state[build.step] === 'run') done += weight(STEPS[build.step]) * build.sub;
  const pct = TOTAL ? Math.min(100, (done / TOTAL) * 100) : 0;
  $('#bar-fill').style.width = `${pct}%`;
  $('#build-pct').textContent = `${Math.floor(pct)}%`;
  if (build.running && pct > 2) {
    const el = (Date.now() - build.start) / 1000, left = el * (100 - pct) / pct;
    $('#build-eta').textContent = left > 60 ? `~${Math.round(left / 60)} min restantes` : '< 1 min';
  } else $('#build-eta').textContent = '';
}
function setBuilding(on) {
  build.running = on;
  $('#build').disabled = on;
  $('#cancel').disabled = !on;
  $('.progress-block .bar').classList.toggle('idle', !on);
  refresh();
}

const logEl = $('#log');
function logLine(text, cls) {
  const span = document.createElement('span');
  if (cls) span.className = cls;
  span.textContent = text + '\n';
  logEl.append(span);
  while (logEl.childNodes.length > 800) logEl.firstChild.remove();
  logEl.scrollTop = logEl.scrollHeight;
}

function onEvent(e) {
  const i = STEPS.indexOf(e.step);
  switch (e.type) {
    case 'start':
      useSteps(e.steps || []);
      build.log = e.log;
      logLine(`Instalador ${e.release} · ${e.mode} · ${e.dest}`, 'step');
      break;
    case 'step':
      if (i < 0) break;
      if (e.state === 'started') {
        build.step = i; build.sub = 0; build.state[i] = 'run';
        $('#build-phase').textContent = STEP_INFO[e.step] ? STEP_INFO[e.step].slice(0, 2).join(' · ') : message(`step.${e.step}`);
        $('#build-activity').textContent = ' ';
        logLine(message(`step.${e.step}`), 'step');
      } else {
        build.state[i] = 'done';
        if (i === build.step) build.sub = 1;
        logLine(`  ${e.state === 'skipped' ? 'já feito' : `${e.seconds.toFixed(0)} s`}`, 'step');
      }
      break;
    case 'progress': {
      let text = message(`phase.${e.step}.${e.phase}`);
      if (e.total) {
        if (i === build.step) build.sub = Math.min(1, e.done / e.total);
        text += e.unit === 'bytes' ? ` · ${gb(e.done)} de ${gb(e.total)}` : ` · ${e.done} de ${e.total} ${UNITS[e.unit] || e.unit || ''}`;
      }
      $('#build-activity').textContent = text;
      break;
    }
    case 'space':
      logLine(`Espaço em ${e.dest}: precisa de ${gb(e.need)}, livres ${gb(e.free)}`);
      break;
    case 'warning':
      logLine(`Aviso: ${message(e.id, e.params)}`, 'err');
      break;
    case 'error':
      build.error = message(e.id, e.params);
      if (i >= 0) build.state[i] = 'fail';
      else if (build.step >= 0 && build.state[build.step] === 'run') build.state[build.step] = 'fail';
      $('.progress-block .bar').classList.add('error');
      $('#build-phase').textContent = 'Erro';
      logLine(build.error, 'err');
      break;
    case 'end':
      logLine(`Fim: ${e.result} (${Math.round(e.seconds)} s)`, 'step');
      break;
  }
  renderBuild();
}

function onBuildLine({ text, err }) {
  const t = text.trim();
  if (!err && t.startsWith('{')) {
    try { return onEvent(JSON.parse(t)); } catch { /* not an event: shown as a line */ }
  }
  if (t) logLine(t, err ? 'err' : '');
}

async function install() {
  await refresh();
  if (status.building) return;
  showView('install');
  if (!status.release) {
    const r = releases.find((x) => !x.prerelease) || releases[0];
    if (!r) return showError('Falta o instalador', releasesError || 'Nenhuma versão do instalador foi encontrada no GitHub.');
    return downloadRelease(r);   // the install starts from the button once the download is done
  }
  const inputs = { ...prefs.inputs, lang: $('#in-lang').value };
  if (!status.has_state) {
    const missing = [['base', 'o jogo base (NSP ou XCI)'], ['update', 'a atualização 1.2.1 ou 1.4.3 (NSP)'], ['keys', 'o prod.keys']]
      .find(([k]) => !inputs[k]);
    if (missing) return showError('Falta escolher', missing[1]);
  }
  useSteps(Object.keys(STEP_INFO));
  Object.assign(build, { step: -1, sub: 0, start: Date.now(), error: null, log: null });
  $('.progress-block .bar').classList.remove('error');
  $('#build-phase').textContent = 'Iniciando…';
  $('#build-activity').textContent = ' ';
  logEl.textContent = '';
  renderBuild();
  try {
    catalog = await invoke('installer_text');
    await invoke('start_build', { inputs });
    setBuilding(true);
  } catch (e) { showError('Não foi possível iniciar a instalação', String(e)); }
}
$('#build').onclick = () => install();
$('#cancel').onclick = () => { $('#build-phase').textContent = 'Cancelando…'; invoke('cancel_build'); };

const PICK = {
  base: ['Jogo base (NSP ou XCI)', ['nsp', 'xci']],
  update: ['Atualização 1.2.1 ou 1.4.3 (NSP)', ['nsp']],
  keys: ['prod.keys', ['keys']],
};
$$('[data-pick]').forEach((b) => (b.onclick = async () => {
  const [title, extensions] = PICK[b.dataset.pick];
  const file = await openDialog({ title, filters: [{ name: title, extensions }, { name: '*', extensions: ['*'] }] });
  if (!file) return;
  prefs.inputs[b.dataset.pick] = file;
  savePrefs();
  renderInstallForm();
}));
$('#find-files').onclick = async () => {
  const dir = await openDialog({ directory: true, title: 'Pasta com o jogo base, a atualização e o prod.keys' });
  if (!dir) return;
  const f = await invoke('find_game_files', { dir });
  const got = ['base', 'update', 'keys'].filter((k) => f[k]);
  for (const k of got) prefs.inputs[k] = f[k];
  savePrefs();
  renderInstallForm();
  const names = { base: 'o jogo base', update: 'a atualização', keys: 'o prod.keys' };
  const miss = ['base', 'update', 'keys'].filter((k) => !f[k]).map((k) => names[k]);
  if (miss.length) showError('Arquivos encontrados em parte', `Não encontrei ${miss.join(', ')} nessa pasta. Escolha ${miss.length > 1 ? 'eles' : 'ele'} com o botão Escolher.`);
};

$('#pick-home').onclick = async () => {
  const dir = await openDialog({ directory: true, title: 'Pasta de instalação (vazia, ou com uma instalação anterior)' });
  if (!dir) return;
  try { await invoke('set_home', { path: dir }); await refresh(); await loadSettings(); }
  catch (e) { showError('Pasta inválida', String(e)); }
};
$('#pick-release').onclick = async () => {
  const dir = await openDialog({ directory: true, title: 'Pasta do instalador (com installer/)' });
  if (!dir) return;
  try { await invoke('set_release', { path: dir }); await refresh(); }
  catch (e) { showError('Pasta inválida', String(e)); }
};

listen('build-line', (e) => onBuildLine(e.payload));
listen('build-done', async (e) => {
  const code = e.payload;
  if (code === 0) {
    build.state = build.state.map((s) => (s === 'fail' ? s : 'done'));
    build.sub = 1;
    $('#build-phase').textContent = 'Concluído';
  } else {
    if (build.step >= 0 && build.state[build.step] === 'run') build.state[build.step] = 'fail';
    $('#build-phase').textContent = code === 3 ? 'Cancelado' : 'Erro';
    $('.progress-block .bar').classList.add('error');
  }
  renderBuild();
  setBuilding(false);
  await loadSettings();   // (the installer wrote totk.ini)
  loadMods();
  if (code !== 0 && code !== 3) {
    showError('A instalação parou', build.error || [...logEl.childNodes].slice(-14).map((n) => n.textContent).join(''), build.log);
  }
});

// ---------------------------------------------------------------- updates
// The launcher's own (tauri-plugin-updater: the GitHub release's latest.json, signed) and the game's: a newer installer
// release than the one that built the install folder. Both show a card on Início.
let launcherUpdate = null;
async function checkUpdates() {
  try { launcherUpdate = await checkLauncherUpdate(); } catch { launcherUpdate = null; }   // (offline, or no release yet)
  renderUpdates();
}
function gameUpdate() {
  const r = releases.find((x) => !x.prerelease);
  return status.installed && r && status.installed_release && newer(r.tag, status.installed_release) ? r : null;
}
function renderUpdates() {
  const box = $('#updates');
  box.textContent = '';
  const card = (title, sub, items, label, action) => {
    const c = document.createElement('div');
    c.className = 'update-card';
    const head = document.createElement('div');
    head.className = 'update-head';
    const t = document.createElement('div');
    const b = document.createElement('b'); b.textContent = title;
    const sm = document.createElement('small'); sm.textContent = sub;
    t.append(b, sm);
    const btn = document.createElement('button');
    btn.className = 'gold';
    btn.textContent = label;
    btn.onclick = () => action(btn);
    head.append(t, btn);
    c.append(head);
    if (items.length) {
      const ul = document.createElement('ul');
      items.slice(0, 4).forEach((x) => { const li = document.createElement('li'); li.textContent = x; ul.append(li); });
      if (items.length > 4) { const li = document.createElement('li'); li.className = 'more'; li.textContent = `+ ${items.length - 4} mudanças`; ul.append(li); }
      c.append(ul);
    }
    box.append(c);
  };
  const notes = (body) => (body || '').split('\n').filter((l) => /^\s*[-*]\s+/.test(l)).map((l) => l.replace(/^\s*[-*]\s*/, '').replace(/\*\*|`/g, '').trim()).filter(Boolean);
  const g = gameUpdate();
  if (g) {
    card(`TotK Recomp ${g.tag}`, `Nova versão do jogo · instalada: ${status.installed_release.split('+')[0]}`, notes(g.notes), 'Atualizar jogo', async () => {
      showView('install');
      const i = releases.indexOf(g);
      $('#rel-list').value = String(i);
      const current = !status.dev && status.release_version && !newer(g.tag, status.release_version) && !newer(status.release_version, g.tag);
      if (!current) await downloadRelease(g);
      else install();
    });
  }
  if (launcherUpdate) {
    card(`Launcher ${launcherUpdate.version}`, 'Nova versão do launcher', notes(launcherUpdate.body), 'Atualizar launcher', async (btn) => {
      btn.disabled = true;
      let got = 0, size = 0;
      try {
        await launcherUpdate.downloadAndInstall((ev) => {
          if (ev.event === 'Started') size = ev.data.contentLength || 0;
          if (ev.event === 'Progress') { got += ev.data.chunkLength; btn.textContent = size ? `${Math.round((got / size) * 100)}%` : '…'; }
        });
        await relaunch();
      } catch (e) { btn.disabled = false; btn.textContent = 'Atualizar launcher'; showError('Não foi possível atualizar o launcher', String(e)); }
    });
  }
}

// ---------------------------------------------------------------- play: transition, hand-over, return
// totk.exe opens its own window (size and fullscreen from totk.ini); the transition covers the wait and the launcher
// hides when the game's window shows ("game-visible"), or closes when Fechar o launcher ao jogar is on.
async function play() {
  if (!status.installed) { install(); return; }
  document.body.classList.add('in-game');
  $('#t-msg').textContent = 'Uma nova jornada começa no céu';
  $('#transition').classList.add('on');
  fadeMusic(0, 1400);
  await new Promise((r) => setTimeout(r, 900));   // the transition fades in first
  try {
    const m = await invoke('mods_list').catch(() => null);
    if (m && m.stale) {   // a mod or the game files changed since the merge
      $('#t-msg').textContent = 'Preparando os mods';
      await invoke('mods_apply', { enabled: m.enabled });
      $('#t-msg').textContent = 'Uma nova jornada começa no céu';
    }
    await invoke('launch_game');
    refresh();
  } catch (e) {
    backFromGame();
    showError('Não foi possível abrir o jogo', String(e));
  }
}
async function backFromGame() {
  document.body.classList.remove('in-game');
  await win.show();
  await win.unminimize();
  await win.setFocus();
  setTimeout(() => $('#transition').classList.remove('on'), 150);
  applyMusic();
  refresh();
  loadSettings();   // (the game's menu and F11 write totk.ini too)
}
$('#play').onclick = play;
listen('game-visible', async () => {
  await new Promise((r) => setTimeout(r, 250));   // the game's first frame is up
  if (prefs.closeOnPlay) return win.close();
  await win.hide();
});
listen('game-exit', async (e) => {
  await backFromGame();
  if (e.payload !== 0) {
    const tail = await invoke('log_tail', { lines: 18 });
    showError(`O jogo fechou com erro (código ${e.payload})`, tail || 'Sem log.');
  }
});

// ---------------------------------------------------------------- game controller (src-tauri pad_thread)
// The d-pad or left stick moves a highlight to the nearest control in that direction; the bottom button presses it,
// the right one goes back (closes a dialog, returns to Início), Start plays. Sliders and lists change with left/right.
let padEl = null;
const PAD_SEL = 'button:not(:disabled):not(.win-btn), select, input[type=range], input[type=text], input:not([type]), textarea, .bind-slot, label.toggle';
function padVisible(el) {
  if (!el || !el.isConnected) return false;
  const r = el.getBoundingClientRect();
  return r.width > 0 && r.height > 0 && !el.closest('.hidden, #min, #max, #close');
}
function padCandidates() {
  const dialog = $('#dialog');
  const root = !dialog.classList.contains('hidden') ? dialog : document;
  return [...root.querySelectorAll(PAD_SEL)].filter((el) => padVisible(el) && !(root === document && el.closest('#dialog')));
}
function padFocus(el) {
  if (padEl) padEl.classList.remove('pad-focus');
  padEl = el;
  if (!el) return;
  el.classList.add('pad-focus');
  el.scrollIntoView({ block: 'nearest', inline: 'nearest' });
}
function padMove(dir) {
  const list = padCandidates();
  if (!padVisible(padEl) || !list.includes(padEl)) return padFocus(list.find((e) => e.id === 'play') || list[0]);
  if (padEl.matches('input[type=range]') && (dir === 'left' || dir === 'right')) {
    if (dir === 'left') padEl.stepDown(); else padEl.stepUp();
    padEl.dispatchEvent(new Event('input')); padEl.dispatchEvent(new Event('change'));
    return;
  }
  if (padEl.matches('select') && (dir === 'left' || dir === 'right')) {
    const i = padEl.selectedIndex + (dir === 'left' ? -1 : 1);
    if (i >= 0 && i < padEl.options.length) { padEl.selectedIndex = i; padEl.dispatchEvent(new Event('change')); }
    return;
  }
  const a = padEl.getBoundingClientRect(), ax = a.left + a.width / 2, ay = a.top + a.height / 2;
  let best = null, bestScore = Infinity;
  for (const el of list) {
    if (el === padEl) continue;
    const b = el.getBoundingClientRect(), dx = b.left + b.width / 2 - ax, dy = b.top + b.height / 2 - ay;
    const along = { up: -dy, down: dy, left: -dx, right: dx }[dir], across = dir === 'up' || dir === 'down' ? Math.abs(dx) : Math.abs(dy);
    if (along <= 4) continue;
    const score = along + across * 2;
    if (score < bestScore) { bestScore = score; best = el; }
  }
  if (best) padFocus(best);
}
function padPress() {
  const el = padEl;
  if (!padVisible(el)) return padMove('down');
  if (el.matches('.bind-slot')) { if (!el.classList.contains('listening')) el.dispatchEvent(new MouseEvent('mousedown', { button: 0, bubbles: true })); return; }
  if (el.matches('input[type=text], input:not([type]), textarea')) { el.focus(); return; }
  if (el.matches('select, input[type=range]')) return;
  el.click();
}
listen('pad', ({ payload }) => {
  if (!document.hasFocus()) return;
  if (!$('#intro').classList.contains('gone')) { dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter' })); return; }
  if (payload === 'ok') return padPress();
  if (payload === 'back') {
    if (!$('#dialog').classList.contains('hidden')) return $('#dialog-ok').click();
    if (listening) return stopListening();
    if (document.activeElement && document.activeElement.matches('input, textarea')) return document.activeElement.blur();
    showView('home');
    return padFocus($('#play'));
  }
  if (payload === 'start') {
    showView('home');
    padFocus($('#play'));
    if (!$('#play').disabled) $('#play').click();
    return;
  }
  padMove(payload);
});
addEventListener('mousemove', () => padFocus(null));

// ---------------------------------------------------------------- opening
// index.html #intro: plays once at start (prefs.intro, Configurações > Launcher); a click or a key skips it.
function playIntro() {
  const el = $('#intro');
  if (prefs.intro === false || matchMedia('(prefers-reduced-motion: reduce)').matches) { el.classList.add('gone'); return; }
  let ended = false;
  const end = () => {
    if (ended) return;
    ended = true;
    el.classList.add('done');
    setTimeout(() => el.classList.add('gone'), 950);
    removeEventListener('keydown', end);
  };
  el.onclick = end;
  addEventListener('keydown', end);
  setTimeout(end, 3600);
}

// ---------------------------------------------------------------- start
// Language (i18n.js): the saved choice, else the system's
setLang(prefs.lang || (navigator.language.toLowerCase().startsWith('pt') ? 'pt' : 'en'));
$('#set-lang').value = prefs.lang || (navigator.language.toLowerCase().startsWith('pt') ? 'pt' : 'en');
$('#set-lang').onchange = (e) => { prefs.lang = e.target.value; savePrefs(); setLang(prefs.lang); };
playIntro();
$('#set-intro').checked = prefs.intro !== false;
$('#set-intro').onchange = (e) => { prefs.intro = e.target.checked; savePrefs(); };
$('#set-music').checked = prefs.music;
$('#set-volume').value = prefs.volume;
$('#set-close').checked = prefs.closeOnPlay;

(async function init() {
  await loadArt();
  buildSlides();
  useSteps(Object.keys(STEP_INFO));
  renderBuild();
  await refresh();
  await loadSettings();
  setBuilding(status.building);
  bgm.volume = 0;
  applyMusic();
  const view = await invoke('start_view');
  if (view && $(`#view-${view}`)) showView(view);
  const lv = await getVersion();
  $('#about-versions').textContent = `Launcher ${lv}` + (status.installed_release ? ` · jogo: ${status.installed_release}` : '');
  await loadSource();
  loadReleases();
  checkUpdates();
})();
