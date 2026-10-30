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
const prefs = Object.assign({ music: true, volume: 35, frameless: false, inputs: {} },
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
$$('.tab').forEach((t) => (t.onclick = () => showView(t.dataset.view)));
$$('[data-set]').forEach((b) => (b.onclick = () => {
  $$('[data-set]').forEach((x) => x.classList.toggle('active', x === b));
  $$('.set-page').forEach((pg) => pg.classList.toggle('active', pg.id === `set-${b.dataset.set}`));
}));

// ---------------------------------------------------------------- status
let status = {};
async function refresh() {
  status = await invoke('status');
  $('#version').textContent = status.installed_release ? `v${status.installed_release}` : '';
  $('#home-path').value = status.home;
  $('#release-path').value = status.release || '';
  $('#release-version').textContent = status.release_version ? `· versão ${status.release_version}${status.dev ? ' (repositório)' : ''}` : '';
  const play = $('#play'), msg = $('#home-status');
  play.disabled = status.building || status.running;
  play.classList.toggle('install', !status.installed);
  play.querySelector('.play-label').textContent = status.installed ? 'Jogar' : 'Instalar';
  if (status.building) msg.innerHTML = 'Instalação em andamento…';
  else if (status.running) msg.innerHTML = 'O jogo está aberto';
  else if (status.installed) msg.innerHTML = `Pronto para jogar · <b>Tears of the Kingdom ${esc(status.game_version || '')}</b>`;
  else if (!status.release) msg.innerHTML = 'Escolha a pasta do instalador em <b>Instalação</b>';
  else msg.innerHTML = 'Jogo ainda não instalado · leva cerca de 30 min';
  renderInstallForm();
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
  $('#in-lang').value = prefs.inputs.lang || status.lang || 'en-US';
  const known = $('#inputs-known');
  known.classList.toggle('hidden', !status.has_state);
  if (status.has_state) {
    const files = [status.base_name, status.update_name].filter(Boolean).map((n) => `<code>${esc(n)}</code>`).join(' + ');
    known.innerHTML = 'Esta pasta já tem uma instalação: Instalar continua ou atualiza, e os arquivos do jogo só são ' +
      `pedidos de novo se a extração precisar rodar outra vez.${files ? `<br>${files}` : ''}`;
  }
}

// ---------------------------------------------------------------- settings (settings.cfg)
const KNOWN = ['TOTK_FPS_OVERLAY', 'TOTK_FULLSCREEN', 'TOTK_GYRO_AXES', 'TOTK_FPS60', 'TOTK_RENDER_RES', 'TOTK_RUMBLE', 'TOTK_GYRO',
  'TOTK_GYRO_DEADZONE', 'TOTK_OPERATION_MODE'];
const isKnown = (k) => KNOWN.includes(k) || k.startsWith('TOTK_KEY_') || MOUSE_KEYS.includes(k);
function showResField() { $('#res-field').classList.toggle('on', $('#set-resmod').checked); }
let settings = {};
async function loadSettings() {
  settings = await invoke('read_settings');
  $('#set-fps').checked = settings.TOTK_FPS_OVERLAY === '1';
  $('#set-fullscreen').checked = settings.TOTK_FULLSCREEN === '1';
  // controls: rumble and gyro are on unless "0" (host/hid_feedback.c); the dead zone empty = the controller's default
  $('#set-rumble').checked = settings.TOTK_RUMBLE !== '0';
  $('#set-gyro-on').checked = settings.TOTK_GYRO !== '0';
  $('#set-handheld').checked = settings.TOTK_OPERATION_MODE === 'handheld';
  $('#set-gyro').value = settings.TOTK_GYRO_AXES || '';
  $('#set-gyro-dz').value = settings.TOTK_GYRO_DEADZONE || 0;
  $('#set-gyro-dz').dataset.auto = settings.TOTK_GYRO_DEADZONE ? '' : '1';
  showGyro();
  loadInput();
  $('#set-fps60').checked = settings.TOTK_FPS60 === '1';
  $('#mod-fps60').classList.toggle('on', $('#set-fps60').checked);
  $('#set-resmod').checked = !!settings.TOTK_RENDER_RES;
  if (settings.TOTK_RENDER_RES) $('#set-res').value = settings.TOTK_RENDER_RES;
  showResField();
  $('#set-extra').value = Object.entries(settings).filter(([k]) => !isKnown(k)).map(([k, v]) => `${k}=${v}`).join('\n');
  $('#set-borderless').checked = prefs.frameless;
  $('#set-music').checked = prefs.music;
  $('#set-volume').value = prefs.volume;
}
let savedTimer;
async function saveSettings() {
  const s = {
    TOTK_FPS_OVERLAY: $('#set-fps').checked ? '1' : '',
    TOTK_FULLSCREEN: $('#set-fullscreen').checked ? '1' : '',
    TOTK_GYRO_AXES: $('#set-gyro').value.trim(),
    TOTK_RUMBLE: $('#set-rumble').checked ? '' : '0',
    TOTK_GYRO: $('#set-gyro-on').checked ? '' : '0',
    TOTK_GYRO_DEADZONE: $('#set-gyro-dz').dataset.auto ? '' : String(+$('#set-gyro-dz').value),
    TOTK_OPERATION_MODE: $('#set-handheld').checked ? 'handheld' : '',
    TOTK_FPS60: $('#set-fps60').checked ? '1' : '',
    TOTK_RENDER_RES: $('#set-resmod').checked ? $('#set-res').value : '',
    ...inputSettings(),
  };
  for (const line of $('#set-extra').value.split('\n')) {
    const m = line.trim().match(/^(TOTK_[A-Z0-9_]+)\s*=\s*(.*)$/);
    if (m) s[m[1]] = m[2];
  }
  try {
    await invoke('write_settings', { settings: s });
    settings = s;
    $('#saved').classList.add('on');
    clearTimeout(savedTimer);
    savedTimer = setTimeout(() => $('#saved').classList.remove('on'), 1400);
  } catch (e) { showError('Não foi possível salvar', String(e)); }
}
['#set-fps', '#set-fullscreen', '#set-fps60', '#set-rumble', '#set-handheld'].forEach((s) => ($(s).onchange = saveSettings));
// Gyro: its options only matter with it on; the axes field follows the preset (or is typed: "custom")
function showGyro() {
  $('#set-control').classList.toggle('disabled-gyro', !$('#set-gyro-on').checked);
  const dz = $('#set-gyro-dz');
  $('#gyro-dz-val').textContent = dz.dataset.auto ? 'automática' : `${(+dz.value).toFixed(1)} °/s`;
  const v = $('#set-gyro').value.trim(), preset = $('#set-gyro-preset');
  preset.value = [...preset.options].some((o) => o.value === v && v !== 'custom') ? v : 'custom';
  $('#set-gyro').classList.toggle('hidden', preset.value !== 'custom');
}
$('#set-gyro-on').onchange = () => { showGyro(); saveSettings(); };
$('#set-gyro-dz').oninput = (e) => { e.target.dataset.auto = ''; showGyro(); };
$('#set-gyro-dz').onchange = saveSettings;
$('#gyro-dz-auto').onclick = () => { $('#set-gyro-dz').dataset.auto = '1'; $('#set-gyro-dz').value = 0; showGyro(); saveSettings(); };
$('#set-gyro-preset').onchange = (e) => {
  if (e.target.value !== 'custom') $('#set-gyro').value = e.target.value;
  $('#set-gyro').classList.toggle('hidden', e.target.value !== 'custom');
  if (e.target.value === 'custom') $('#set-gyro').focus(); else saveSettings();
};
['#set-gyro', '#set-extra'].forEach((s) => ($(s).onchange = saveSettings));

// Keyboard and mouse (host/keyboard_input.c, host/mouse_camera.c: upstream PR #20). Bindings are SDL scancodes, two per
// action, 512-514 the mouse buttons (src/host/keyboard_bindings.def; only those differing from it are saved).
const MOUSE_KEYS = ['TOTK_CAMERA_MODE', 'TOTK_MOUSE_CAMERA_VERSION', 'TOTK_MOUSE_BACKEND', 'TOTK_MOUSE_LOOK_DEG_PER_COUNT',
  'TOTK_MOUSE_AIM_MULTIPLIER', 'TOTK_MOUSE_SCOPE_MULTIPLIER', 'TOTK_MOUSE_SMOOTH_MS', 'TOTK_MOUSE_INVERT_Y', 'TOTK_MOUSE_SENSITIVITY'];
const ACTIONS = [   // [id, group, name, what it does, default 1, default 2]
  ['MOVE_UP', 'Movimento', 'Frente', 'Analógico esquerdo para cima', 26, 0],
  ['MOVE_DOWN', 'Movimento', 'Trás', 'Analógico esquerdo para baixo', 22, 0],
  ['MOVE_LEFT', 'Movimento', 'Esquerda', 'Analógico esquerdo para a esquerda', 4, 0],
  ['MOVE_RIGHT', 'Movimento', 'Direita', 'Analógico esquerdo para a direita', 7, 0],
  ['LOOK_UP', 'Câmera', 'Olhar para cima', 'Também move a luneta e seleções', 82, 0],
  ['LOOK_DOWN', 'Câmera', 'Olhar para baixo', '', 81, 0],
  ['LOOK_LEFT', 'Câmera', 'Olhar à esquerda', '', 80, 0],
  ['LOOK_RIGHT', 'Câmera', 'Olhar à direita', '', 79, 0],
  ['A', 'Ações', 'A · Interagir', 'Pegar, falar, confirmar', 8, 0],
  ['B', 'Ações', 'B · Correr / cancelar', 'Correr (segurar), guardar a arma', 225, 41],
  ['X', 'Ações', 'X · Pular', 'Parapente no ar', 44, 0],
  ['Y', 'Ações', 'Y · Atacar', 'Ataque carregado (segurar)', 512, 9],
  ['L', 'Ações', 'L · Habilidade', 'Roda de habilidades (segurar)', 6, 0],
  ['R', 'Ações', 'R · Arremessar', 'Mira o arremesso, solta para jogar', 23, 0],
  ['ZL', 'Ações', 'ZL · Focar / escudo', 'Travar no alvo, erguer o escudo', 514, 20],
  ['ZR', 'Ações', 'ZR · Arco', 'Puxar (segurar), soltar dispara', 513, 21],
  ['STICK_L', 'Ações', 'Agachar', 'Clique do analógico esquerdo', 224, 0],
  ['STICK_R', 'Ações', 'Luneta', 'Clique do analógico direito', 226, 0],
  ['PLUS', 'Menus', '+ · Inventário', 'Bolsa e pausa', 43, 0],
  ['MINUS', 'Menus', '− · Purah Pad', 'Mapa e diário', 53, 16],
  ['DPAD_UP', 'Menus rápidos', 'Materiais', 'Direcional para cima (segurar)', 25, 0],
  ['DPAD_DOWN', 'Menus rápidos', 'Assobiar', 'Chama o cavalo', 10, 0],
  ['DPAD_LEFT', 'Menus rápidos', 'Escudos', 'Direcional para a esquerda (segurar)', 29, 0],
  ['DPAD_RIGHT', 'Menus rápidos', 'Armas / arcos', 'Direcional para a direita (segurar)', 27, 0],
];
const RESERVED = new Set([58, 59, 60, 65, 68]);   // F1-F3 (overlays, 60 FPS), F8 (cursor), F11 (fullscreen)
const SDL_CODE = (() => {   // KeyboardEvent.code -> SDL scancode (USB HID usage)
  const m = { Enter: 40, Escape: 41, Backspace: 42, Tab: 43, Space: 44, Minus: 45, Equal: 46, BracketLeft: 47, BracketRight: 48,
    Backslash: 49, Semicolon: 51, Quote: 52, Backquote: 53, Comma: 54, Period: 55, Slash: 56, CapsLock: 57, PrintScreen: 70,
    ScrollLock: 71, Pause: 72, Insert: 73, Home: 74, PageUp: 75, Delete: 76, End: 77, PageDown: 78, ArrowRight: 79,
    ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82, NumLock: 83, NumpadDivide: 84, NumpadMultiply: 85, NumpadSubtract: 86,
    NumpadAdd: 87, NumpadEnter: 88, Numpad0: 98, NumpadDecimal: 99, IntlBackslash: 100, ControlLeft: 224, ShiftLeft: 225,
    AltLeft: 226, MetaLeft: 227, ControlRight: 228, ShiftRight: 229, AltRight: 230, MetaRight: 231 };
  for (let i = 0; i < 26; i++) m['Key' + String.fromCharCode(65 + i)] = 4 + i;
  for (let i = 1; i <= 9; i++) { m['Digit' + i] = 29 + i; m['Numpad' + i] = 88 + i; }
  m.Digit0 = 39;
  for (let i = 1; i <= 12; i++) m['F' + i] = 57 + i;
  return m;
})();
const MAC = navigator.platform.startsWith('Mac');
function keyName(code) {
  if (!code) return '—';
  if (code === 512) return 'Mouse esq.';
  if (code === 513) return 'Mouse dir.';
  if (code === 514) return 'Mouse meio';
  const named = { 40: MAC ? 'Return' : 'Enter', 41: 'Esc', 42: 'Backspace', 43: 'Tab', 44: 'Espaço', 57: 'Caps Lock', 79: '→', 80: '←',
    81: '↓', 82: '↑', 224: 'Ctrl esq.', 225: 'Shift esq.', 226: MAC ? 'Option esq.' : 'Alt esq.', 227: MAC ? 'Cmd esq.' : 'Win esq.',
    228: 'Ctrl dir.', 229: 'Shift dir.', 230: MAC ? 'Option dir.' : 'Alt dir.', 231: MAC ? 'Cmd dir.' : 'Win dir.', 53: '`' };
  if (named[code]) return named[code];
  if (code >= 4 && code <= 29) return String.fromCharCode(61 + code);
  if (code >= 30 && code <= 39) return String((code - 29) % 10);
  if (code >= 58 && code <= 69) return 'F' + (code - 57);
  const back = Object.entries(SDL_CODE).find(([, v]) => v === code);
  return back ? back[0].replace(/^Numpad/, 'Num ') : `Tecla ${code}`;
}
let binds = {};   // id -> [code, code]
let listening = null;   // {id, slot, el}
function loadInput() {
  $('#set-cam').value = settings.TOTK_CAMERA_MODE === 'mouse' ? 'mouse' : '';
  $('#set-mouse-sens').value = settings.TOTK_MOUSE_LOOK_DEG_PER_COUNT || 0.04;
  $('#set-mouse-aim').value = settings.TOTK_MOUSE_AIM_MULTIPLIER || 1;
  $('#set-mouse-scope').value = settings.TOTK_MOUSE_SCOPE_MULTIPLIER || 1;
  $('#set-mouse-smooth').value = ['5', '10'].includes(settings.TOTK_MOUSE_SMOOTH_MS) ? settings.TOTK_MOUSE_SMOOTH_MS : '0';
  $('#set-mouse-invert').checked = settings.TOTK_MOUSE_INVERT_Y === '1';
  binds = {};
  for (const [id, , , , d1, d2] of ACTIONS) {
    const m = (settings['TOTK_KEY_' + id] || '').match(/^(\d+),(\d+)$/);
    binds[id] = m ? [+m[1], +m[2]] : [d1, d2];
  }
  showInput();
}
function inputSettings() {
  const mouse = $('#set-cam').value === 'mouse', s = {};
  s.TOTK_CAMERA_MODE = mouse ? 'mouse' : '';
  s.TOTK_MOUSE_CAMERA_VERSION = mouse ? '2' : '';
  const num = (sel, def) => (+$(sel).value === def ? '' : String(+$(sel).value));
  s.TOTK_MOUSE_LOOK_DEG_PER_COUNT = num('#set-mouse-sens', 0.04);
  s.TOTK_MOUSE_AIM_MULTIPLIER = num('#set-mouse-aim', 1);
  s.TOTK_MOUSE_SCOPE_MULTIPLIER = num('#set-mouse-scope', 1);
  s.TOTK_MOUSE_SMOOTH_MS = $('#set-mouse-smooth').value === '0' ? '' : $('#set-mouse-smooth').value;
  s.TOTK_MOUSE_INVERT_Y = $('#set-mouse-invert').checked ? '1' : '';
  for (const [id, , , , d1, d2] of ACTIONS) {
    const [a, b] = binds[id];
    s['TOTK_KEY_' + id] = a === d1 && b === d2 ? '' : `${a},${b}`;
  }
  return s;
}
function showInput() {
  $('#set-control').classList.toggle('keys-camera', $('#set-cam').value !== 'mouse');
  $('#mouse-sens-val').textContent = `${(+$('#set-mouse-sens').value).toFixed(3)} °/ponto`;
  $('#mouse-aim-val').textContent = `${(+$('#set-mouse-aim').value).toFixed(2)}×`;
  $('#mouse-scope-val').textContent = `${(+$('#set-mouse-scope').value).toFixed(2)}×`;
  const box = $('#bindings');
  box.textContent = '';
  let group = '';
  for (const [id, g, name, what] of ACTIONS) {
    if (g !== group) { group = g; const h = document.createElement('h4'); h.textContent = g; box.append(h); }
    const row = document.createElement('div');
    row.className = 'bind-row';
    const label = document.createElement('div');
    label.className = 'what';
    label.textContent = name;
    if (what) { const sm = document.createElement('small'); sm.textContent = what; label.append(sm); }
    row.append(label);
    binds[id].forEach((code, slot) => {
      const el = document.createElement('div');
      el.className = 'bind-slot' + (code ? '' : ' empty');
      el.tabIndex = 0;
      el.textContent = keyName(code);
      const x = document.createElement('button');
      x.className = 'clear';
      x.title = 'Apagar';
      x.textContent = '×';
      x.onmousedown = (e) => e.stopPropagation();
      x.onclick = (e) => { e.stopPropagation(); stopListening(); binds[id][slot] = 0; showInput(); saveSettings(); };
      el.append(x);
      el.onmousedown = (e) => {
        if (listening && listening.el === el) { e.preventDefault(); assign([512, 514, 513][e.button]); return; }
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
  showInput();
}
function assign(code) {
  if (!listening || code === undefined) return;
  const { id, slot } = listening;
  for (const k in binds) binds[k] = binds[k].map((c) => (c === code ? 0 : c));   // one action per key (as the game does)
  binds[id][slot] = code;
  listening = null;
  showInput();
  saveSettings();
}
addEventListener('keydown', (e) => {
  if (!listening) return;
  e.preventDefault();
  e.stopPropagation();
  const code = SDL_CODE[e.code];
  if (code && !RESERVED.has(code)) assign(code);
}, true);
addEventListener('mousedown', (e) => { if (listening && !listening.el.contains(e.target)) stopListening(); });
$('#keys-reset').onclick = () => {
  stopListening();
  for (const [id, , , , d1, d2] of ACTIONS) binds[id] = [d1, d2];
  showInput();
  saveSettings();
};
$('#set-cam').onchange = () => { showInput(); saveSettings(); };
['#set-mouse-sens', '#set-mouse-aim', '#set-mouse-scope'].forEach((sel) => { $(sel).oninput = showInput; $(sel).onchange = saveSettings; });
['#set-mouse-smooth', '#set-mouse-invert'].forEach((sel) => ($(sel).onchange = saveSettings));
// The resolution mod: the heap overlay for the chosen size is made first (tools/make_res_overlay.py), then saved.
async function applyRes() {
  showResField();
  const status = $('#res-status');
  if ($('#set-resmod').checked) {
    status.textContent = 'Preparando a memória do jogo para essa resolução…';
    try {
      await invoke('make_res_overlay', { size: $('#set-res').value });
      status.textContent = 'Pronto: vale na próxima vez que o jogo abrir.';
    } catch (e) {
      $('#set-resmod').checked = false;
      showResField();
      status.textContent = '';
      showError('Não foi possível preparar a resolução', String(e));
    }
  } else status.textContent = '';
  await saveSettings();
}
$('#set-resmod').onchange = applyRes;
$('#set-res').onchange = applyRes;
$('#set-borderless').onchange = (e) => { prefs.frameless = e.target.checked; savePrefs(); };
$('#set-music').onchange = (e) => { prefs.music = e.target.checked; savePrefs(); applyMusic(); };
$('#set-volume').oninput = (e) => { prefs.volume = +e.target.value; savePrefs(); if (musicOn()) bgm.volume = prefs.volume / 100; };
$$('[data-url]').forEach((b) => (b.onclick = () => invoke('open_path', { path: b.dataset.url })));
$$('[data-open]').forEach((b) => (b.onclick = () => invoke('open_path', { path: b.dataset.open }).catch((e) => showError('Não foi possível abrir', String(e)))));

// ---------------------------------------------------------------- dialog
let dialogLog = null;   // the log the dialog's button opens: the installer's, else the game's
function showError(title, body, log = null) {
  $('#dialog-title').textContent = title;
  $('#dialog-body').textContent = body;
  dialogLog = log;
  $('#dialog').classList.remove('hidden');
}
$('#dialog-ok').onclick = () => $('#dialog').classList.add('hidden');
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
  $('.bar').classList.toggle('idle', !on);
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
        $('#build-activity').textContent = ' ';
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
      $('.bar').classList.add('error');
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
  if (!status.release) return showError('Falta escolher', 'A pasta do instalador (o release descompactado, com installer/).');
  const inputs = { ...prefs.inputs, lang: $('#in-lang').value };
  if (!status.has_state) {
    const missing = [['base', 'o jogo base (NSP ou XCI)'], ['update', 'a atualização 1.2.1 ou 1.4.3 (NSP)'], ['keys', 'o prod.keys']]
      .find(([k]) => !inputs[k]);
    if (missing) return showError('Falta escolher', missing[1]);
  }
  useSteps(Object.keys(STEP_INFO));
  Object.assign(build, { step: -1, sub: 0, start: Date.now(), error: null, log: null });
  $('.bar').classList.remove('error');
  $('#build-phase').textContent = 'Iniciando…';
  $('#build-activity').textContent = ' ';
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
const LANGS = [['en-US', 'English (US)'], ['en-GB', 'English (UK)'], ['ja', '日本語'], ['fr', 'Français'],
  ['fr-CA', 'Français (Canada)'], ['de', 'Deutsch'], ['it', 'Italiano'], ['es', 'Español'],
  ['es-419', 'Español (Latinoamérica)'], ['nl', 'Nederlands'], ['ru', 'Русский'], ['ko', '한국어'],
  ['zh-CN', '简体中文'], ['zh-TW', '繁體中文']];
$('#in-lang').innerHTML = LANGS.map(([v, n]) => `<option value="${v}">${n}</option>`).join('');
$('#in-lang').onchange = (e) => { prefs.inputs.lang = e.target.value; savePrefs(); };

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
    $('.bar').classList.add('error');
  }
  renderBuild();
  setBuilding(false);
  if (code !== 0 && code !== 3) {
    showError('A instalação parou', build.error || [...logEl.childNodes].slice(-14).map((n) => n.textContent).join(''), build.log);
  }
});

// ---------------------------------------------------------------- updates
// The launcher's own (tauri-plugin-updater: the GitHub release's latest.json, signed). Checked once at start; it
// shows a card on Início.
let launcherUpdate = null;
async function checkUpdates() {
  try { launcherUpdate = await checkLauncherUpdate(); } catch { launcherUpdate = null; }   // (offline, or no release yet)
  renderUpdates();
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
  if (launcherUpdate) {
    const notes = (launcherUpdate.body || '').split('\n').map((l) => l.replace(/^[-*]\s*/, '').trim()).filter(Boolean);
    card(`Launcher ${launcherUpdate.version}`, 'Nova versão do launcher', notes, 'Atualizar launcher', async (btn) => {
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
async function play() {
  if (!status.installed) { showView('install'); return; }
  const fullscreen = $('#set-fullscreen').checked;
  document.body.classList.add('in-game');
  $('#t-msg').textContent = 'Uma nova jornada começa no céu';
  $('#transition').classList.add('on');
  fadeMusic(0, 1400);
  if (fullscreen) await win.setFullscreen(true);
  await new Promise((r) => setTimeout(r, 1000));   // the transition fades in first
  const pos = await win.outerPosition(), size = await win.outerSize();
  try {
    await invoke('launch_game', {
      rect: [pos.x, pos.y, size.width, size.height], fullscreen, borderless: prefs.frameless, lang: currentLang(),
    });
    watchStartup();
    refresh();
  } catch (e) {
    backFromGame();
    showError('Não foi possível abrir o jogo', String(e));
  }
}
async function backFromGame() {
  document.body.classList.remove('in-game');
  if (await win.isFullscreen()) await win.setFullscreen(false);
  await win.show();
  await win.unminimize();
  await win.setFocus();
  setTimeout(() => $('#transition').classList.remove('on'), 150);
  applyMusic();
  refresh();
}
$('#play').onclick = play;

// The game's start-up under the transition (startup.txt via main.rs startup_status): what it is doing while its window
// stays hidden - preparing, then compiling the shaders it made ahead (n / total) - until it shows ("ready").
let startupTimer;
function watchStartup() {
  const box = $('#t-status'), text = $('#t-status-text'), bar = $('#t-bar');
  const t0 = Date.now();
  clearInterval(startupTimer);
  box.classList.remove('on', 'busy');
  startupTimer = setInterval(async () => {
    let s = '';
    try { s = await invoke('startup_status'); } catch { /* not yet */ }
    const m = s.match(/^shaders (\d+) (\d+)/);
    if (s.startsWith('ready') || !document.body.classList.contains('in-game')) { clearInterval(startupTimer); box.classList.remove('on'); return; }
    if (m && +m[2]) {
      box.classList.add('on'); box.classList.remove('busy');
      const n = Math.min(+m[1], +m[2]);
      text.textContent = `Compilando shaders · ${n.toLocaleString('pt-BR')} / ${(+m[2]).toLocaleString('pt-BR')}`;
      bar.style.width = `${(n / +m[2]) * 100}%`;
    } else if (Date.now() - t0 > 1500) {   // (the tagline alone first)
      box.classList.add('on', 'busy');
      text.textContent = 'Preparando o jogo';
    }
  }, 250);
}
listen('game-visible', async () => {
  await new Promise((r) => setTimeout(r, 250));   // the game's first frame is up behind us
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

// ---------------------------------------------------------------- start
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
// Language (i18n.js): the saved choice, else the system's
setLang(prefs.lang || (navigator.language.toLowerCase().startsWith('pt') ? 'pt' : 'en'));
$('#set-lang').value = prefs.lang || (navigator.language.toLowerCase().startsWith('pt') ? 'pt' : 'en');
$('#set-lang').onchange = (e) => { prefs.lang = e.target.value; savePrefs(); setLang(prefs.lang); };
playIntro();
$('#set-intro').checked = prefs.intro !== false;
$('#set-intro').onchange = (e) => { prefs.intro = e.target.checked; savePrefs(); };

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
  checkUpdates();
})();
