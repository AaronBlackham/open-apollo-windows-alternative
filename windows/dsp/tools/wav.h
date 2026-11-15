/*
 * wav.h - minimal WAV reader/writer for the offline DSP tools (PC only).
 * Reads PCM 16/24/32-bit and float 32/64-bit, plain or WAVE_FORMAT_EXTENSIBLE.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once
#include <stddef.h>

typedef struct wav_audio {
    int    channels;
    int    rate;
    long   frames;
    float *data;            /* interleaved, frames * channels */
} wav_audio;

int  wav_read(const char *path, wav_audio *out, char *err, size_t errlen);
int  wav_write(const char *path, const wav_audio *a, int pcm24);
void wav_free(wav_audio *a);
