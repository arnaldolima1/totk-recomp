// Voice chat (voice.h). One capture thread reads the microphone through SDL3 and encodes 20 ms Opus frames into a
// small queue the connection's thread sends (net_mp.c VOICE lines); frames from the room are decoded into one ring per
// friend, which SDL's audio thread mixes over the game's audio (audio_mixer.c sdl_pull).
#include "voice.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <opus/opus.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

enum {
    RATE = 48000, FRAME = 960,      // 20 ms at 48 kHz, mono
    OUT_QUEUE = 32,                 // frames waiting for the connection (0.64 s); the oldest go when it backs up
    RING = RATE,                    // one second of a friend's voice
    PREFILL = 3 * FRAME,            // 60 ms buffered before a friend's voice starts, against the network's jitter
    HANG_FRAMES = 20,               // open microphone: 400 ms sent after the speech falls under the gate
    HEARD_MS = 400,                 // a friend counts as speaking this long after their last frame
};
static const float GATE_RMS = 0.012f;   // open microphone: about -38 dBFS, over a quiet room's noise

// ---- what the connection sends
static PlatMutex out_lock = PLAT_MUTEX_INIT;
static uint8_t out_q[OUT_QUEUE][VOICE_PACKET_MAX];
static int out_len[OUT_QUEUE], out_head, out_tail;

static volatile int32_t ptt, joined, sending, mic_failed, started;
static volatile float level;

// ---- what the friends say
typedef struct {
    char name[VOICE_NAME_MAX];
    OpusDecoder *dec;
    float ring[RING];
    uint64_t rd, wr;    // samples read and written; wr - rd are buffered
    double pos;         // fractional read position for an output rate other than 48 kHz
    int playing;
    uint64_t heard_ms;
} Speaker;
static PlatMutex in_lock = PLAT_MUTEX_INIT;
static Speaker speakers[VOICE_SPEAKERS];
static double out_step = 1.0;   // 48 kHz samples per output frame

static int voice_on(void) { return cfg_bool("TOTK_MP_VOICE"); }
static int open_mode(void) { return !strcmp(cfg_value("TOTK_MP_VOICE_MODE"), "open"); }

// ---- capture

static void push_packet(const uint8_t *p, int n) {
    plat_mutex_lock(&out_lock);
    int next = (out_tail + 1) % OUT_QUEUE;
    if (next == out_head) out_head = (out_head + 1) % OUT_QUEUE;   // full: the oldest frame is stale anyway
    memcpy(out_q[out_tail], p, (size_t)n);
    out_len[out_tail] = n;
    out_tail = next;
    plat_mutex_unlock(&out_lock);
}

static void capture_main(void *arg) {
    (void)arg;
    SDL_AudioStream *mic = NULL;
    OpusEncoder *enc = NULL;
    int running = 0, hang = 0, audio_ok = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!audio_ok) rt_log("voice: SDL audio init failed (%s)", SDL_GetError());
    float frame[FRAME];
    uint8_t packet[VOICE_PACKET_MAX];
    for (;;) {
        int want_open = audio_ok && joined && voice_on();
        int want_run = want_open && (open_mode() || ptt);
        if (want_open && !mic && !mic_failed) {
            SDL_AudioSpec spec = {SDL_AUDIO_F32, 1, RATE};
            mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, NULL, NULL);
            int err = 0;
            if (!enc) enc = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &err);
            if (!mic || !enc) {
                rt_log("voice: no microphone (%s) or encoder (%d): voice is receive-only", mic ? "ok" : SDL_GetError(), err);
                if (mic) SDL_DestroyAudioStream(mic);
                mic = NULL;
                mic_failed = 1;
            } else {
                opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
                opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
                rt_log("voice: microphone '%s' opened",
                       SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(mic)) ? SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(mic)) : "?");
                running = 0;
            }
        } else if (!want_open && mic) {   // voice off or out of the room: the microphone is released
            SDL_DestroyAudioStream(mic);
            mic = NULL, running = 0, sending = 0, level = 0;
            rt_log("voice: microphone closed");
        }
        if (!want_open) mic_failed = 0;   // tried again the next time voice is wanted
        if (mic && want_run != running) {
            if (want_run) {
                SDL_ClearAudioStream(mic);   // nothing said before the key went down
                SDL_ResumeAudioStreamDevice(mic);
            } else {
                SDL_PauseAudioStreamDevice(mic);
                sending = 0, level = 0, hang = 0;
            }
            running = want_run;
        }
        if (!running) {
            plat_sleep_ms(10);
            continue;
        }
        int got = 0;
        while (SDL_GetAudioStreamAvailable(mic) >= (int)sizeof frame) {
            if (SDL_GetAudioStreamData(mic, frame, (int)sizeof frame) != (int)sizeof frame) break;
            got = 1;
            double sq = 0;
            for (int i = 0; i < FRAME; i++) sq += (double)frame[i] * frame[i];
            float rms = (float)sqrt(sq / FRAME);
            level = fminf(1.0f, rms * 8);
            int speak = 1;
            if (open_mode()) {
                if (rms >= GATE_RMS) hang = HANG_FRAMES;
                speak = hang > 0;
                if (hang > 0) hang--;
            }
            sending = speak;
            if (!speak) continue;
            int n = opus_encode_float(enc, frame, FRAME, packet, (opus_int32)sizeof packet);
            if (n > 0) push_packet(packet, n);
        }
        if (!got) plat_sleep_ms(5);
    }
}

