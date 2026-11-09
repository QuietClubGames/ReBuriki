// Audio output: Windows waveOut playback and/or a WAV file recording.
#pragma once
#include <stdint.h>

void audio_out_init(int sample_rate, bool play, const char* wav_path);
// Queue interleaved stereo 16-bit samples (frames = sample pairs).
void audio_out_push(const int16_t* stereo, int frames);
void audio_out_pause(bool on);       // the pause menu holds playback
void audio_out_close(void);
