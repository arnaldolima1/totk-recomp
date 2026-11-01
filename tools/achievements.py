"""Achievements: the condition catalogue, the default set, the icons, and the compiler the game reads.

The game keeps its progress in GameData flags, saved by hash (Murmur3 of the flag's name) in progress.sav. An
achievement names a condition over those flags; the runtime (runtime/achievements.c) checks every achievement after
each save the game commits and shows a notice for the ones that became true.

Where things live, all in the install folder:
    achievements/achievements.json   the definitions (the editor, achievements_editor.py, writes them; the setup puts
                                     the default set there when the file is missing)
    achievements/icons/<id>.png      each achievement's icon, 128x128 with its frame (the editor makes them; the default
                                     set's are drawn here)
    achievements/compiled.tsv        what totk.exe reads: every condition resolved to flag hashes (compile below)
    achievements/icons/<id>.rgba     the icon at 64x64, straight RGBA, for the game's menu
    profile/achievements.tsv         the unlocked ones and when (the runtime writes it)

Conditions (the "condition" object of an achievement):
    {"kind": "quest", "quest": Q}                 the quest Q is complete: the enum Step_<Q> holds Complete
    {"kind": "flag", "name": N}                   the bool N is true
    {"kind": "count", "family": F, "min": n}      at least n members of the struct F are true (or hold "value": V,
                                                  for a struct of enums such as DungeonState)
    {"kind": "flags", "names": [...], "min": n}   at least n of the bools are true
    {"kind": "number", "name": N, "min": n}       the int, uint or float N is at least n
    {"kind": "pouch", "pouch": P, "item": I, "min": n}   at least n of item I in inventory pouch P
    {"kind": "array_sum", "name": N, "min": n}    the int array N adds up to at least n
    {"kind": "all"}                               every other achievement is unlocked (the platinum one)

compile() resolves names against GameData/GameDataList.Product.<n>.byml.zs of the install's RomFS: a name that is not
a flag of the expected type there stops the compile with its name, so a typo or another game version shows at once.

Commands (the release's Python, which has compression.zstd):
    python achievements.py defaults DEST    write the default set into DEST/achievements when it has none, then compile
    python achievements.py compile DEST     compile DEST/achievements/achievements.json

Standard library only.
"""
import json
import math
import os
import re
import struct
import sys
import zlib
from pathlib import Path

RARITIES = ('common', 'rare', 'epic', 'legendary', 'platinum')
RARITY_LABEL = {'common': 'Comum', 'rare': 'Rara', 'epic': 'Épica', 'legendary': 'Lendária', 'platinum': 'Platina'}
# frame colours: (rim, glow) in RGB
RARITY_COLOURS = {
    'common': ((176, 141, 87), (120, 92, 52)),
    'rare': ((111, 196, 205), (40, 120, 130)),
    'epic': ((176, 128, 232), (92, 52, 150)),
    'legendary': ((236, 196, 92), (150, 104, 24)),
    'platinum': ((236, 240, 248), (120, 210, 200)),
}
FORMAT = 'totk-achievements 1'


# ---- Murmur3 (x86, 32-bit, seed 0): the game's hash of a flag name

def murmur3(data):
    if isinstance(data, str):
        data = data.encode()
    c1, c2, h = 0xcc9e2d51, 0x1b873593, 0
    n = len(data) // 4
    for i in range(n):
        k = int.from_bytes(data[4 * i:4 * i + 4], 'little')
        k = (k * c1) & 0xffffffff
        k = ((k << 15) | (k >> 17)) & 0xffffffff
        h ^= (k * c2) & 0xffffffff
        h = ((h << 13) | (h >> 19)) & 0xffffffff
        h = (h * 5 + 0xe6546b64) & 0xffffffff
    tail, k = data[4 * n:], 0
    for i in reversed(range(len(tail))):
        k = (k << 8) | tail[i]
    if tail:
        k = (k * c1) & 0xffffffff
        k = ((k << 15) | (k >> 17)) & 0xffffffff
        h ^= (k * c2) & 0xffffffff
    h ^= len(data)
    h ^= h >> 16
    h = (h * 0x85ebca6b) & 0xffffffff
    h ^= h >> 13
    h = (h * 0xc2b2ae35) & 0xffffffff
    return h ^ (h >> 16)


# ---- the catalogue: the conditions the editor offers, with their Portuguese labels

# Presets: a condition with a fixed target and an adjustable minimum ("max" bounds it in the editor; "unit" divides
# the stored number for display, as the game counts hearts in quarters).
PRESETS = {
    'shrines': dict(label='Santuários concluídos', cond={'kind': 'count', 'family': 'DungeonState', 'value': 'Clear'},
                    max=152),
    'lightroots': dict(label='Raízes de luz acesas', cond={'kind': 'count', 'family': 'CheckPoint_IsDisplayFoliage'},
                       max=120),
    'towers': dict(label='Torres de observação ativadas', cond={'kind': 'count', 'family': 'IsOpenCannon'}, max=15),
    'bubbul': dict(label='Gemas Bubbul obtidas', cond={'kind': 'count', 'family': 'IsGetCaveMasterMedal'}, max=147),
    'locations': dict(label='Locais descobertos', cond={'kind': 'count', 'family': 'IsVisitLocation'}, max=1400),
    'chests': dict(label='Baús especiais abertos', cond={'kind': 'count', 'family': 'IsTreasureBoxOpen'}, max=222),
    'dragon_tears': dict(label='Lágrimas do Dragão vistas',
                         cond={'kind': 'flags', 'names': [f'Complete_DragonTears{i:02d}' for i in range(1, 13)]},
                         max=12),
    'sages': dict(label='Sábios ao seu lado',
                  cond={'kind': 'flags', 'names': [f'PlayerStatus.Companion.{s}.IsGet'
                                                   for s in ('Wind', 'Fire', 'Water', 'Electric', 'Soul')]}, max=5),
    'hearts': dict(label='Corações (máximo)', cond={'kind': 'number', 'name': 'PlayerStatus.MaxLife'}, max=40, unit=4),
    'stamina': dict(label='Rodas de vigor (máximo)', cond={'kind': 'number', 'name': 'PlayerStatus.MaxStamina'}, max=3,
                    unit=1000),
    'rupees': dict(label='Rúpias na carteira', cond={'kind': 'number', 'name': 'PlayerStatus.CurrentRupee'}, max=999999),
    'korok_seeds': dict(label='Sementes de Korok no bolso',
                        cond={'kind': 'pouch', 'pouch': 'KeyItem', 'item': 'Obj_KorokNuts'}, max=1000),
    'hestu': dict(label='Expansões de bolso com Hestu', cond={'kind': 'array_sum', 'name': 'OldKorok_ExpandPouchNum'},
                  max=60),
    'bargainer': dict(label='Encontros com a Estátua do Barganhista',
                      cond={'kind': 'number', 'name': 'DemonStatue_MeetCount'}, max=20),
    # Multiplayer: counters the game keeps while in a room (runtime/net_mp.c), not save flags. Achievements over them
    # stay outside the platinum and count for 100 %.
    'mp_rooms': dict(label='Multijogador: vezes que entrou numa sala', cond={'kind': 'mp', 'counter': 'rooms'},
                     max=1000),
    'mp_players': dict(label='Multijogador: jogadores juntos na sala (máximo)',
                       cond={'kind': 'mp', 'counter': 'players'}, max=32),
    'mp_chat': dict(label='Multijogador: mensagens enviadas no chat', cond={'kind': 'mp', 'counter': 'chat'},
                    max=10000),
    'mp_voice': dict(label='Multijogador: minutos falando no chat de voz', cond={'kind': 'mp', 'counter': 'voice'},
                     max=6000, unit=60),
    'mp_minutes': dict(label='Multijogador: horas numa sala', cond={'kind': 'mp', 'counter': 'minutes'}, max=1000,
                       unit=60),
    'mp_friend_ach': dict(label='Multijogador: conquistas de amigos vistas na sala',
                          cond={'kind': 'mp', 'counter': 'friend_unlocks'}, max=1000),
    'mp_group_ach': dict(label='Multijogador: conquistas suas com amigos na sala',
                         cond={'kind': 'mp', 'counter': 'group_unlocks'}, max=100),
    'mp_tiers': dict(label='Multijogador: níveis de desafios do grupo concluídos',
                     cond={'kind': 'mp', 'counter': 'tiers'}, max=24),
    'mp_meet': dict(label='Multijogador: encontrou um amigo no mundo (a menos de 30 m)',
                    cond={'kind': 'mp', 'counter': 'meet'}, max=1),
    'mp_marks': dict(label='Multijogador: marcadores colocados no radar', cond={'kind': 'mp', 'counter': 'marks'},
                     max=1000),
}
# Numbers the prologue inflates: under Hyrule Castle Link has 30 hearts and three stamina wheels, and the game saves
# them (MaxLife 120, MaxStamina 3000) before setting 3 hearts and one wheel when he wakes in the Room of Awakening. A
# number condition on these counts only once that flag is true; the compiler adds it whatever the definition says, so
# sets made before this rule follow it too.
COUNT_AFTER = {'PlayerStatus.MaxLife': 'SageOfSky_IsOpenShutterInWakeUpRoom',
               'PlayerStatus.MaxStamina': 'SageOfSky_IsOpenShutterInWakeUpRoom'}

