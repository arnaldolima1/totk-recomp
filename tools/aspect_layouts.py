"""Layout edits that give TotK's interface the proportions of a 16:10 or 21:9 game resolution.

runtime/resolution.c aspect_init gives the 3D camera and the 2D canvas the output's shape, but the game still lays
its interface out in a 1920x1080 space and draws that space over the whole output: at 16:10 every layout is 11 %
taller than drawn, at 21:9 34 % wider. tools/make_romfs_patches.py runs apply() on the layout archive for each shape
and writes the result as romfs-patches/16x10 and romfs-patches/21x9; runtime/romfs.c serves the one of the output's
shape.

The edits follow the behaviour of Fruithapje21's TOTK-Ultrawide (GPL-3.0, romfs_script.py) for its 1.0.0 to 1.2.1
mods, written down as a specification and implemented from it; no code of it is used. With k the interface's shrink
on the output's stretched axis, (16/9) / aspect on a wider output and aspect / (16/9) on a narrower one:
- every layout's RootPane is scaled by k on that axis, so its panes keep their proportions; the full-screen layouts
  (FULL_SCREEN) are left out;
- panes that must still cover the screen (backgrounds, blur, fades, the camera scope) are scaled by 1 / k on it, a
  few that a full-screen layout holds by k;
- layouts and panes anchored to an edge move to it again: the HUD to the top and bottom edges at 16:10 and to the
  left and right ones at 21:9, the minimap through the end point of its slide-in animation;
- at 21:9 the hit boxes of three button layouts and two screen captures of ChallengeLog_00 move off the screen.

Every edit is a value of k that equals the file's own value at k = 1 (16:9); apply() checks that before it writes,
so an archive of another layout stops the build instead of drawing a broken interface. Run with .venv-py314.
"""
import struct

ASPECT_16_9 = 16 / 9
# The aspect each overlay is made for: 16:10 is one shape (1280x800, 1920x1200, 2560x1600); the 21:9 monitors are
# 2.370 (2560x1080, 5120x2160) to 2.400 (3840x1600), and 3440x1440 (43:18) lies between, so no size is more than 1 %
# from it. The camera always takes the output's exact shape (runtime/resolution.c aspect_init).
SHAPES = {'16x10': 16 / 10, '21x9': 43 / 18}

# Layouts that cover the whole screen and keep their size
FULL_SCREEN = {'AppMenuBG_00', 'PauseMenuBG_00', 'ShopMenuParasailBG_00', 'ShortCutBG_00', 'TitleMenuBG_00',
               'EventFade_00', 'EventFadeWipe_00', 'CameraShutter_00', 'PaAppPictureBookDetail_00',
               'AppMenuOverlay_00'}
SYSTEM_MENUS = ('SystemLoadList_00', 'SystemOption_00', 'SystemSaveLoad_00')
HIDE = 10000.0   # a translate that puts a pane outside the screen


def s2(k):
    return 1 - k   # the part of the screen the scaled layout no longer covers


def s3(k):
    return 1 / k - 1   # the same in the layout's own (scaled) units


def shrink(k):
    return k


def grow(k):
    return 1 / k


def edits(table):
    """[(layout, pane, field, value(k))] from rows of (layouts, panes, fields, value): each layout, pane and field."""
    one = lambda x: (x,) if isinstance(x, str) else x
    return [(lay, pane, field, value) for lays, panes, fields, value in table
            for lay in one(lays) for pane in one(panes) for field in one(fields)]


