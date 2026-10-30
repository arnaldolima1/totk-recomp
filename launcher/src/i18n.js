// Languages: the page is written in Portuguese; English replaces each text it shows. Every text node and the title /
// placeholder attributes go through EN (exact, whitespace-normalized) or PATTERNS (texts with numbers or names in
// them), including what main.js writes later (a MutationObserver), so main.js needs no wrapping of its strings.
// Choosing Português again puts the original texts back.

const EN = {
  // window, tabs, home
  'Uma nova jornada começa no céu': 'A new journey begins in the sky',
  'clique para pular': 'click to skip',
  'Início': 'Home',
  'Instalação': 'Installation',
  'Configurações': 'Settings',
  'Sobre': 'About',
  'Música': 'Music',
  'Minimizar': 'Minimize',
  'Maximizar': 'Maximize',
  'Fechar': 'Close',
  'Port nativo para PC · Vulkan': 'Native PC port · Vulkan',
  'Jogar': 'Play',
  'Verificando instalação…': 'Checking the installation…',
  'Instalação em andamento…': 'Installation in progress…',
  'O jogo está aberto': 'The game is running',
  'Pronto para jogar ·': 'Ready to play ·',
  'Escolha a pasta do jogo em': 'Choose the game folder in',
  'Jogo ainda não instalado · leva cerca de 30 min': 'Game not installed yet · takes about 30 min',
  'Preparando o jogo': 'Preparing the game',
  'Sem log.': 'No log.',

  // installation
  'Traduz e compila o jogo a partir dos': 'Translates and builds the game from',
  'seus': 'your own',
  'Alterar': 'Change',
  'e': 'and',
  'com': 'with',
  'Escolher': 'Choose',
  'Instalar / Atualizar': 'Install / Update',
  'Instalar': 'Install',
  'Cancelar': 'Cancel',
  'Parado': 'Idle',
  'Registro detalhado': 'Detailed log',
  'Extrair': 'Extract',
  'Verificar': 'Check',
  'Traduzir': 'Translate',
  'código → C': 'code → C',
  'Compilar': 'Build',
  'para Vulkan': 'for Vulkan',
  'Concluir': 'Finish',
  'Iniciando…': 'Starting…',
  'Cancelado': 'Cancelled',
  'Erro': 'Error',
  'Concluído': 'Done',
  'A instalação parou': 'The installation stopped',
  'Falta escolher': 'Not chosen yet',
  'Pasta inválida': 'Invalid folder',
  'arquivos (base 1.0.0 com a atualização 1.2.1 ou 1.4.3). Roda uma vez; o jogo fica na pasta de instalação.':
    'files (base 1.0.0 with the 1.2.1 or 1.4.3 update). It runs once; the game goes to the install folder.',
  'Pasta de instalação': 'Install folder',
  'Pasta do instalador': 'Installer folder',
  'o release descompactado (com installer/)': 'the unpacked release (with installer/)',
  'Jogo base (NSP ou XCI)': 'Base game (NSP or XCI)',
  'Atualização 1.2.1 ou 1.4.3 (NSP)': 'Update 1.2.1 or 1.4.3 (NSP)',
  'Idioma do jogo': 'Game language',
  'As chaves são lidas no lugar pelo instalador: o conteúdo delas nunca é mostrado, copiado nem guardado.':
    'The installer reads the keys in place: their contents are never shown, copied or stored.',
  'PC e arquivos': 'PC and files',
  'NSP / XCI': 'NSP / XCI',
  'gerar o totk.exe': 'build totk.exe',
  'limpeza': 'clean up',
  'Cancelando…': 'Cancelling…',
  'já feito': 'already done',
  'o jogo base (NSP ou XCI)': 'the base game (NSP or XCI)',
  'a atualização 1.2.1 ou 1.4.3 (NSP)': 'the 1.2.1 or 1.4.3 update (NSP)',
  'o prod.keys': 'prod.keys',
  'A pasta do instalador (o release descompactado, com installer/).': 'The installer folder (the unpacked release, with installer/).',
  'Pasta de instalação (vazia, ou com uma instalação anterior)': 'Install folder (empty, or with an earlier installation)',
  'Pasta do instalador (com installer/)': 'Installer folder (with installer/)',
  'Escolha a pasta do instalador em': 'Choose the installer folder in',
  'Esta pasta já tem uma instalação: Instalar continua ou atualiza, e os arquivos do jogo só são pedidos de novo se a extração precisar rodar outra vez.':
    'This folder already has an installation: Install continues or updates it, and the game files are asked for again only if the extraction has to run again.',
  'A pasta de instalação precisa estar vazia ou ter uma instalação anterior (totk-install.json).':
    'The install folder must be empty or hold an earlier installation (totk-install.json).',
  'Essa pasta não tem o instalador (installer/).': 'This folder has no installer (installer/).',
  'Escolha a pasta do instalador (o release, com installer/).': 'Choose the installer folder (the release, with installer/).',
  'Não foi possível iniciar a instalação': 'Could not start the installation',

  // mods
  'Melhorias aplicadas pelo port na hora de abrir o jogo. Nenhum arquivo do jogo é modificado.':
    'Improvements the port applies when the game starts. No game file is changed.',
  'Experimental': 'Experimental',
  'Dobra a taxa de quadros do jogo de 30 para 60, com a física, as animações e a passagem do tempo ajustadas para continuar na velocidade certa.':
    'Doubles the frame rate from 30 to 60, with physics, animations and the passing of time adjusted to keep the right speed.',
  'Desempenho': 'Performance',
  'Requer CPU forte': 'Needs a strong CPU',
  'Ligar / desligar': 'On / off',
  'Resolução maior': 'Higher resolution',
  'Renderiza a cena acima dos 1600×900 do Switch (até 4K), com a memória gráfica do jogo aumentada na medida. Ligado, a resolução é escolhida em':
    'Renders the scene above the Switch\'s 1600×900 (up to 4K), with the game\'s graphics memory enlarged to match. When on, the resolution is chosen in',
  'Qualidade': 'Quality',
  'Requer GPU forte': 'Needs a strong GPU',
  'Mais mods em breve': 'More mods soon',
  'Distância de visão e outras melhorias vão aparecer aqui.': 'View distance and other improvements will show up here.',
  'Preparando a memória do jogo para essa resolução…': 'Preparing the game\'s memory for this resolution…',
  'Pronto: vale na próxima vez que o jogo abrir.': 'Done: it applies the next time the game starts.',
  'Não foi possível preparar a resolução': 'Could not prepare the resolution',

  // settings: display
  'Exibição': 'Display',
  'Controle': 'Controls',
  'Avançado': 'Advanced',
  'Contador de FPS': 'FPS counter',
  'Mostra o FPS no canto do jogo.': 'Shows the FPS in the corner of the game.',
  'liga/desliga durante o jogo.': 'toggles it in game.',
  'Tela cheia': 'Fullscreen',
  'Abre o jogo em tela cheia.': 'Opens the game in fullscreen.',
  'alterna durante o jogo.': 'switches in game.',
  'Janela sem borda': 'Borderless window',
  'Tira a barra de título do jogo (minimizar, maximizar, fechar). Desligado: janela normal.':
    'Removes the game\'s title bar (minimize, maximize, close). Off: a normal window.',
  'Modo portátil': 'Handheld mode',
  'O jogo se comporta como no Switch fora da base: saída 1280×720 e resolução interna menor. Mais leve em máquinas fracas.':
    'The game behaves as on an undocked Switch: 1280×720 output and a lower internal resolution. Lighter on weak machines.',
  'Resolução da cena': 'Scene resolution',
  '(mod Resolução maior)': '(Higher resolution mod)',
  '2160p · 4K (3840×2160) — experimental': '2160p · 4K (3840×2160) — experimental',

  // settings: controls
  'Controle de jogo': 'Game controller',
  'Qualquer controle que o sistema reconheça (Xbox, PlayStation, Switch Pro, 8BitDo). Os botões seguem o Switch:':
    'Any controller the system recognizes (Xbox, PlayStation, Switch Pro, 8BitDo). Buttons follow the Switch:',
  'é o da direita,': 'is the right one,',
  'o de baixo.': 'the bottom one.',
  'Vibração': 'Rumble',
  'O jogo vibra o controle (quando ele tem motores).': 'The game rumbles the controller (when it has motors).',
  'Giroscópio': 'Gyro',
  'Mira e quebra-cabeças por movimento, com controles que têm sensor (DualSense, Switch Pro).':
    'Motion aiming and puzzles, with controllers that have the sensor (DualSense, Switch Pro).',
  'Zona morta:': 'Dead zone:',
  '(rotação mais lenta que isso conta como parado; tira o tremor da mão)': '(slower rotation counts as still; removes hand shake)',
  'automática': 'automatic',
  'Automático': 'Automatic',
  'Eixos': 'Axes',
  '(se a mira gira para o lado errado)': '(if aiming turns the wrong way)',
  'Padrão do controle': 'Controller default',
  'Inverter vertical': 'Invert vertical',
  'Inverter horizontal': 'Invert horizontal',
  'Inverter os dois': 'Invert both',
  'Personalizado…': 'Custom…',
  'ex.: -x,y,-z': 'e.g. -x,y,-z',
  'Teclado e mouse': 'Keyboard and mouse',
  'Câmera': 'Camera',
  'Teclas (setas)': 'Keys (arrows)',
  'Sensibilidade:': 'Sensitivity:',
  'Mirando (arco, armas):': 'Aiming (bow, weapons):',
  'Luneta:': 'Scope:',
  'Suavização': 'Smoothing',
  'Nenhuma': 'None',
  'Leve (5 ms)': 'Light (5 ms)',
  'Média (10 ms)': 'Medium (10 ms)',
  'Mouse para cima olha para baixo.': 'Mouse up looks down.',
  'No jogo o cursor fica preso na janela:': 'In game the cursor is held in the window:',
  'solta e prende de novo (clicar na janela também prende). Com o mouse na câmera, o giroscópio do controle não gira a câmera.':
    'releases and holds it again (clicking the window holds it too). With the mouse on the camera, the controller\'s gyro does not turn the camera.',
  'Clique num campo e aperte a tecla ou o botão do mouse. Clique fora para cancelar;':
    'Click a field and press the key or mouse button. Click elsewhere to cancel;',
  'apaga. Um controle conectado funciona junto.': 'clears it. A connected controller works alongside.',
  'No jogo': 'In game',
  'Painel de desempenho': 'Performance panel',
  'Soltar / prender o cursor': 'Release / hold the cursor',
  'Restaurar padrão': 'Restore defaults',
  'Apagar': 'Clear',
  'Aperte…': 'Press…',
  'Movimento': 'Movement',
  'Frente': 'Forward',
  'Trás': 'Back',
  'Esquerda': 'Left',
  'Direita': 'Right',
  'Analógico esquerdo para cima': 'Left stick up',
  'Analógico esquerdo para baixo': 'Left stick down',
  'Analógico esquerdo para a esquerda': 'Left stick left',
  'Analógico esquerdo para a direita': 'Left stick right',
  'Olhar para cima': 'Look up',
  'Também move a luneta e seleções': 'Also moves the scope and selections',
  'Olhar para baixo': 'Look down',
  'Olhar à esquerda': 'Look left',
  'Olhar à direita': 'Look right',
  'Ações': 'Actions',
  'A · Interagir': 'A · Interact',
  'Pegar, falar, confirmar': 'Pick up, talk, confirm',
  'B · Correr / cancelar': 'B · Sprint / cancel',
  'Correr (segurar), guardar a arma': 'Sprint (hold), put the weapon away',
  'X · Pular': 'X · Jump',
  'Parapente no ar': 'Paraglider in the air',
  'Y · Atacar': 'Y · Attack',
  'Ataque carregado (segurar)': 'Charged attack (hold)',
  'L · Habilidade': 'L · Ability',
  'Roda de habilidades (segurar)': 'Ability wheel (hold)',
  'R · Arremessar': 'R · Throw',
  'Mira o arremesso, solta para jogar': 'Aims the throw, release to throw',
  'ZL · Focar / escudo': 'ZL · Focus / shield',
  'Travar no alvo, erguer o escudo': 'Lock on, raise the shield',
  'ZR · Arco': 'ZR · Bow',
  'Puxar (segurar), soltar dispara': 'Draw (hold), release to fire',
  'Agachar': 'Crouch',
  'Clique do analógico esquerdo': 'Left stick press',
  'Luneta': 'Scope',
  'Clique do analógico direito': 'Right stick press',
  'Menus': 'Menus',
  '+ · Inventário': '+ · Inventory',
  'Bolsa e pausa': 'Pouch and pause',
  '− · Purah Pad': '− · Purah Pad',
  'Mapa e diário': 'Map and adventure log',
  'Menus rápidos': 'Quick menus',
  'Materiais': 'Materials',
  'Direcional para cima (segurar)': 'D-pad up (hold)',
  'Assobiar': 'Whistle',
  'Chama o cavalo': 'Calls your horse',
  'Escudos': 'Shields',
  'Direcional para a esquerda (segurar)': 'D-pad left (hold)',
  'Armas / arcos': 'Weapons / bows',
  'Direcional para a direita (segurar)': 'D-pad right (hold)',
  'Espaço': 'Space',
  'Mouse esq.': 'Left mouse',
  'Mouse dir.': 'Right mouse',
  'Mouse meio': 'Middle mouse',
  'Ctrl esq.': 'Left Ctrl',
  'Ctrl dir.': 'Right Ctrl',
  'Shift esq.': 'Left Shift',
  'Shift dir.': 'Right Shift',
  'Option esq.': 'Left Option',
  'Option dir.': 'Right Option',
  'Alt esq.': 'Left Alt',
  'Alt dir.': 'Right Alt',
  'Cmd esq.': 'Left Cmd',
  'Cmd dir.': 'Right Cmd',
  'Win esq.': 'Left Win',
  'Win dir.': 'Right Win',

  // settings: launcher, extensions, advanced
  'Idioma': 'Language',
  'Abertura animada': 'Animated opening',
  'A animação ao abrir o launcher (um clique ou tecla pula).': 'The animation when the launcher opens (a click or key skips it).',
  'Música de fundo': 'Background music',
  'Toca no launcher; para quando o jogo abre. Use sua própria música e imagens:':
    'Plays in the launcher; stops when the game starts. Use your own music and images:',
  'na pasta': 'in the',
  '(botão abaixo).': 'folder (button below).',
  'Arte do launcher': 'Launcher art',
  '). Ficam só neste computador.': '). They stay on this computer only.',
  'Extensão': 'Extension',
  'Variáveis extras (': 'Extra variables (',
  'TOTK_NOME=valor': 'TOTK_NAME=value',
  ', uma por linha)': ', one per line)',
  'Log do jogo': 'Game log',
  'Salvo': 'Saved',
  'Não foi possível salvar': 'Could not save',
  'Não foi possível abrir': 'Could not open',
  'Não foi possível abrir o jogo': 'Could not start the game',
  'Abrir log': 'Open log',

  // about
  'é um port nativo não oficial de': 'is an unofficial native port of',
  '1.4.3 para PC. Não é um emulador: o código do jogo é traduzido antes (recompilação estática ARM64 → C, compilado para o processador do PC) e o sistema do Switch e a API gráfica NVN são substituídos por implementações próprias sobre Vulkan, no Windows e no macOS.':
    '1.4.3 for PC. It is not an emulator: the game\'s code is translated ahead of time (static recompilation ARM64 → C, built for the PC\'s processor) and the Switch\'s system and NVN graphics API are replaced by its own implementations over Vulkan, on Windows and macOS.',
  'Criador e desenvolvedor principal': 'Creator and lead developer',
  'Todo o port — recompilação, runtime, camada gráfica Vulkan, áudio e o processo de build — é trabalho dele.':
    'The whole port — recompilation, runtime, Vulkan graphics layer, audio and the build process — is his work.',
  'Ver projeto': 'View project',
  'Construído sobre': 'Built on',
  'Camada NVN, serviço de tradução de shaders e inventário de shaders (MIT)': 'NVN layer, shader translation service and shader inventory (MIT)',
  'Tradutor de shaders Maxwell → SPIR-V, versão 1.3.2 (MIT)': 'Maxwell → SPIR-V shader translator, version 1.3.2 (MIT)',
  'LASRecomp e B2recomp': 'LASRecomp and B2recomp',
  'Partes do runtime de recompilação (MIT / GPL-3.0)': 'Parts of the recompilation runtime (MIT / GPL-3.0)',
  'bc7enc e rgbcx, codificação de texturas BC7 / BC1 (MIT)': 'bc7enc and rgbcx, BC7 / BC1 texture encoding (MIT)',
  'Técnicas de BotW usadas como referência no controle de quadros e na renderização': 'BotW techniques used as reference for frame pacing and rendering',
  'Câmera com mouse e teclado remapeável (PR #20)': 'Mouse camera and remappable keyboard (PR #20)',
  'Tabelas de salto do lifter e pacote único de shaders (PRs #24 e #25)': 'Lifter jump tables and the single shader pack (PRs #24 and #25)',
  'Vulkan sobre Metal no macOS e a camada de shader objects': 'Vulkan over Metal on macOS and the shader object layer',
  'Leitura de NSP/XCI na extensão de extração (BSD-3)': 'NSP/XCI reading in the extraction extension (BSD-3)',
  'Janela, controles e áudio · compilador · este launcher': 'Window, controllers and audio · compiler · this launcher',
  'Este fork': 'This fork',
  'Por': 'By',
  'Port para macOS': 'macOS port',
  'em Apple silicon (M1 em diante), sobre MoltenVK': 'on Apple silicon (M1 and later), over MoltenVK',
  'Este launcher': 'This launcher',
  ': instalação guiada, extensões, configurações e abertura animada': ': guided installation, extensions, settings and the animated opening',
  'Extração direta': 'Direct extraction',
  'dos seus NSP/XCI com a extensão nxextract': 'from your NSP/XCI with the nxextract extension',
  'Build incremental': 'Incremental build',
  ': uma mudança recompila só o que ela afeta': ': a change rebuilds only what it affects',
  'Contador de FPS, correção de iluminação para GPUs AMD e ajustes de janela': 'FPS counter, a lighting fix for AMD GPUs and window tweaks',
  'Distribuído sob a': 'Distributed under the',
  'ou posterior; cada componente de terceiros mantém sua licença.': 'or later; each third-party component keeps its license.',
  'Nada do jogo vem com o port: tudo é gerado no seu PC a partir do seu próprio dump e deve ficar aqui. Não afiliado nem endossado pela Nintendo.':
    'Nothing of the game comes with the port: everything is made on your PC from your own dump and must stay here. Not affiliated with or endorsed by Nintendo.',
  'são marcas da Nintendo.': 'are trademarks of Nintendo.',

  // updates (game_update.rs, the launcher's updater)
  'Baixar': 'Download',
  'código do jogo': 'game source',
  'Consultando o GitHub…': 'Asking GitHub…',
  'Atualizar': 'Update',
  'Nova versão do launcher': 'New launcher version',
  'Atualizar launcher': 'Update launcher',
  'Não foi possível atualizar o launcher': 'Could not update the launcher',

  // messages from the launcher's back end (src-tauri)
  'A instalação já está rodando.': 'The installation is already running.',
  'O jogo já está aberto.': 'The game is already running.',
};

