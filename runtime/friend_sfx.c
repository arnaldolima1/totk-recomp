// Sounds of friends' Links in a multiplayer room: the game's own footsteps, swings, jumps and the like, which
// tools/link_sounds.py decodes from the player's RomFS into <profile>/link-sounds.bin (its format is described there).
// The overlay (ui/menu.cpp figure_sounds) starts them as the friends' figures move; they mix on SDL's audio thread on
// top of the game's output (audio_mixer.c sdl_pull), each with its own gain and pan.
#include "friend_sfx.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

enum { MAX_CLIPS = 512, MAX_VOICES = 24, NAME_MAX = 48 };
typedef struct {
    char name[NAME_MAX];   // Mt_Grass_Pl_Jump_00
    char base[NAME_MAX];   // the name without its variant number (Mt_Grass_Pl_Jump)
    uint32_t rate, count;
    const int16_t *pcm;
} Clip;
typedef struct {
    const Clip *clip;
    double pos, step;      // in the clip's samples; its samples per output frame
    float left, right;
} Voice;

static Clip clips[MAX_CLIPS];
static int nclips, tried;
static Voice voices[MAX_VOICES];
static PlatMutex lock = PLAT_MUTEX_INIT;
static _Atomic int out_rate = 48000;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void load(void) {
    tried = 1;
    char path[1200];
    snprintf(path, sizeof path, "%s/link-sounds.bin", rt_config()->profile);
    uint64_t n = 0;
    uint8_t *d = (uint8_t *)plat_file_read(path, &n);
    if (!d || n < 12 || memcmp(d, "LNKS", 4) || rd32(d + 4) != 1) {
        rt_log("friend sounds: %s %s; friends are silent", path, d ? "is not a sound file this build reads" : "not found");
        free(d);
        return;
    }
    uint32_t count = rd32(d + 8);
    uint64_t at = 12;
    for (uint32_t i = 0; i < count && nclips < MAX_CLIPS; i++) {
        if (at + 56 > n) break;
        Clip *c = &clips[nclips];
        memcpy(c->name, d + at, NAME_MAX - 1), c->name[NAME_MAX - 1] = 0;
        memcpy(c->base, c->name, NAME_MAX);
        c->rate = rd32(d + at + 48), c->count = rd32(d + at + 52);
        at += 56;
        if (at + 2ull * c->count > n || !c->rate || c->rate > 192000) break;
        c->pcm = (const int16_t *)(d + at);
        at += 2ull * c->count;
        size_t len = strlen(c->base);   // drop the variant number and the underscore before it
        while (len && c->base[len - 1] >= '0' && c->base[len - 1] <= '9') len--;
        if (len && c->base[len - 1] == '_') len--;
        c->base[len] = 0;
        nclips++;
    }
    // d stays for good: the clips point into it
    rt_log("friend sounds: %d clips from %s", nclips, path);
}

void friend_sfx_rate(int rate) {
    if (rate > 0) out_rate = rate;
}

int friend_sfx_play(const char *base, float gain, float pan) {
    if (!tried) load();
    if (!nclips || gain <= 0.001f) return 0;
    int pick[16], n = 0;
    for (int i = 0; i < nclips && !n; i++)   // a variant named in full (PV000_02) first
        if (!strcmp(clips[i].name, base)) pick[n++] = i;
    for (int i = 0; !n && i < nclips; i++)   // else the variants of that name
        if (!strcmp(clips[i].base, base)) {
            for (int j = i; j < nclips && n < 16; j++)
                if (!strcmp(clips[j].base, base)) pick[n++] = j;
        }
    if (!n) return 0;
    static uint32_t seed = 0x9e3779b9u;
    seed = seed * 1664525u + 1013904223u;
    const Clip *c = &clips[pick[(seed >> 16) % (uint32_t)n]];
    if (pan < -1) pan = -1;
    if (pan > 1) pan = 1;
    float l = gain * sqrtf(0.5f * (1 - pan)), r = gain * sqrtf(0.5f * (1 + pan));   // equal power
    plat_mutex_lock(&lock);
    int slot = 0;   // a free voice, else the one furthest through its clip
    double most = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].clip) {
            slot = i;
            break;
        }
        double done = voices[i].pos / voices[i].clip->count;
        if (done > most) most = done, slot = i;
    }
    voices[slot] = (Voice){c, 0, (double)c->rate / out_rate, l, r};
    plat_mutex_unlock(&lock);
    return 1;
}

void friend_sfx_mix(float *out, uint32_t frames) {
    if (!nclips) return;
    plat_mutex_lock(&lock);
    for (int k = 0; k < MAX_VOICES; k++) {
        Voice *v = &voices[k];
        if (!v->clip) continue;
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t s = (uint32_t)v->pos;
            if (s + 1 >= v->clip->count) {
                v->clip = NULL;
                break;
            }
            float a = v->clip->pcm[s], b = v->clip->pcm[s + 1], t = (float)(v->pos - s);
            float x = (a + (b - a) * t) * (1.0f / 32768.0f);
            out[2 * i] += x * v->left, out[2 * i + 1] += x * v->right;
            v->pos += v->step;
        }
    }
    plat_mutex_unlock(&lock);
}