# Fields: tx, ty (translate), sx, sy (scale) of a pane. value(1) is the file's own value; value None hides the pane.
WIDE = edits([
    ('AppAlbum_00', 'N_FadeInOut_00', 'sx', shrink),
    ('AppMenuOverlay_00', ('N_FadeInOut_00', 'N_FadeInOut_01', 'N_Icon_00', 'N_TitleOne_01'), 'sx', shrink),
    ('CameraPointer_00', 'N_Type_01', 'sx', shrink),
    ('CameraSystemWindow_00', ('N_AlbumCon_00', 'Pa_BtnL_00', 'Pa_BtnR_00', 'T_Text_00'), 'sx', shrink),
    ('LoadingFade_00', ('P_SideBGDeco_00', 'Pa_IconPlayer_00', 'Pa_LargeDungeonMap_00', 'Pa_LargeDungeonMap_01',
                        'Pa_Map_00', 'Pa_Map_01'), 'sx', shrink),
    ('PauseMenuOverlay_00', ('P_DecoL_00', 'P_DecoR_00'), 'sx', shrink),
    ('SystemActionGuide_00', ('A_Title_00', 'P_DecoL_01', 'P_DecoL_02'), 'sx', shrink),
    (SYSTEM_MENUS, ('A_Title_00', 'P_DecoL_00', 'P_DecoR_00'), 'sx', shrink),

    ('AppAlbum_00', 'N_Open_01', 'sx', grow),
    ('AppMap_00', ('N_BlurInOut_00', 'N_BlurInOut_01', 'N_BlurInOut_02', 'N_BlurInOut_03'), 'sx', grow),
    (('AppOpenDemoWindow_00', 'GameOver_00', 'LoadingFade_00', 'PaLargeDungeonMap_00', 'PaRecipeCardList_00'),
     'P_BG_00', 'sx', grow),
    ('LoadingFade_00', 'N_Base_00', 'sx', grow),
    (('AutoBuilder_00', 'OwnedHorseList_00', 'ShopMenu_00', 'StaffRoll_00'), 'N_BG_00', 'sx', grow),
    ('CameraPointer_00', 'N_InOutScope_00', ('sx', 'sy'), grow),
    ('CameraPointer_00', ('Pa_CameraFocus_00', 'S_Scissor_00', 'W_Shadow_01'), 'sx', grow),
    ('CameraSystemWindow_00', 'N_InOut_00', 'sx', grow),
    ('MessageLog_01', 'P_Capture_00', 'sx', grow),
    ('PauseMenuOverlay_00', ('P_FooterBase_00', 'S_DecoScissor_00'), 'sx', grow),
    (('SystemActionGuide_00',) + SYSTEM_MENUS, ('N_Header_00', 'N_SystemBG_00'), 'sx', grow),

    ('AppMap_00', 'N_MiniMap_00', 'tx', lambda k: -270 * s2(k)),
    ('ChallengeLog_00', ('C_CaptureUp_00', 'C_CaptureUp_01'), 'tx', None),
    ('PaCommonBtnTitle_00', ('B_Hit_00', 'N_Cursor_00'), 'tx', None),
    ('PaMessageBtn_00', 'B_Hit_00', 'tx', None),
    ('LoadingFade_00', ('N_Logo_00', 'N_Status_00'), 'tx', lambda k: 960 * s3(k)),
    ('LoadingFade_00', 'N_Tips_00', 'tx', lambda k: -960 * s3(k)),
    (('PauseMenuOverlay_00',) + SYSTEM_MENUS, 'P_DecoL_00', 'tx', lambda k: -(675 + 960 * s3(k)) * k),
    (('PauseMenuOverlay_00',) + SYSTEM_MENUS, 'P_DecoR_00', 'tx', lambda k: (737 + 960 * s3(k)) * k),
    ('SaveLoadIcon_00', 'RootPane', 'tx', lambda k: -960 * s2(k)),
    ('SystemActionGuide_00', 'P_DecoL_01', 'tx', lambda k: -(675 + 960 * s3(k)) * k),
    ('SystemActionGuide_00', 'P_DecoL_02', 'tx', lambda k: (675 + 960 * s3(k)) * k),

    # The HUD at the screen's left and right edges instead of the middle 16:9 (TOTK-Ultrawide's "corner")
    (('EnvironmentInfo_00', 'EventSkip_00', 'InstantTipsAppMenu_00', 'InstantTipsWithPict_00', 'InstantTips_00',
      'NumDisplaySecond_00', 'NumDisplay_00', 'PickUpWin_00'), 'RootPane', 'tx', lambda k: 960 * s2(k)),
    ('Location_00', 'N_Small_00', 'tx', lambda k: -(388 + 960 * s3(k))),
    ('PlayerStatus3D_00', 'N_ExtraStaminaTarget_01', 'tx', lambda k: -(745 + 960 * s3(k))),
    ('PlayerStatus_00', 'N_DevilDemoTargetC_00', 'tx', lambda k: -(284 + 960 * s3(k))),
    ('PlayerStatus_00', ('N_DevilDemoTargetLT_00', 'N_HeartGaugeTargetLT_00'), 'tx', lambda k: -(576 + 960 * s3(k))),
    ('PlayerStatus_00', 'N_HeartPos_00', 'tx', lambda k: -960 * s3(k)),
    ('PlayerStatus_00', 'Pa_CompanionLife_00', 'tx', lambda k: -(840 + 960 * s3(k))),
])