# The armor sets: the base id of each (the first of its five ids; the other four are its upgrade levels), whose head,
# body and legs pieces each have a flag IsGet.Armor_<id>_<Head|Upper|Lower> that turns true when the piece is first
# obtained. Labels are ours.
ARMOR_SETS = {
    'hylian': ('Traje Hyliano', 1), 'wild': ('Traje Selvagem', 5), 'zora': ('Armadura Zora', 6),
    'desert_voe': ('Traje do Deserto', 8), 'snowquill': ('Traje Rito da Neve', 9),
    'flamebreaker': ('Armadura Antichamas', 11), 'stealth': ('Traje Furtivo', 12),
    'climbing': ('Equipamento de Escalada', 14), 'radiant': ('Traje Radiante', 17), 'soldier': ('Armadura de Soldado', 20),
    'rubber': ('Armadura de Borracha', 46), 'barbarian': ('Armadura Bárbara', 48), 'dark': ('Traje Sombrio', 160),
    'phantom': ('Armadura Fantasma', 171), 'tingle': ('Fantasia do Tingle', 174),
    'royal_guard': ('Uniforme da Guarda Real', 179), 'evil_spirit': ('Armadura do Espírito Maligno', 180),
    'time': ('Traje do Tempo', 200), 'wind': ('Traje do Vento', 205), 'twilight': ('Traje do Crepúsculo', 210),
    'sky': ('Traje do Céu', 215), 'fierce_deity': ('Armadura da Divindade Feroz', 225), 'hero': ('Traje do Herói', 230),
    'glide': ('Traje de Planador', 1006), 'froggy': ('Traje de Sapo', 1046), 'miner': ('Traje de Minerador', 1051),
    'ember': ('Traje Brasa', 1061), 'charged': ('Traje Carregado', 1066), 'frostbite': ('Traje Gélido', 1071),
    'mystic': ('Traje Místico', 1086), 'zonaite': ('Armadura de Zonaíta', 1091), 'awakening': ('Traje do Despertar', 1096),
    'depths': ('Traje das Profundezas', 1141),
}
CLASSIC_SETS = ('wild', 'time', 'wind', 'twilight', 'sky', 'hero')   # the outfits of earlier Links


def armor_flags(*sets):
    """The IsGet flags of the head, body and legs of each set."""
    out = []
    for s in sets:
        base = ARMOR_SETS[s][1]
        out += [f'IsGet.Armor_{base:03d}_{part}' for part in ('Head', 'Upper', 'Lower')]
    return out


PRESETS['armor_pieces'] = dict(label='Peças de conjuntos de roupa obtidas',
                               cond={'kind': 'flags', 'names': armor_flags(*ARMOR_SETS)}, max=3 * len(ARMOR_SETS))

# Single flags with a Portuguese label.
FLAGS = {
    'SageOfSky_IsOpenShutterInWakeUpRoom': 'Acordar na Sala do Despertar (depois do ataque de Ganondorf)',
    'Firstskyisland_OnDispLocationName': 'Saltar e pousar na Grande Ilha do Céu',
    'HaveMasterSword': 'Ter a Espada Mestra',
    'IsGameClear': 'Zerar o jogo (derrotar Ganondorf)',
    'GreatFairy4_AllRevival': 'Despertar as quatro Grandes Fadas',
    'HorseInn_RegisterFirstHorse': 'Registrar o primeiro cavalo num estábulo',
    'MapData.IsOpenUnderGround': 'Abrir o mapa das Profundezas',
}
# Quests with a Portuguese name; any other quest of the game can be picked by its internal name.
QUESTS = {
    'MainChallenge': 'Ilha do Grande Céu (prólogo)',
    'SageOfWind': 'Fenômeno regional: Rito (Templo do Vento)',
    'SageOfFire': 'Fenômeno regional: Goron (Templo do Fogo)',
    'SageOfZora': 'Fenômeno regional: Zora (Templo da Água)',
    'SageOfGerudo': 'Fenômeno regional: Gerudo (Templo do Relâmpago)',
    'SageOfSoul': 'O Sábio do Espírito (Templo do Espírito)',
    'GetMasterSword': 'A Espada Mestra',
    'MemoryOfTheDragon': 'As Lágrimas do Dragão',
    'DefeatGanondorf': 'Derrotar Ganondorf',
    'GetCamera': 'A câmera do Purah Pad',
    'NewsPaper_Mitsuba': 'Jornal Trevo da Sorte (Penn)',
    'Hateno_VillageMayorElection': 'A eleição de Hateno',
    'Uotori_RevivePlan': 'Reconstrução de Lurelin',
    'GiantHorse': 'O cavalo gigante',
    'FindWhiteHorse': 'O cavalo branco',
    'EightHeroStatues': 'As estátuas das oito heroínas',
    'MinusYiga': 'Mestre Kohga nas Profundezas',
    'TheStrongestWeapon': 'A arma mais forte',
}

# ---- the default set: (id, name, description, rarity, emblem, condition)


def _q(quest):
    return {'kind': 'quest', 'quest': quest}


def _p(preset, n):
    return dict(PRESETS[preset]['cond'], min=n * PRESETS[preset].get('unit', 1), preset=preset)


def _armor(*sets):
    return {'kind': 'armor_set', 'sets': list(sets)}


