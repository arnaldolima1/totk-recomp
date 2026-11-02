// Sounds of friends' Links in a multiplayer room (friend_sfx.c, tools/link_sounds.py).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Starts the clip named base: that variant when it names one in full (PV000_02), else one of the variants of that name
// without the number (Mt_Grass_Pl_Jump), at random; at gain (0..1) and pan (-1 left .. 1 right). 0 when there is no
// such clip or no sound file. Any thread.
int friend_sfx_play(const char *base, float gain, float pan);
void friend_sfx_mix(float *stereo, uint32_t frames);   // SDL's audio thread: adds the playing clips
void friend_sfx_rate(int rate);                        // the output's sample rate (audio_mixer.c output_open)
#ifdef __cplusplus
}
#endif