TALL = edits([
    ('AppAlbum_00', 'N_FadeInOut_00', 'sy', shrink),
    ('AppMenuOverlay_00', ('N_FadeInOut_00', 'N_FadeInOut_01', 'N_Icon_00', 'N_TitleOne_01'), 'sy', shrink),
    ('CameraPointer_00', 'N_Type_01', 'sy', shrink),
    ('CameraSystemWindow_00', 'N_FirstOut_00', 'sy', shrink),
    ('LoadingFade_00', ('Pa_IconPlayer_00', 'Pa_LargeDungeonMap_00', 'Pa_LargeDungeonMap_01', 'Pa_Map_00',
                        'Pa_Map_01'), 'sy', shrink),

    ('AppAlbum_00', 'N_Open_01', 'sy', grow),
    ('AppMap_00', ('N_BlurInOut_00', 'N_BlurInOut_01', 'N_BlurInOut_02', 'N_BlurInOut_03'), 'sy', grow),
    (('AppOpenDemoWindow_00', 'GameOver_00', 'LoadingFade_00', 'PaLargeDungeonMap_00', 'PaRecipeCardList_00'),
     'P_BG_00', 'sy', grow),
    ('LoadingFade_00', 'N_Base_00', 'sy', grow),
    (('AutoBuilder_00', 'OwnedHorseList_00', 'ShopMenu_00', 'StaffRoll_00'), 'N_BG_00', 'sy', grow),
    ('CameraPointer_00', ('N_InOutScope_00', 'Pa_CameraFocus_00', 'S_Scissor_00', 'W_Shadow_01'), 'sy', grow),
    ('CameraSystemWindow_00', 'N_InOut_00', 'sy', grow),
    ('MessageLog_01', 'P_Capture_00', 'sy', grow),
    (('SystemActionGuide_00',) + SYSTEM_MENUS, 'N_SystemBG_00', 'sy', grow),

    # The HUD at the screen's top and bottom edges instead of the middle 16:9
    (('AppAlbum_00', 'AppPictureBook_00', 'ChallengeLog_00', 'Challenge_00', 'InstantTipsWithPict_00', 'InstantTips_00',
      'MiniGameTimer_00', 'NumDisplaySecond_00', 'NumDisplay_00', 'PickUpWin_00', 'PlayerStatus_00', 'Pouch_00',
      'ShopMenu_00', 'SystemActionGuide_00') + SYSTEM_MENUS, 'RootPane', 'ty', lambda k: 540 * s2(k)),
    (('CtrlGuide_00', 'DoCommand_00', 'EnvironmentInfo_00', 'HorseGauge_00', 'MessageDialog_00', 'MessageLog_01',
      'PauseMenuOverlay_00', 'SaveLoadIcon_00', 'TreasureFullShortCut_00'), 'RootPane', 'ty', lambda k: -540 * s2(k)),
    ('AppAlbum_00', 'N_PicLoad_01', 'ty', lambda k: -540 * s2(k)),
    ('AppMap_00', 'N_AllMapTime_00', 'ty', lambda k: -540 * s3(k)),
    ('AppMenuOverlay_00', ('A_Align_00', 'Pa_GuideLeft_00'), 'ty', lambda k: -(505 + 540 * s3(k))),
    ('AppMenuOverlay_00', 'N_TitleTab_00', 'ty', lambda k: 491 + 540 * s3(k)),
    ('LoadingFade_00', 'N_Logo_00', 'ty', lambda k: -540 * s3(k)),
    ('LoadingFade_00', 'N_Sage_00', 'ty', lambda k: -477 - 2 * 540 * s3(k)),
    ('LoadingFade_00', 'N_Status_00', 'ty', lambda k: 540 * s3(k)),
    ('LoadingFade_00', 'N_Tips_00', 'ty', lambda k: -(360 + 540 * s3(k))),
    ('Location_00', 'N_Large_00', 'ty', lambda k: 270 + 540 * s3(k)),
    ('Location_00', 'N_Small_00', 'ty', lambda k: -(395 + 540 * s3(k))),
    ('MessageLog_01', 'P_Capture_00', 'ty', lambda k: 540 * s3(k)),
    ('PaExtraStaminaGauge_00', ('N_BasePattern_00', 'N_GaugeMainPattern_00'), 'ty', lambda k: 540 * s3(k)),
    ('Pouch_00', ('N_Info_00', 'Pa_ExtraStamina_00'), 'ty', lambda k: -540 * s3(k)),
    ('ShopMenu_00', 'A_AlignGuide_00', 'ty', lambda k: -(477 + 540 * s3(k))),
    (('SystemActionGuide_00',) + SYSTEM_MENUS, 'N_SystemBG_00', 'ty', lambda k: -540 * s3(k)),
    ('SystemLoadList_00', 'Pa_Guide_00', 'ty', lambda k: -(474 + 540 * s3(k))),
    ('SystemOption_00', 'A_Guide_00', 'ty', lambda k: -(474 + 540 * s3(k))),
])