DEFAULTS = [
    # the story
    ('toque_rei_demonio', 'O Toque do Rei Demônio', 'Sobreviva ao ataque de Ganondorf sob o Castelo de Hyrule.',
     'common', 'mask', {'kind': 'flag', 'name': 'SageOfSky_IsOpenShutterInWakeUpRoom'}),
    ('salto_de_fe', 'Salto de Fé', 'Salte do templo e pouse na Grande Ilha do Céu.', 'common', 'feather',
     {'kind': 'flag', 'name': 'Firstskyisland_OnDispLocationName'}),
    ('despertar', 'Despertar nos Céus', 'Conclua o prólogo na Ilha do Grande Céu.', 'common', 'triforce',
     _q('MainChallenge')),
    ('primeira_torre', 'Olhos no Horizonte', 'Ative sua primeira torre de observação.', 'common', 'tower',
     _p('towers', 1)),
    ('todas_torres', 'Hyrule Mapeada', 'Ative todas as 15 torres de observação.', 'epic', 'tower', _p('towers', 15)),
    ('vento', 'Asas de Tulin', 'Liberte a região Rito do fenômeno do Templo do Vento.', 'rare', 'feather',
     _q('SageOfWind')),
    ('fogo', 'Rolando com Yunobo', 'Liberte a região Goron do fenômeno do Templo do Fogo.', 'rare', 'flame',
     _q('SageOfFire')),
    ('agua', 'A Chuva de Sidon', 'Liberte o Domínio Zora do fenômeno do Templo da Água.', 'rare', 'wave',
     _q('SageOfZora')),
    ('relampago', 'O Trovão de Riju', 'Liberte o deserto Gerudo do fenômeno do Templo do Relâmpago.', 'rare', 'bolt',
     _q('SageOfGerudo')),
    ('espirito', 'A Voz de Mineru', 'Conclua o Templo do Espírito com Mineru.', 'rare', 'mask', _q('SageOfSoul')),
    ('sabios', 'Juramento dos Sábios', 'Tenha os cinco sábios lutando ao seu lado.', 'epic', 'sages', _p('sages', 5)),
    ('espada_mestra', 'A Lâmina que Sela as Trevas', 'Retire a Espada Mestra das mãos do Dragão de Luz.', 'epic',
     'sword', {'kind': 'flag', 'name': 'HaveMasterSword'}),
    ('primeira_lagrima', 'Uma Lembrança Perdida', 'Veja sua primeira Lágrima do Dragão.', 'common', 'tear',
     _p('dragon_tears', 1)),
    ('todas_lagrimas', 'Memórias de Zelda', 'Veja todas as Lágrimas do Dragão.', 'epic', 'tear',
     _p('dragon_tears', 12)),
    ('lenda', 'O Fim do Cataclismo', 'Derrote Ganondorf e salve Hyrule.', 'legendary', 'triforce',
     {'kind': 'flag', 'name': 'IsGameClear'}),
    # shrines
    ('santuario_1', 'Primeira Bênção', 'Conclua seu primeiro santuário.', 'common', 'shrine', _p('shrines', 1)),
    ('santuario_10', 'Peregrino', 'Conclua 10 santuários.', 'common', 'shrine', _p('shrines', 10)),
    ('santuario_40', 'Devoto da Luz', 'Conclua 40 santuários.', 'rare', 'shrine', _p('shrines', 40)),
    ('santuario_80', 'Sábio dos Testes', 'Conclua 80 santuários.', 'epic', 'shrine', _p('shrines', 80)),
    ('santuario_152', 'A Luz de Todos os Santuários', 'Conclua todos os 152 santuários.', 'legendary', 'shrine',
     _p('shrines', 152)),
    # the depths
    ('profundezas', 'Rumo à Escuridão', 'Abra o mapa das Profundezas.', 'common', 'root',
     {'kind': 'flag', 'name': 'MapData.IsOpenUnderGround'}),
    ('raiz_1', 'Raiz da Esperança', 'Acenda sua primeira raiz de luz.', 'common', 'root', _p('lightroots', 1)),
    ('raiz_30', 'Caminho Iluminado', 'Acenda 30 raízes de luz.', 'rare', 'root', _p('lightroots', 30)),
    ('raiz_60', 'Luz nas Profundezas', 'Acenda 60 raízes de luz.', 'epic', 'root', _p('lightroots', 60)),
    ('raiz_120', 'Profundezas Iluminadas', 'Acenda todas as 120 raízes de luz.', 'legendary', 'root',
     _p('lightroots', 120)),
    ('kohga', 'O Fim do Mestre Kohga', 'Derrote o Mestre Kohga nas Profundezas de uma vez por todas.', 'rare', 'mask',
     _q('MinusYiga')),
    # collectables
    ('bubbul_1', 'Coaxar Brilhante', 'Obtenha sua primeira Gema Bubbul.', 'common', 'gem', _p('bubbul', 1)),
    ('bubbul_25', 'Colecionador de Gemas', 'Obtenha 25 Gemas Bubbul.', 'rare', 'gem', _p('bubbul', 25)),
    ('bubbul_100', 'Amigo de Koltin', 'Obtenha 100 Gemas Bubbul.', 'epic', 'gem', _p('bubbul', 100)),
    ('korok_1', 'Yahaha!', 'Tenha uma semente de Korok no bolso.', 'common', 'leaf', _p('korok_seeds', 1)),
    ('korok_100', 'Bolso de Sementes', 'Tenha 100 sementes de Korok no bolso ao mesmo tempo.', 'rare', 'leaf',
     _p('korok_seeds', 100)),
    ('hestu_1', 'Maracas de Hestu', 'Expanda um bolso com a ajuda de Hestu.', 'common', 'leaf', _p('hestu', 1)),
    ('hestu_15', 'Melhor Amigo de Hestu', 'Expanda os bolsos 15 vezes com Hestu.', 'epic', 'leaf', _p('hestu', 15)),
    ('baus_25', 'Caçador de Tesouros', 'Abra 25 baús especiais.', 'rare', 'chest', _p('chests', 25)),
    ('locais_100', 'Pés na Estrada', 'Descubra 100 locais.', 'common', 'map', _p('locations', 100)),
    ('locais_400', 'Cartógrafo de Hyrule', 'Descubra 400 locais.', 'rare', 'map', _p('locations', 400)),
    ('locais_800', 'Nenhum Canto Esquecido', 'Descubra 800 locais.', 'epic', 'map', _p('locations', 800)),
    # the hero
    ('coracoes_10', 'Coração Valente', 'Alcance 10 corações.', 'rare', 'heart', _p('hearts', 10)),
    ('coracoes_20', 'Coração de Herói', 'Alcance 20 corações.', 'epic', 'heart', _p('hearts', 20)),
    ('vigor_2', 'Fôlego de Campeão', 'Alcance duas rodas de vigor.', 'rare', 'stamina', _p('stamina', 2)),
    ('rupias_1000', 'Bolso Tilintando', 'Junte 1.000 rúpias.', 'common', 'rupee', _p('rupees', 1000)),
    ('rupias_10000', 'Magnata de Hyrule', 'Junte 10.000 rúpias.', 'epic', 'rupee', _p('rupees', 10000)),
    ('barganhista', 'Negócio Sombrio', 'Encontre a Estátua do Barganhista.', 'common', 'mask', _p('bargainer', 1)),
    # Hyrule's people
    ('cavalo', 'Companheiro de Estrada', 'Registre seu primeiro cavalo num estábulo.', 'common', 'horse',
     {'kind': 'flag', 'name': 'HorseInn_RegisterFirstHorse'}),
    ('cavalo_gigante', 'Montaria Colossal', 'Conclua a aventura do cavalo gigante.', 'rare', 'horse',
     _q('GiantHorse')),
    ('cavalo_branco', 'A Crina Branca', 'Conclua a aventura do cavalo branco.', 'rare', 'horse', _q('FindWhiteHorse')),
    ('fadas', 'Orquestra das Fadas', 'Desperte as quatro Grandes Fadas.', 'epic', 'fairy',
     {'kind': 'flag', 'name': 'GreatFairy4_AllRevival'}),
    ('camera', 'Diga Xis!', 'Ganhe a câmera do Purah Pad.', 'common', 'camera', _q('GetCamera')),
    ('jornal', 'Furo de Reportagem', 'Conclua as reportagens do Jornal Trevo da Sorte.', 'epic', 'feather',
     _q('NewsPaper_Mitsuba')),
    ('hateno', 'Voto em Hateno', 'Resolva a eleição de Hateno.', 'rare', 'flag', _q('Hateno_VillageMayorElection')),
    ('lurelin', 'Lurelin Renasce', 'Reconstrua a vila de Lurelin.', 'rare', 'wave', _q('Uotori_RevivePlan')),
    ('heroinas', 'Heroínas Esquecidas', 'Conclua a aventura das estátuas das oito heroínas.', 'rare', 'sword',
     _q('EightHeroStatues')),
    ('arma_forte', 'A Arma Mais Forte', 'Conclua a aventura da arma mais forte.', 'rare', 'sword',
     _q('TheStrongestWeapon')),
    # clothes
    ('roupa_hyliana', 'Moda de Hateno', 'Tenha o Traje Hyliano completo: capuz, túnica e calças.', 'common', 'armor',
     _armor('hylian')),
    ('roupa_despertar', 'Vestido para Acordar', 'Tenha o Traje do Despertar completo.', 'rare', 'armor',
     _armor('awakening')),
    ('roupa_profundezas', 'Explorador das Trevas', 'Tenha o Traje das Profundezas completo.', 'rare', 'armor',
     _armor('depths')),
    ('roupa_zonaita', 'Tecnologia Ancestral', 'Tenha a Armadura de Zonaíta completa.', 'rare', 'armor',
     _armor('zonaite')),
    ('roupa_barbara', 'Fúria Bárbara', 'Tenha a Armadura Bárbara completa.', 'epic', 'armor', _armor('barbarian')),
    ('roupa_tempo', 'O Herói do Tempo', 'Tenha o Traje do Tempo completo.', 'epic', 'armor', _armor('time')),
    ('roupa_divindade', 'Máscara da Divindade', 'Tenha a Armadura da Divindade Feroz completa.', 'epic', 'armor',
     _armor('fierce_deity')),
    ('roupas_classicas', 'Lendas que Vestem', 'Tenha os seis trajes dos heróis lendários: Selvagem, do Tempo, do '
     'Vento, do Crepúsculo, do Céu e do Herói.', 'legendary', 'armor', _armor(*CLASSIC_SETS)),
    ('roupas_30', 'Guarda-Roupa Cheio', 'Obtenha 30 peças de conjuntos de roupa.', 'rare', 'armor',
     _p('armor_pieces', 30)),
    ('roupas_todas', 'Estilista de Hyrule', f'Tenha todos os {len(ARMOR_SETS)} conjuntos de roupa completos.',
     'legendary', 'armor', _p('armor_pieces', 3 * len(ARMOR_SETS))),
    # the platinum one
    # multiplayer: outside the platinum, inside 100 %
    ('mp_primeiro_contato', 'Não Estamos Sós', 'Entre numa sala multijogador.', 'common', 'group', _p('mp_rooms', 1)),
    ('mp_trio', 'A Força do Trio', 'Esteja numa sala com mais dois jogadores.', 'rare', 'group',
     _p('mp_players', 3)),
    ('mp_festa', 'Festa em Hyrule', 'Esteja numa sala com mais quatro jogadores.', 'epic', 'group',
     _p('mp_players', 5)),
    ('mp_tagarela', 'Tagarela de Hateno', 'Envie 50 mensagens no chat da sala.', 'rare', 'chat', _p('mp_chat', 50)),
    ('mp_voz', 'Voz dos Sábios', 'Fale por 10 minutos no chat de voz.', 'rare', 'voice', _p('mp_voice', 10)),
    ('mp_companhia', 'Companheiros de Jornada', 'Passe 5 horas numa sala com amigos.', 'epic', 'group',
     _p('mp_minutes', 5)),
    ('mp_torcida', 'Torcida Organizada', 'Veja 25 conquistas de amigos na sala.', 'rare', 'flag',
     _p('mp_friend_ach', 25)),
    ('mp_comemoracao', 'Vitória Compartilhada', 'Desbloqueie uma conquista com amigos na sala.', 'common', 'fairy',
     _p('mp_group_ach', 1)),
    ('mp_esforco', 'Esforço Conjunto', 'Conclua um nível de desafio do grupo.', 'common', 'shrine',
     _p('mp_tiers', 1)),
    ('mp_alianca', 'Aliança de Hyrule', 'Conclua 12 níveis de desafios do grupo.', 'epic', 'shrine',
     _p('mp_tiers', 12)),
    ('mp_encontro', 'Encontro em Hyrule', 'Encontre um amigo no mundo: fiquem a menos de 30 metros um do outro.',
     'rare', 'map', _p('mp_meet', 1)),
    ('mp_guia', 'Guia de Hyrule', 'Coloque 20 marcadores no radar dos amigos, cada um a 50 m do anterior.', 'rare', 'flag', _p('mp_marks', 20)),
    ('mp_lendas', 'Lendas Unidas', 'Conclua os 24 níveis de desafios do grupo.', 'legendary', 'triforce',
     _p('mp_tiers', 24)),
    ('platina', 'Lenda de Hyrule', 'Desbloqueie todas as outras conquistas (as de multijogador ficam de fora).',
     'platinum', 'triforce', {'kind': 'all'}),
]