const PATTERNS = [
  [/^· versão (\S+)( \(repositório\))?$/, (m, a, b) => `· version ${a}${b ? ' (repository)' : ''}`],
  [/^(.+) · ([\d.]+) GB de ([\d.]+) GB$/, (m, a, b, c) => `${a} · ${b} GB of ${c} GB`],
  [/^(.+) · (\d+) de (\d+) (blocos|objetos|shaders)$/, (m, a, b, c, d) => `${a} · ${b} of ${c} ${{ blocos: 'chunks', objetos: 'objects', shaders: 'shaders' }[d]}`],
  [/^Espaço em (.+): precisa de ([\d.]+) GB, livres ([\d.]+) GB$/, (m, a, b, c) => `Space on ${a}: ${b} GB needed, ${c} GB free`],
  [/^Aviso: (.+)$/s, (m, a) => `Warning: ${a}`],
  [/^Fim: (\w+) \((\d+) s\)$/, (m, a, b) => `End: ${a} (${b} s)`],
  [/^Instalador (.+)$/, (m, a) => `Installer ${a}`],
  [/^Launcher (\S+) · jogo: (.+)$/, (m, a, b) => `Launcher ${a} · game: ${b}`],
  [/^O Python do instalador não foi encontrado em (.+)$/, (m, a) => `The installer's Python was not found in ${a}`],
  [/^· (\d+) GB livres(?: \(precisa de ~(\d+) GB\))?$/, (m, a, b) => `· ${a} GB free${b ? ` (needs ~${b} GB)` : ''}`],
  [/^~(\d+) min restantes$/, (m, a) => `~${a} min left`],
  [/^(\d+) arquivos: (.*)$/, (m, a, b) => `${a} files: ${b}`],
  [/^([\d.,]+) de ([\d.,]+) GB$/, (m, a, b) => `${a} of ${b} GB`],
  [/^Compilando shaders · (.+) \/ (.+)$/, (m, a, b) => `Compiling shaders · ${a} / ${b}`],
  [/^O jogo fechou com erro \(código (-?\d+)\)$/, (m, a) => `The game closed with an error (code ${a})`],
  [/^Imagem (\d+)$/, (m, a) => `Image ${a}`],
  [/^Tecla (\d+)$/, (m, a) => `Key ${a}`],
  [/^([\d.]+) °\/ponto$/, (m, a) => `${a} °/count`],
  [/^(.+) não existe ainda: faça a instalação primeiro\.$/, (m, a) => `${a} does not exist yet: install the game first.`],
  [/^resolução inválida: (.+)$/, (m, a) => `invalid resolution: ${a}`],
  [/^\+ (\d+) mudanças$/, (m, a) => `+ ${a} more changes`],
  [/^commit inválido: (.+)$/, (m, a) => `invalid commit: ${a}`],
];

