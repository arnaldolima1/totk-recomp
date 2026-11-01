// Voice chat for the multiplayer room (net_mp.c): the microphone in 20 ms Opus frames out, the friends' frames in,
// decoded and mixed over the game's audio (audio_mixer.c). docs/multiplayer.md, "Voice".
//
// TOTK_MP_VOICE (0/1) turns it on; TOTK_MP_VOICE_MODE is push (send while the push-to-talk key, TOTK_MP_VOICE_KEY, is
// held) or open (send while the microphone hears speech); TOTK_MP_VOICE_VOL is the friends' volume in percent.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { VOICE_PACKET_MAX = 400, VOICE_NAME_MAX = 32, VOICE_SPEAKERS = 8 };

// ---- audio_mixer.c (the output)
void voice_output_rate(int rate);              // the output stream opened at rate Hz
void voice_mix(float *stereo, uint32_t frames); // SDL's audio thread: adds the friends' voices

// ---- net_mp.c (the connection's thread)
void voice_connected(int connected);           // joined the room or left it: capture may run while joined
int voice_take_packet(uint8_t *out, int cap);  // the next frame to send: its length, 0 when none
void voice_receive(const char *from, const uint8_t *data, int n);

// ---- the window adapters and the menu
void voice_ptt(int down);                      // the push-to-talk key went down or up (or the window lost focus)
int voice_transmitting(void);                  // this game is sending its microphone now
float voice_level(void);                       // the microphone's level, 0..1, while it is open
int voice_speakers(char names[][VOICE_NAME_MAX], int max);   // the friends heard in the last moment
int voice_mic_error(void);                     // 1 when no microphone could be opened

#ifdef __cplusplus
}
#endif