def default_definitions():
    return {'format': FORMAT, 'offered': sorted(i for i, *_ in DEFAULTS), 'achievements': [
        {'id': i, 'name': n, 'description': d, 'rarity': r, 'emblem': e, 'condition': c, 'hidden': False}
        for i, n, d, r, e, c in DEFAULTS]}


# ---- images: a small RGBA canvas, PNG in and out

class Canvas:
    """RGBA float canvas (0..1), straight alpha."""

    def __init__(self, w, h, fill=(0, 0, 0, 0)):
        self.w, self.h = w, h
        self.px = [list(fill) for _ in range(w * h)]

    def blend(self, x, y, rgb, a):
        if a <= 0 or not (0 <= x < self.w and 0 <= y < self.h):
            return
        p = self.px[y * self.w + x]
        out_a = a + p[3] * (1 - a)
        if out_a <= 0:
            return
        for i in range(3):
            p[i] = (rgb[i] * a + p[i] * p[3] * (1 - a)) / out_a
        p[3] = out_a

    def shape(self, inside, rgb, alpha=1.0, samples=3, bbox=None):
        """Fill the region where inside(x, y) holds (pixel coordinates, y down), antialiased."""
        x0, y0, x1, y1 = bbox or (0, 0, self.w, self.h)
        offs = [(i + 0.5) / samples for i in range(samples)]
        full = samples * samples
        for y in range(max(0, int(y0)), min(self.h, int(math.ceil(y1)))):
            for x in range(max(0, int(x0)), min(self.w, int(math.ceil(x1)))):
                hits = sum(1 for oy in offs for ox in offs if inside(x + ox, y + oy))
                if hits:
                    self.blend(x, y, rgb, alpha * hits / full)

    def to_rgba8(self):
        out = bytearray(self.w * self.h * 4)
        for i, p in enumerate(self.px):
            out[4 * i:4 * i + 4] = bytes(max(0, min(255, round(c * 255))) for c in p)
        return bytes(out)

    @classmethod
    def from_rgba8(cls, w, h, data):
        c = cls(w, h)
        c.px = [[data[4 * i + k] / 255 for k in range(4)] for i in range(w * h)]
        return c

    def resized(self, w, h):
        """Area-averaged downscale (or bilinear upscale) to w x h, with premultiplied alpha."""
        out = Canvas(w, h)
        sx, sy = self.w / w, self.h / h
        for y in range(h):
            for x in range(w):
                if sx >= 1 and sy >= 1:
                    xa, xb = int(x * sx), max(int(x * sx) + 1, int((x + 1) * sx))
                    ya, yb = int(y * sy), max(int(y * sy) + 1, int((y + 1) * sy))
                    acc, n = [0.0] * 4, 0
                    for yy in range(ya, min(yb, self.h)):
                        for xx in range(xa, min(xb, self.w)):
                            p = self.px[yy * self.w + xx]
                            acc[0] += p[0] * p[3]; acc[1] += p[1] * p[3]; acc[2] += p[2] * p[3]; acc[3] += p[3]
                            n += 1
                else:
                    fx, fy = min(self.w - 1.0, max(0.0, (x + 0.5) * sx - 0.5)), min(self.h - 1.0, max(0.0, (y + 0.5) * sy - 0.5))
                    ix, iy = int(fx), int(fy)
                    tx, ty = fx - ix, fy - iy
                    acc, n = [0.0] * 4, 1
                    for dx, dy, wgt in ((0, 0, (1 - tx) * (1 - ty)), (1, 0, tx * (1 - ty)), (0, 1, (1 - tx) * ty),
                                        (1, 1, tx * ty)):
                        p = self.px[min(self.h - 1, iy + dy) * self.w + min(self.w - 1, ix + dx)]
                        acc[0] += p[0] * p[3] * wgt; acc[1] += p[1] * p[3] * wgt; acc[2] += p[2] * p[3] * wgt
                        acc[3] += p[3] * wgt
                a = acc[3] / n if n else 0
                out.px[y * w + x] = [acc[0] / acc[3] if acc[3] else 0, acc[1] / acc[3] if acc[3] else 0,
                                     acc[2] / acc[3] if acc[3] else 0, a]
        return out


def png_write(path, w, h, rgba):
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(tag, body):
        return struct.pack('>I', len(body)) + tag + body + struct.pack('>I', zlib.crc32(tag + body) & 0xffffffff)
    data = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(data)


def png_read(path):
    """(w, h, RGBA bytes) of an 8-bit, non-interlaced PNG (grey, grey+alpha, RGB, RGBA or palette)."""
    data = Path(path).read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', f'{path}: not a PNG'
    pos, idat, palette, trns = 8, b'', None, None
    while pos < len(data):
        n, tag = struct.unpack_from('>I4s', data, pos)
        body = data[pos + 8:pos + 8 + n]
        if tag == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif tag == b'PLTE':
            palette = body
        elif tag == b'tRNS':
            trns = body
        elif tag == b'IDAT':
            idat += body
        pos += 12 + n
    assert depth == 8 and not interlace, f'{path}: only 8-bit non-interlaced PNG'
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * channels
    rows, prev, pos = [], bytearray(stride), 0
    for _ in range(h):
        f, line = raw[pos], bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if f == 1: line[i] = (line[i] + a) & 0xff
            elif f == 2: line[i] = (line[i] + b) & 0xff
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 0xff
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xff
        rows.append(line)
        prev = line
    out = bytearray(w * h * 4)
    for y, line in enumerate(rows):
        for x in range(w):
            o = 4 * (y * w + x)
            if ctype == 6: out[o:o + 4] = line[4 * x:4 * x + 4]
            elif ctype == 2: out[o:o + 4] = bytes(line[3 * x:3 * x + 3]) + b'\xff'
            elif ctype == 0: out[o:o + 4] = bytes([line[x]] * 3) + b'\xff'
            elif ctype == 4: out[o:o + 4] = bytes([line[2 * x]] * 3) + bytes([line[2 * x + 1]])
            else:
                i = line[x]
                out[o:o + 3] = palette[3 * i:3 * i + 3]
                out[o + 3] = trns[i] if trns and i < len(trns) else 255
    return w, h, bytes(out)