const ATTRS = ['title', 'placeholder', 'aria-label'];
const original = new WeakMap();   // text node -> its Portuguese text; element -> {attr: Portuguese}
const produced = new Set(Object.values(EN));   // our own output: not translated again
let lang = 'pt';

function translate(text) {
  const key = text.trim().replace(/\s+/g, ' ');
  if (!key || produced.has(key)) return null;
  let out = EN[key];
  if (out === undefined) {
    for (const [re, f] of PATTERNS) {
      const m = key.match(re);
      if (m) { out = f(...m); break; }
    }
  }
  if (out === undefined) return null;
  produced.add(out);
  return text.match(/^\s*/)[0] + out + text.match(/\s*$/)[0];
}

function apply(node) {
  if (node.nodeType === Node.TEXT_NODE) {
    if (lang === 'pt') {
      const pt = original.get(node);
      if (pt !== undefined && node.data !== pt) node.data = pt;
      return;
    }
    const en = translate(node.data);
    if (en !== null) { original.set(node, node.data); node.data = en; }
    return;
  }
  if (node.nodeType !== Node.ELEMENT_NODE || node.closest('script, style, textarea, #log')) return;
  let saved = original.get(node);
  for (const a of ATTRS) {
    if (!node.hasAttribute(a)) continue;
    if (lang === 'pt') { if (saved && saved[a] !== undefined) node.setAttribute(a, saved[a]); continue; }
    const en = translate(node.getAttribute(a));
    if (en !== null) { saved = saved || {}; saved[a] = node.getAttribute(a); original.set(node, saved); node.setAttribute(a, en); }
  }
  for (const c of node.childNodes) apply(c);
}

export function setLang(l) {
  lang = l === 'en' ? 'en' : 'pt';
  document.documentElement.lang = lang === 'en' ? 'en' : 'pt-BR';
  apply(document.body);
}

export function currentLang() { return lang; }

// Texts main.js writes from now on are translated as they appear.
new MutationObserver((list) => {
  if (lang === 'pt') return;
  for (const m of list) {
    if (m.type === 'characterData') apply(m.target);
    else if (m.type === 'attributes') apply(m.target);
    else m.addedNodes.forEach(apply);
  }
}).observe(document.documentElement, { subtree: true, childList: true, characterData: true, attributes: true, attributeFilter: ATTRS });