static void start(void) {
    if (plat_cas32(&started, 1, 0) == 0 && !plat_thread_spawn(capture_main, NULL, "voice", 0))
        rt_log("voice: no thread for the microphone");
}

void voice_connected(int connected) {
    joined = connected;
    if (connected) start();
    if (!connected) {
        plat_mutex_lock(&out_lock);
        out_head = out_tail = 0;
        plat_mutex_unlock(&out_lock);
    }
}

int voice_take_packet(uint8_t *out, int cap) {
    plat_mutex_lock(&out_lock);
    int n = 0;
    if (out_head != out_tail) {
        n = out_len[out_head] <= cap ? out_len[out_head] : 0;
        if (n) memcpy(out, out_q[out_head], (size_t)n);
        out_head = (out_head + 1) % OUT_QUEUE;
    }
    plat_mutex_unlock(&out_lock);
    return n;
}

void voice_ptt(int down) { ptt = down != 0; }
int voice_transmitting(void) { return sending && joined && voice_on(); }
float voice_level(void) { return joined && voice_on() ? level : 0; }
int voice_mic_error(void) { return mic_failed; }

// ---- playback

void voice_receive(const char *from, const uint8_t *data, int n) {
    if (!voice_on() || n <= 0) return;
    float pcm[FRAME * 6];   // Opus frames are at most 120 ms
    plat_mutex_lock(&in_lock);
    Speaker *s = NULL, *free_one = NULL, *oldest = &speakers[0];
    for (int i = 0; i < VOICE_SPEAKERS; i++) {
        Speaker *c = &speakers[i];
        if (c->dec && !strcmp(c->name, from)) s = c;
        if (!c->dec && !free_one) free_one = c;
        if (c->heard_ms < oldest->heard_ms) oldest = c;
    }
    if (!s) {   // a new voice: a free slot, else the one silent the longest
        s = free_one ? free_one : oldest;
        int err = 0;
        if (!s->dec) s->dec = opus_decoder_create(RATE, 1, &err);
        else opus_decoder_ctl(s->dec, OPUS_RESET_STATE);
        if (!s->dec) {
            plat_mutex_unlock(&in_lock);
            return;
        }
        snprintf(s->name, sizeof s->name, "%s", from);
        s->rd = s->wr = 0, s->pos = 0, s->playing = 0;
    }
    int got = opus_decode_float(s->dec, data, n, pcm, FRAME * 6, 0);
    for (int i = 0; i < got; i++) s->ring[s->wr++ % RING] = pcm[i];
    if (s->wr - s->rd > RING) s->rd = s->wr - RING;   // more than a second behind: the oldest goes
    s->heard_ms = plat_now_ms();
    plat_mutex_unlock(&in_lock);
}

void voice_output_rate(int rate) { out_step = rate > 0 ? (double)RATE / rate : 1.0; }

void voice_mix(float *out, uint32_t frames) {
    if (!started) return;
    double vol = cfg_double("TOTK_MP_VOICE_VOL") / 100.0;
    float g = (float)(vol < 0 ? 0 : vol > 2 ? 2 : vol);
    plat_mutex_lock(&in_lock);
    for (int k = 0; k < VOICE_SPEAKERS; k++) {
        Speaker *s = &speakers[k];
        if (!s->dec) continue;
        if (!s->playing) {
            if (s->wr - s->rd < PREFILL) continue;
            s->playing = 1, s->pos = 0;
        }
        for (uint32_t i = 0; i < frames; i++) {
            if (s->wr - s->rd < 2) {   // ran dry: buffer up again before the next word
                s->playing = 0;
                break;
            }
            float a = s->ring[s->rd % RING], b = s->ring[(s->rd + 1) % RING];
            float v = (a + (b - a) * (float)s->pos) * g;
            out[2 * i] += v, out[2 * i + 1] += v;
            s->pos += out_step;
            while (s->pos >= 1.0) s->pos -= 1.0, s->rd++;
        }
    }
    plat_mutex_unlock(&in_lock);
}

int voice_speakers(char names[][VOICE_NAME_MAX], int max) {
    uint64_t now = plat_now_ms();
    int n = 0;
    plat_mutex_lock(&in_lock);
    for (int k = 0; k < VOICE_SPEAKERS && n < max; k++)
        if (speakers[k].dec && now - speakers[k].heard_ms < HEARD_MS) snprintf(names[n++], VOICE_NAME_MAX, "%s", speakers[k].name);
    plat_mutex_unlock(&in_lock);
    return n;
}