# ---- the icons: a medallion in the rarity's colours with an emblem, or a picture in the same frame

def _in_poly(x, y, pts):
    inside = False
    for i in range(len(pts)):
        (x1, y1), (x2, y2) = pts[i - 1], pts[i]
        if (y1 > y) != (y2 > y) and x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
            inside = not inside
    return inside


def _emblem(name):
    """inside(u, v) of an emblem in a unit box (-1..1, y down)."""
    P = lambda pts: (lambda u, v: _in_poly(u, v, pts))
    C = lambda cx, cy, r: (lambda u, v: (u - cx) ** 2 + (v - cy) ** 2 <= r * r)
    ring = lambda cx, cy, r0, r1: (lambda u, v: r0 * r0 <= (u - cx) ** 2 + (v - cy) ** 2 <= r1 * r1)
    union = lambda *fs: (lambda u, v: any(f(u, v) for f in fs))
    minus = lambda a, b: (lambda u, v: a(u, v) and not b(u, v))
    if name == 'triforce':   # three triangles of side a, apexes at the top and at the two bottom-row corners
        a, h = 0.88, 0.88 * 0.866
        tri = lambda ax, ay: P([(ax, ay), (ax + a / 2, ay + h), (ax - a / 2, ay + h)])
        top = -h
        return union(tri(0, top), tri(-a / 2, top + h), tri(a / 2, top + h))
    if name == 'shrine':   # the Sheikah-like eye in a teardrop arch
        arch = P([(0, -0.8), (0.62, -0.1), (0.5, 0.75), (-0.5, 0.75), (-0.62, -0.1)])
        eye = lambda u, v: (u / 0.44) ** 2 + ((v - 0.12) / 0.24) ** 2 <= 1
        return union(minus(arch, eye), C(0, 0.12, 0.11))
    if name == 'tower':
        return union(P([(-0.16, -0.55), (0.16, -0.55), (0.24, 0.8), (-0.24, 0.8)]),
                     P([(-0.42, -0.62), (0.42, -0.62), (0.3, -0.42), (-0.3, -0.42)]), C(0, -0.78, 0.12))
    if name == 'root':
        trunk = P([(-0.1, -0.2), (0.1, -0.2), (0.16, 0.5), (0.5, 0.78), (0.32, 0.8), (0, 0.6), (-0.32, 0.8),
                   (-0.5, 0.78), (-0.16, 0.5)])
        return union(trunk, C(0, -0.42, 0.34))
    if name == 'gem':
        return P([(0, -0.78), (0.6, -0.2), (0, 0.8), (-0.6, -0.2)])
    if name == 'rupee':
        return P([(0, -0.82), (0.42, -0.42), (0.42, 0.42), (0, 0.82), (-0.42, 0.42), (-0.42, -0.42)])
    if name == 'leaf':
        blade = lambda u, v: (u / 0.42) ** 2 + ((v + 0.1) / 0.72) ** 2 <= 1
        return union(blade, P([(-0.04, 0.5), (0.04, 0.5), (0.06, 0.85), (-0.06, 0.85)]))
    if name == 'heart':
        return union(C(-0.32, -0.22, 0.38), C(0.32, -0.22, 0.38), P([(-0.68, -0.08), (0.68, -0.08), (0, 0.78)]))
    if name == 'stamina':
        return minus(ring(0, 0, 0.42, 0.72), P([(0, 0), (0.9, -0.9), (0.9, 0.0)]))
    if name == 'sword':
        return union(P([(-0.09, -0.85), (0.09, -0.85), (0.09, 0.35), (0, 0.48), (-0.09, 0.35)]),
                     P([(-0.45, 0.35), (0.45, 0.35), (0.45, 0.48), (-0.45, 0.48)]),
                     P([(-0.07, 0.48), (0.07, 0.48), (0.07, 0.82), (-0.07, 0.82)]))
    if name == 'map':
        return union(ring(0, 0, 0.62, 0.76), P([(0, -0.6), (0.14, 0), (0, 0.6), (-0.14, 0)]))
    if name == 'chest':
        return minus(union(P([(-0.7, -0.1), (0.7, -0.1), (0.7, 0.62), (-0.7, 0.62)]),
                           lambda u, v: (u / 0.7) ** 2 + ((v + 0.1) / 0.42) ** 2 <= 1 and v <= -0.1),
                     P([(-0.12, -0.04), (0.12, -0.04), (0.12, 0.2), (-0.12, 0.2)]))
    if name == 'tear':
        return union(C(0, 0.22, 0.46), P([(0, -0.8), (0.44, 0.1), (-0.44, 0.1)]))
    if name == 'sages':
        return union(*[C(0.55 * math.sin(k * 2 * math.pi / 5), -0.55 * math.cos(k * 2 * math.pi / 5), 0.17)
                       for k in range(5)], ring(0, 0, 0.2, 0.3))
    if name == 'feather':
        vane = lambda u, v: ((u + 0.3 * v) / 0.3) ** 2 + (v / 0.82) ** 2 <= 1
        return minus(vane, P([(-0.02, -0.9), (0.02, -0.9), (0.3, 0.9), (0.26, 0.9)]))
    if name == 'flame':   # three tongues over a round base, with a dark core
        outer = P([(0.0, -0.88), (0.2, -0.38), (0.42, -0.62), (0.56, 0.05), (0.46, 0.55), (0.0, 0.82),
                   (-0.46, 0.55), (-0.56, 0.05), (-0.4, -0.5), (-0.2, -0.22)])
        return minus(outer, P([(0.0, -0.05), (0.22, 0.35), (0.12, 0.6), (-0.12, 0.6), (-0.22, 0.35)]))
    if name == 'wave':
        return lambda u, v: abs(v - 0.22 * math.sin(u * 4.2)) < 0.14 or abs(v - 0.42 - 0.22 * math.sin(u * 4.2)) < 0.1
    if name == 'bolt':
        return P([(0.15, -0.85), (-0.4, 0.08), (-0.02, 0.08), (-0.18, 0.85), (0.42, -0.12), (0.04, -0.12)])
    if name == 'mask':
        face = lambda u, v: (u / 0.62) ** 2 + (v / 0.78) ** 2 <= 1
        return minus(face, union(C(-0.24, -0.12, 0.12), C(0.24, -0.12, 0.12),
                                 P([(-0.22, 0.32), (0.22, 0.32), (0.16, 0.42), (-0.16, 0.42)])))
    if name == 'horse':   # a horseshoe
        return minus(ring(0, -0.05, 0.36, 0.66), P([(-0.3, 0.2), (0.3, 0.2), (0.6, 0.9), (-0.6, 0.9)]))
    if name == 'fairy':
        wing = lambda cx, s: (lambda u, v: ((u - cx) / 0.32) ** 2 + ((v + 0.12 * s) / 0.48) ** 2 <= 1)
        return union(wing(-0.36, 1), wing(0.36, 1), C(0, 0.1, 0.16), C(0, -0.22, 0.12))
    if name == 'camera':
        return minus(union(P([(-0.72, -0.36), (0.72, -0.36), (0.72, 0.52), (-0.72, 0.52)]),
                           P([(-0.24, -0.52), (0.24, -0.52), (0.3, -0.36), (-0.3, -0.36)])), ring(0, 0.08, 0.0, 0.26))
    if name == 'armor':   # a tunic: the body, the sleeves and a belt
        body = P([(-0.36, -0.62), (-0.14, -0.72), (0.0, -0.56), (0.14, -0.72), (0.36, -0.62), (0.78, -0.22),
                  (0.6, 0.0), (0.4, -0.18), (0.42, 0.8), (-0.42, 0.8), (-0.4, -0.18), (-0.6, 0.0), (-0.78, -0.22)])
        return minus(body, P([(-0.42, 0.18), (0.42, 0.18), (0.42, 0.28), (-0.42, 0.28)]))
    if name == 'flag':
        return union(P([(-0.5, -0.8), (-0.38, -0.8), (-0.38, 0.85), (-0.5, 0.85)]),
                     P([(-0.38, -0.8), (0.6, -0.55), (-0.38, -0.25)]))
    if name == 'group':   # three people: a head over rounded shoulders, the middle one in front
        person = lambda cx, cy, k: union(C(cx, cy - 0.3 * k, 0.2 * k),
                                         minus(C(cx, cy + 0.32 * k, 0.4 * k), P([(cx - 1, cy + 0.36 * k),
                                                                                 (cx + 1, cy + 0.36 * k),
                                                                                 (cx + 1, cy + 2), (cx - 1, cy + 2)])))
        side = union(person(-0.48, -0.05, 0.85), person(0.48, -0.05, 0.85))
        return union(minus(side, C(0, 0.12, 0.5)), person(0, 0.12, 1.0))
    if name == 'chat':   # a speech bubble with three dots cut out
        bubble = union(P([(-0.78, -0.6), (0.78, -0.6), (0.78, 0.3), (-0.2, 0.3), (-0.5, 0.72), (-0.42, 0.3),
                          (-0.78, 0.3)]))
        return minus(bubble, union(C(-0.36, -0.15, 0.11), C(0, -0.15, 0.11), C(0.36, -0.15, 0.11)))
    if name == 'voice':   # a microphone on its stand
        head = union(C(0, -0.42, 0.26), P([(-0.26, -0.42), (0.26, -0.42), (0.26, 0.05), (-0.26, 0.05)]),
                     C(0, 0.05, 0.26))
        cup = minus(ring(0, 0.0, 0.4, 0.5), P([(-1, -1), (1, -1), (1, 0.0), (-1, 0.0)]))
        return union(head, cup, P([(-0.05, 0.48), (0.05, 0.48), (0.05, 0.75), (-0.05, 0.75)]),
                     P([(-0.32, 0.72), (0.32, 0.72), (0.32, 0.82), (-0.32, 0.82)]))
    return C(0, 0, 0.5)