FIELDS = {'tx': 44, 'ty': 48, 'sx': 68, 'sy': 72}   # offsets in a pane section (after magic, size, flags and name)
PANES = {b'pan1', b'pic1', b'txt1', b'wnd1', b'bnd1', b'prt1', b'ali1', b'scr1', b'cpt1'}
FLPA_X, FLPA_Y = 0, 1   # FLPA (pane SRT) targets: translate x, y
FLTS_SX, FLTS_SY = 3, 4  # FLTS (texture SRT) targets: scale x, y


class LayoutError(Exception):
    pass


def close(a, b):
    return abs(a - b) <= 1e-3 * max(1.0, abs(b))


def panes_of(data, lo):
    """Pane name -> offset of its first pane section in the BFLYT at data[lo:]."""
    if data[lo:lo + 4] != b'FLYT':
        raise LayoutError(f'{lo:#x}: not a BFLYT')
    pos, out = lo + struct.unpack_from('<H', data, lo + 6)[0], {}
    for _ in range(struct.unpack_from('<H', data, lo + 16)[0]):
        magic, size = bytes(data[pos:pos + 4]), struct.unpack_from('<I', data, pos + 4)[0]
        if magic in PANES:
            out.setdefault(data[pos + 12:pos + 36].split(b'\0')[0].decode(), pos)
        pos += size
    return out


def curves_of(data, lo, pane, tag):
    """[(target, [absolute key offsets])] of the hermite curves of one pane and animation tag in the BFLAN at data[lo:]."""
    if data[lo:lo + 4] != b'FLAN':
        raise LayoutError(f'{lo:#x}: not a BFLAN')
    pos, out = lo + struct.unpack_from('<H', data, lo + 6)[0], []
    for _ in range(struct.unpack_from('<H', data, lo + 0x10)[0]):
        magic, size = data[pos:pos + 4], struct.unpack_from('<I', data, pos + 4)[0]
        if magic == b'pai1':
            table = pos + struct.unpack_from('<I', data, pos + 16)[0]
            for e in range(struct.unpack_from('<H', data, pos + 14)[0]):
                eo = pos + struct.unpack_from('<I', data, table + 4 * e)[0]
                if data[eo:eo + 28].split(b'\0')[0].decode() != pane:
                    continue
                for t in range(data[eo + 28]):
                    to = eo + struct.unpack_from('<I', data, eo + 32 + 4 * t)[0]
                    if data[to:to + 4] != tag:
                        continue
                    for g in range(data[to + 4]):
                        go = to + struct.unpack_from('<I', data, to + 8 + 4 * g)[0]
                        if data[go + 2] != 2:
                            raise LayoutError(f'{pane}: a {tag.decode()} curve that is not hermite')
                        keys = go + struct.unpack_from('<I', data, go + 8)[0]
                        out.append((data[go + 1], [keys + 12 * i for i in range(struct.unpack_from('<H', data, go + 4)[0])]))
        pos += size
    return out


def get(data, at):
    return struct.unpack_from('<f', data, at)[0]


def put(data, at, value):
    struct.pack_into('<f', data, at, value)