EMBLEMS = ('triforce', 'shrine', 'tower', 'root', 'gem', 'rupee', 'leaf', 'heart', 'stamina', 'sword', 'map', 'chest',
           'tear', 'sages', 'feather', 'flame', 'wave', 'bolt', 'mask', 'horse', 'fairy', 'camera', 'flag', 'armor',
           'group', 'chat', 'voice')
ICON = 128


def _frame(c, rarity, centre_fill):
    """The medallion: a dark Zonai-teal disc with a rim in the rarity's colour and a soft glow; centre_fill(c) paints
    what goes inside the disc (an emblem or a picture) before the rim."""
    rim, glow = RARITY_COLOURS[rarity]
    s = c.w
    cx = cy = s / 2
    r_out, r_in = s * 0.47, s * 0.40
    rim_rgb = tuple(v / 255 for v in rim)
    glow_rgb = tuple(v / 255 for v in glow)

    def band(d, lo, hi):   # coverage of the ring lo..hi at distance d, one pixel of antialiasing
        return max(0.0, min(1.0, d - lo + 0.5, hi - d + 0.5))
    dist = [math.hypot(x + 0.5 - cx, y + 0.5 - cy) for y in range(s) for x in range(s)]
    for i, d in enumerate(dist):   # the background disc: a dark teal radial gradient warmed by the rarity's glow
        a = band(d, -1, r_out)
        if a > 0:
            k = 1 - min(1.0, d / r_out) * 0.6
            rgb = (0.02 + 0.10 * k * glow_rgb[0], 0.07 + 0.12 * k * (0.5 + glow_rgb[1] / 2),
                   0.07 + 0.11 * k * (0.5 + glow_rgb[2] / 2))
            c.blend(i % s, i // s, rgb, a)
    centre_fill(c)
    for i, d in enumerate(dist):
        a = band(d, r_in, r_out)
        if a > 0:   # the rim, lighter on its inner edge
            t = (d - r_in) / (r_out - r_in)
            c.blend(i % s, i // s, tuple(min(1.0, v * (1.1 - 0.35 * t)) for v in rim_rgb), a)
        a = band(d, r_in - s * 0.035, r_in - s * 0.02)
        if a > 0:
            c.blend(i % s, i // s, rim_rgb, 0.45 * a)
        if rarity == 'platinum':   # a thin gold line on the rim
            a = band(d, r_out - s * 0.03, r_out - s * 0.018)
            if a > 0:
                c.blend(i % s, i // s, (0.93, 0.78, 0.36), a)
    # four small diamonds on the rim, as on Zonai devices
    for k in range(4):
        a = k * math.pi / 2 + math.pi / 4
        dx, dy = cx + math.cos(a) * (r_in + r_out) / 2, cy + math.sin(a) * (r_in + r_out) / 2
        d = s * 0.035
        c.shape(lambda x, y, dx=dx, dy=dy, d=d: abs(x - dx) + abs(y - dy) <= d, (0.06, 0.1, 0.1), 1.0,
                bbox=(dx - d - 1, dy - d - 1, dx + d + 1, dy + d + 1))


def emblem_icon(emblem, rarity, size=ICON):
    c = Canvas(size, size)
    rim = tuple(v / 255 for v in RARITY_COLOURS[rarity][0])

    def fill(c):
        inside = _emblem(emblem)
        r = size * 0.27
        cx = cy = size / 2
        c.shape(lambda x, y: inside((x - cx) / r, (y - cy) / r), tuple(0.25 + 0.75 * v for v in rim), 1.0,
                bbox=(cx - r - 2, cy - r - 2, cx + r + 2, cy + r + 2))
    _frame(c, rarity, fill)
    return c


def picture_icon(picture, rarity, zoom=1.0, dx=0.0, dy=0.0, size=ICON):
    """A picture (Canvas) inside the medallion: scaled to cover the inner disc times zoom, moved by dx, dy (fractions
    of the disc)."""
    c = Canvas(size, size)
    inner = size * 0.40
    side = 2 * inner * zoom
    scale = max(side / picture.w, side / picture.h)
    pw, ph = max(1, round(picture.w * scale)), max(1, round(picture.h * scale))
    pic = picture.resized(pw, ph)

    def fill(c):
        ox = size / 2 - pw / 2 + dx * inner
        oy = size / 2 - ph / 2 + dy * inner
        for y in range(size):
            for x in range(size):
                if math.hypot(x + 0.5 - size / 2, y + 0.5 - size / 2) > inner:
                    continue
                px, py = int(x - ox), int(y - oy)
                if 0 <= px < pw and 0 <= py < ph:
                    p = pic.px[py * pw + px]
                    c.blend(x, y, p[:3], p[3])
    _frame(c, rarity, fill)
    return c


# ---- GameDataList: the flags of the game version, by hash

def read_byml(data):
    """A BYML document (version 7, either byte order) as Python values; 0x20 hash maps as dicts."""
    e = '<' if data[:2] == b'YB' else '>'
    bo = 'little' if e == '<' else 'big'
    assert data[:2] in (b'YB', b'BY'), 'not a BYML'
    keys_at, strings_at, root = struct.unpack_from(e + 'III', data, 4)

    def strtab(off):
        n = int.from_bytes(data[off + 1:off + 4], bo)
        offs = struct.unpack_from(f'{e}{n + 1}I', data, off + 4)
        return [data[off + offs[i]:data.index(0, off + offs[i])].decode('utf-8') for i in range(n)]
    keys = strtab(keys_at) if keys_at else []
    strings = strtab(strings_at) if strings_at else []

    def value(t, raw):
        if t == 0xA0: return strings[raw]
        if t == 0xD0: return bool(raw)
        if t == 0xD1: return struct.unpack(e + 'i', struct.pack(e + 'I', raw))[0]
        if t == 0xD2: return struct.unpack(e + 'f', struct.pack(e + 'I', raw))[0]
        if t == 0xD3: return raw
        if t == 0xD4: return struct.unpack_from(e + 'q', data, raw)[0]
        if t == 0xD5: return struct.unpack_from(e + 'Q', data, raw)[0]
        if t == 0xD6: return struct.unpack_from(e + 'd', data, raw)[0]
        if t == 0xFF: return None
        return node(raw)

    def node(off):
        t, n = data[off], int.from_bytes(data[off + 1:off + 4], bo)
        if t == 0xC0:
            types, vals = data[off + 4:off + 4 + n], off + 4 + ((n + 3) & ~3)
            return [value(types[i], struct.unpack_from(e + 'I', data, vals + 4 * i)[0]) for i in range(n)]
        if t == 0xC1:
            out = {}
            for i in range(n):
                p = off + 4 + 8 * i
                out[keys[int.from_bytes(data[p:p + 3], bo)]] = value(data[p + 3], struct.unpack_from(e + 'I', data, p + 4)[0])
            return out
        if t == 0x20:
            types = data[off + 4 + 8 * n:off + 4 + 9 * n]
            return {struct.unpack_from(e + 'I', data, off + 4 + 8 * i)[0]:
                    value(types[i], struct.unpack_from(e + 'I', data, off + 8 + 8 * i)[0]) for i in range(n)}
        if t == 0x21:
            types = data[off + 4 + 12 * n:off + 4 + 13 * n]
            return {struct.unpack_from(e + 'Q', data, off + 4 + 12 * i)[0]:
                    value(types[i], struct.unpack_from(e + 'I', data, off + 12 + 12 * i)[0]) for i in range(n)}
        if t == 0xA1:
            return bytes(data[off + 8:off + 8 + struct.unpack_from(e + 'I', data, off + 4)[0]])
        raise ValueError(f'BYML node type {t:#x}')
    return node(root)


def _sarc(data):
    header, _, start = struct.unpack_from('<HxxII', data, 4)
    count = struct.unpack_from('<H', data, header + 6)[0]
    names = header + 12 + count * 16 + 8
    out = {}
    for i in range(count):
        node = header + 12 + i * 16
        _, attr, lo, hi = struct.unpack_from('<4I', data, node)
        o = names + (attr & 0xffffff) * 4
        out[data[o:data.index(0, o)].decode()] = (start + lo, start + hi)
    return out


def load_game_data(romfs):
    """{hash: (type, entry)} of every GameData flag of the RomFS directory romfs (romfs.bin and index.json)."""
    import compression.zstd as zstd
    romfs = Path(romfs)
    files = {f['path']: f for f in json.loads((romfs / 'index.json').read_text())['files']}
    path = max((p for p in files if re.fullmatch(r'GameData/GameDataList\.Product\.\d+\.byml\.zs', p)),
               key=lambda p: int(p.split('.')[2]))
    with (romfs / 'romfs.bin').open('rb') as f:
        def read(p):
            f.seek(files[p]['offset'])
            return f.read(files[p]['size'])
        pack = zstd.decompress(read('Pack/ZsDic.pack.zs'))
        dicts = {}
        for lo, hi in _sarc(pack).values():
            d = zstd.ZstdDict(pack[lo:hi])
            dicts[d.dict_id] = d
        raw = read(path)
    doc = read_byml(zstd.decompress(raw, zstd_dict=dicts.get(zstd.get_frame_info(raw).dictionary_id)))
    return {e['Hash']: (t, e) for t, es in doc['Data'].items() if t != 'Bool64bitKey' for e in es}


def quest_names(gd):
    """The internal names of the quests a picker can offer: the known ones, then the Step_<name> enums found by hash
    among a list of candidates (QUESTS)."""
    return [q for q in QUESTS if murmur3('Step_' + q) in gd]


# ---- compile: definitions -> compiled.tsv and the 64x64 icons

class CompileError(Exception):
    pass


def _flag(gd, name, types):
    h = murmur3(name)
    if h not in gd or gd[h][0] not in types:
        raise CompileError(f'{name}: not a GameData flag of type {"/".join(types)} in this game version')
    return h, gd[h]


def _enum_value(entry, raw, name):
    if raw not in entry['RawValues']:
        raise CompileError(f'{name}: no value {raw} (it has {", ".join(entry["RawValues"])})')
    return entry['Values'][entry['RawValues'].index(raw)]


def resolve(cond, gd, lists):
    """The compiled condition: (kind, a, b, c, d) for the runtime, with the hash lists in lists."""
    k = cond['kind']
    if k == 'quest':
        name = 'Step_' + cond['quest']
        h, (_, e) = _flag(gd, name, ('Enum',))
        return ('enum', f'{h:08x}', f'{_enum_value(e, "Complete", name):08x}', '', '')
    if k == 'flag':
        h, _ = _flag(gd, cond['name'], ('Bool',))
        return ('bool', f'{h:08x}', '', '', '')
    if k == 'number':   # c: what one shown unit is in the flag (hearts are four quarters, a wheel 1000 stamina);
        # d: a bool that must be true for the number to count (COUNT_AFTER), empty when none
        h, _ = _flag(gd, cond['name'], ('Int', 'UInt', 'Float'))
        unit = PRESETS.get(cond.get('preset'), {}).get('unit', 1)
        after = COUNT_AFTER.get(cond['name'])
        gate = f'{_flag(gd, after, ("Bool",))[0]:08x}' if after else ''
        return ('num', f'{h:08x}', str(int(cond['min'])), str(unit), gate)
    if k == 'array_sum':
        h, _ = _flag(gd, cond['name'], ('IntArray', 'UIntArray'))
        return ('arrsum', f'{h:08x}', str(int(cond['min'])), '', '')
    if k == 'pouch':
        names, _ = _flag(gd, f'Pouch.{cond["pouch"]}.Content.Name', ('String64Array',))
        stock, _ = _flag(gd, f'Pouch.{cond["pouch"]}.Content.StockNum', ('IntArray',))
        return ('pouch', f'{names:08x}', f'{stock:08x}', str(int(cond['min'])), cond['item'])
    if k == 'armor_set':   # every piece of the sets: the same as a list of their flags, all needed
        names = armor_flags(*cond['sets'])
        return resolve({'kind': 'flags', 'names': names, 'min': len(names)}, gd, lists)
    if k in ('count', 'flags'):
        if k == 'count':
            fam, (_, e) = _flag(gd, cond['family'], ('Struct',))
            members = [m['Value'] for m in e['DefaultValue']]
        else:
            members = [_flag(gd, n, ('Bool',))[0] for n in cond['names']]
        value = 0
        want = 'Enum' if cond.get('value') else 'Bool'
        for m in members:
            if m not in gd or gd[m][0] != want:
                raise CompileError(f'{cond.get("family", "flags")}: member {m:08x} is not a {want}')
            if cond.get('value'):
                v = _enum_value(gd[m][1], cond['value'], cond['family'])
                if value and v != value:
                    raise CompileError(f'{cond["family"]}: {cond["value"]} differs between members')
                value = v
        key = ','.join(f'{m:08x}' for m in members)
        lid = lists.setdefault(key, f'L{len(lists)}')
        return ('count', lid, str(int(cond['min'])), f'{value:08x}', '')
    if k == 'all':
        return ('all', '', '', '', '')
    if k == 'mp':   # b: the counter's minimum; c: what one shown unit is in it (minutes in seconds, say)
        unit = PRESETS.get(cond.get('preset'), {}).get('unit', 1)
        return ('mp', cond['counter'], str(int(cond['min'])), str(unit), '')
    raise CompileError(f'unknown condition kind {k}')


# The progress figures a multiplayer room shows: the same conditions as the presets, compiled as S lines.
STATS = {
    'shrines': PRESETS['shrines']['cond'], 'lightroots': PRESETS['lightroots']['cond'],
    'towers': PRESETS['towers']['cond'], 'koroks': PRESETS['korok_seeds']['cond'],
    'hearts': dict(PRESETS['hearts']['cond'], preset='hearts'),
}


def _clean(s):
    return str(s).replace('\t', ' ').replace('\r', ' ').replace('\n', ' ')


def ensure_icon(folder, a):
    """The achievement's 128x128 icon PNG: the one the editor saved, else its emblem drawn now."""
    png = folder / 'icons' / f'{a["id"]}.png'
    if not png.is_file():
        c = emblem_icon(a.get('emblem') or 'triforce', a['rarity'])
        png_write(png, c.w, c.h, c.to_rgba8())
    return png


def compile_definitions(dest, romfs=None, log=print):
    """dest/achievements/achievements.json -> compiled.tsv and icons/<id>.rgba. Returns the number of achievements."""
    folder = Path(dest) / 'achievements'
    defs = json.loads((folder / 'achievements.json').read_text(encoding='utf-8'))
    gd = load_game_data(romfs or Path(dest) / 'romfs')
    lists, lines, ids = {}, [], set()
    for a in defs['achievements']:
        if not re.fullmatch(r'[a-z0-9_]{1,40}', a['id']) or a['id'] in ids:
            raise CompileError(f'{a["id"]!r}: an id is 1 to 40 of a-z, 0-9 and _, and unique')
        ids.add(a['id'])
        if a['rarity'] not in RARITIES:
            raise CompileError(f'{a["id"]}: rarity {a["rarity"]!r}')
        try:
            kind, *args = resolve(a['condition'], gd, lists)
        except CompileError as e:
            raise CompileError(f'{a["id"]}: {e}') from None
        png = ensure_icon(folder, a)
        w, h, rgba = png_read(png)
        small = Canvas.from_rgba8(w, h, rgba).resized(64, 64)
        (folder / 'icons' / f'{a["id"]}.rgba').write_bytes(small.to_rgba8())
        lines.append('\t'.join(['A', a['id'], a['rarity'], kind, *args, '1' if a.get('hidden') else '0',
                                _clean(a['name']), _clean(a['description'])]))
    count = len(lines)
    # The progress the multiplayer room shows (runtime/net_mp.c): S lines, a stat name and a condition as on A lines.
    for stat, cond in STATS.items():
        kind, *args = resolve(dict(cond, min=0), gd, lists)
        lines.append('\t'.join(['S', stat, kind, *args]))
    out = [f'# {FORMAT}: written by tools/achievements.py from achievements.json; the editor rewrites it',
           *lines, *(f'{lid}\t{key}' for key, lid in lists.items())]
    tmp = folder / 'compiled.tsv.tmp'
    tmp.write_text('\n'.join(out) + '\n', encoding='utf-8', newline='\n')
    os.replace(tmp, folder / 'compiled.tsv')
    log(f'achievements: {count} compiled, {len(lists)} flag lists')
    return count


def read_compiled(path):
    """([achievement dicts], {list id: [hash]}) of a compiled.tsv."""
    items, lists = [], {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('#'):
            continue
        f = line.split('\t')
        if f[0] == 'A':
            items.append(dict(id=f[1], rarity=f[2], kind=f[3], a=f[4], b=f[5], c=f[6], d=f[7], hidden=f[8] == '1',
                              name=f[9], description=f[10]))
        elif f[0].startswith('L'):
            lists[f[0]] = [int(h, 16) for h in f[1].split(',')]
    return items, lists


# progress.sav: a 0x20-byte header (magic 0x01020304, GameDataList revision, offset of the data area), then
# (hash, value) pairs up to the data area, in sections that each start with a pair (0, type index). Bool, Int, UInt
# and Float sit in the value; arrays and strings are at that offset in the file (a count, then the items).
SAVE_TYPES = ('Bool', 'BoolArray', 'Int', 'IntArray', 'Float', 'FloatArray', 'Enum', 'EnumArray', 'Vector2',
              'Vector2Array', 'Vector3', 'Vector3Array', 'String16', 'String16Array', 'String32', 'String32Array',
              'String64', 'String64Array', 'Binary', 'BinaryArray', 'UInt', 'UIntArray', 'Int64', 'Int64Array',
              'UInt64', 'UInt64Array')


def read_save(path):
    """(file bytes, {hash: (type, raw value)}) of a progress.sav."""
    d = Path(path).read_bytes()
    magic, _, data_at = struct.unpack_from('<III', d, 0)
    if magic != 0x01020304:
        raise ValueError(f'{path}: not a progress.sav')
    vals, kind, pos = {}, None, 0x20
    while pos + 8 <= min(data_at, len(d)):
        h, v = struct.unpack_from('<II', d, pos)
        if h == 0:
            kind = SAVE_TYPES[v] if v < len(SAVE_TYPES) else None
        else:
            vals[h] = (kind, v)
        pos += 8
    return d, vals


def progress(items, lists, save, unlocked=()):
    """{id: (current, target)} of every achievement against a read_save() result; done when current >= target."""
    d, vals = save

    def num(h):
        t, v = vals.get(h, (None, 0))
        if t == 'Int': return struct.unpack('<i', struct.pack('<I', v))[0]
        if t == 'Float': return struct.unpack('<f', struct.pack('<I', v))[0]
        return v if t == 'UInt' else 0

    def ints(h):
        t, off = vals.get(h, (None, 0))
        if t not in ('IntArray', 'UIntArray'): return []
        n = struct.unpack_from('<I', d, off)[0]
        return list(struct.unpack_from(f'<{n}i', d, off + 4))

    out = {}
    for a in items:
        k = a['kind']
        if k == 'bool':
            out[a['id']] = (int(vals.get(int(a['a'], 16), (0, 0))[1] == 1), 1)
        elif k == 'enum':
            out[a['id']] = (int(vals.get(int(a['a'], 16), (0, 0))[1] == int(a['b'], 16)), 1)
        elif k == 'num':
            gated = a['d'] and vals.get(int(a['d'], 16), (0, 0))[1] != 1
            out[a['id']] = (0 if gated else num(int(a['a'], 16)), int(a['b']))
        elif k == 'arrsum':
            out[a['id']] = (sum(x for x in ints(int(a['a'], 16)) if x > 0), int(a['b']))
        elif k == 'count':
            want = int(a['c'], 16) or 1
            out[a['id']] = (sum(1 for h in lists[a['a']] if vals.get(h, (0, None))[1] == want), int(a['b']))
        elif k == 'pouch':
            t, off = vals.get(int(a['a'], 16), (None, 0))
            stock = ints(int(a['b'], 16))
            have = 0
            if t == 'String64Array':
                n = struct.unpack_from('<I', d, off)[0]
                for i in range(min(n, len(stock))):
                    if d[off + 4 + 64 * i:off + 4 + 64 * (i + 1)].split(b'\0')[0].decode() == a['d']:
                        have += max(0, stock[i])
            out[a['id']] = (have, int(a['c']))
    for a in items:
        if a['kind'] == 'mp':   # counted by the game in rooms, not in the save
            out[a['id']] = (0, int(a['b']))
    others = [a['id'] for a in items if a['kind'] not in ('all', 'mp')]
    done = sum(1 for i in others if i in unlocked or out[i][0] >= out[i][1])
    for a in items:
        if a['kind'] == 'all':
            out[a['id']] = (done, len(others))
    return out


def merge_defaults(path):
    """Add to the player's set the default achievements a newer release brought: those whose id is in neither the set nor
    its "offered" list (every default id ever offered to it, so one the player deleted stays deleted). They go before
    the platinum one. Returns the ids added."""
    defs = json.loads(path.read_text(encoding='utf-8'))
    items = defs['achievements']
    have = {a['id'] for a in items}
    offered = set(defs.get('offered', have))   # a set written before the list existed: what it holds was offered
    new = [a for a in default_definitions()['achievements'] if a['id'] not in have and a['id'] not in offered]
    at = next((i for i, a in enumerate(items) if a['condition']['kind'] == 'all'), len(items))
    items[at:at] = new
    defs['offered'] = sorted(offered | {i for i, *_ in DEFAULTS})
    if new or 'offered' not in json.loads(path.read_text(encoding='utf-8')):
        tmp = path.with_suffix('.json.tmp')
        tmp.write_text(json.dumps(defs, ensure_ascii=False, indent=1), encoding='utf-8')
        os.replace(tmp, path)
    return [a['id'] for a in new]


def write_defaults(dest, force=False):
    folder = Path(dest) / 'achievements'
    path = folder / 'achievements.json'
    if path.is_file() and not force:
        added = merge_defaults(path)
        if added:
            print(f'achievements: {len(added)} new default achievements added: {", ".join(added)}')
        return False
    folder.mkdir(parents=True, exist_ok=True)
    defs = default_definitions()
    path.write_text(json.dumps(defs, ensure_ascii=False, indent=1), encoding='utf-8')
    for a in defs['achievements']:
        c = emblem_icon(a['emblem'], a['rarity'])
        png_write(folder / 'icons' / f'{a["id"]}.png', c.w, c.h, c.to_rgba8())
    return True


def main(argv):
    if len(argv) != 2 or argv[0] not in ('defaults', 'compile'):
        sys.exit(__doc__)
    dest = Path(argv[1])
    if argv[0] == 'defaults' and write_defaults(dest):
        print('achievements: default set written')
    try:
        compile_definitions(dest)
    except CompileError as e:
        sys.exit(f'achievements: {e}')


if __name__ == '__main__':
    main(sys.argv[1:])