def apply(blarc, members, shape):
    """Edits the decompressed layout archive blarc (a bytearray; members: name -> (start, end), tools/
    make_romfs_patches.py sarc_members) for SHAPES[shape]. Returns the number of edited values; LayoutError when a
    layout, pane or value differs from what the edits expect."""
    aspect = SHAPES[shape]
    wide = aspect > ASPECT_16_9
    k = ASPECT_16_9 / aspect if wide else aspect / ASPECT_16_9
    axis = 'sx' if wide else 'sy'
    layouts = {n[5:-6]: lo for n, (lo, hi) in members.items() if n.startswith('blyt/') and n.endswith('.bflyt')}
    table = [(lay, 'RootPane', axis, shrink) for lay in sorted(layouts) if lay not in FULL_SCREEN]
    table += WIDE if wide else TALL
    count, cache = 0, {}
    for lay, pane, field, value in table:
        if lay not in layouts:
            raise LayoutError(f'no layout {lay}')
        panes = cache.setdefault(lay, panes_of(blarc, layouts[lay]))
        if pane not in panes:
            raise LayoutError(f'{lay}: no pane {pane}')
        at = panes[pane] + FIELDS[field]
        if value is None:
            put(blarc, at, HIDE)
        else:
            if not close(get(blarc, at), value(1.0)):
                raise LayoutError(f'{lay} {pane} {field} is {get(blarc, at):g}, the edit expects {value(1.0):g}')
            put(blarc, at, value(k))
        count += 1

    def anim(name):
        if f'anim/{name}.bflan' not in members:
            raise LayoutError(f'no animation {name}')
        return members[f'anim/{name}.bflan'][0]

    # The minimap's four cut panes slide in on straight lines from 0 at frame 0 to (765, -360) at frame 1 (hermite
    # keys: the first one's slope is the end point); the end point moves with the screen's edge.
    target, end = (FLPA_X, lambda k: 765 + 960 * s3(k)) if wide else (FLPA_Y, lambda k: -(360 + 540 * s3(k)))
    lo = anim('AppMap_00_MiniMap')
    for pane in ('N_Cut_00', 'N_Cut_01', 'N_Cut_02', 'N_Cut_03'):
        curves = [keys for t, keys in curves_of(blarc, lo, pane, b'FLPA') if t == target]
        if len(curves) != 1 or len(curves[0]) != 2:
            raise LayoutError(f'AppMap_00_MiniMap {pane}: {len(curves)} curves of the target')
        k0, k1 = curves[0]
        if not (get(blarc, k0) == 0 and get(blarc, k0 + 4) == 0 and close(get(blarc, k0 + 8), end(1.0)) and
                get(blarc, k1) == 1 and close(get(blarc, k1 + 4), end(1.0)) and get(blarc, k1 + 8) == 0):
            raise LayoutError(f'AppMap_00_MiniMap {pane}: not a line from 0 to {end(1.0):g}')
        put(blarc, k0 + 8, end(k)), put(blarc, k1 + 4, end(k))
        count += 2
    if not wide:
        # The large place name of a discovery holds its height (285, then 260) in flat keys; they move to the top edge.
        curves = [keys for t, keys in curves_of(blarc, anim('Location_00_IsDiscovery'), 'N_Large_00', b'FLPA')
                  if t == FLPA_Y]
        values = [get(blarc, key + 4) for keys in curves for key in keys]
        if len(curves) != 1 or values != [285, 285, 260]:
            raise LayoutError(f'Location_00_IsDiscovery N_Large_00: keys {values}')
        for key in curves[0]:
            put(blarc, key + 4, get(blarc, key + 4) + 540 * s3(k))
            count += 1
    # The dragon's tears map icon zooms its picture to 1.08 and 1.28 in flat keys; on the stretched axis by 1 / k more.
    curves = [keys for t, keys in curves_of(blarc, anim('PaMapIconDragonTears_00_Zoom'), 'P_Geoglyph_00', b'FLTS')
              if t == (FLTS_SX if wide else FLTS_SY)]
    values = [round(get(blarc, key + 4), 4) for keys in curves for key in keys]
    if len(curves) != 1 or values != [1.08, 1.28]:
        raise LayoutError(f'PaMapIconDragonTears_00_Zoom P_Geoglyph_00: keys {values}')
    for key in curves[0]:
        put(blarc, key + 4, get(blarc, key + 4) / k)
        count += 1
    return count
