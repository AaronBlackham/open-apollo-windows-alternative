/*
 * dsp-render.c - run a WAV file through the Avium voice chain.
 *
 *   dsp-render in.wav out.wav [--preset voice] [--set comp.ratio=4 ...] [--pcm24] [--keep-latency]
 *   dsp-render --list            parameters, ranges, defaults
 *
 * Each channel gets its own chain. The output is time-aligned with the input
 * (the limiter lookahead is trimmed) unless --keep-latency is given.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _CRT_SECURE_NO_WARNINGS
#include "../avdsp.h"
#include "wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    int i;

    printf("usage: dsp-render <in.wav> <out.wav> [--preset NAME] [--set key=value ...] [--pcm24] [--keep-latency]\n"
           "       dsp-render --list\n\npresets:");
    for (i = 0; av_voice_preset_names[i]; i++) printf(" %s", av_voice_preset_names[i]);
    printf("\n");
}

static void list_params(void)
{
    int i;

    printf("%-14s %10s %10s %10s  %-7s %s\n", "parameter", "min", "max", "default", "unit", "what");
    for (i = 0; i < av_voice_params_count; i++) {
        const av_param_info *p = &av_voice_params_info[i];
        printf("%-14s %10g %10g %10g  %-7s %s\n", p->key, p->min, p->max, p->def, p->unit, p->label);
    }
    printf("\nEQ types: 3 = peak, 4 = low shelf, 5 = high shelf. Switches (*.on, comp.rms): 0 or 1.\n");
}

int main(int argc, char **argv)
{
    const char *inPath = NULL, *outPath = NULL, *preset = "voice";
    int pcm24 = 0, keep = 0, i, c, L;
    char err[256];
    wav_audio in, out;
    av_voice_params p;
    av_voice *v;
    float *buf;
    const char *sets[64];
    int nsets = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) { list_params(); return 0; }
        else if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc) preset = argv[++i];
        else if (strcmp(argv[i], "--set") == 0 && i + 1 < argc && nsets < 64) sets[nsets++] = argv[++i];
        else if (strcmp(argv[i], "--pcm24") == 0) pcm24 = 1;
        else if (strcmp(argv[i], "--keep-latency") == 0) keep = 1;
        else if (argv[i][0] == '-') { usage(); return 1; }
        else if (!inPath) inPath = argv[i];
        else if (!outPath) outPath = argv[i];
        else { usage(); return 1; }
    }
    if (!inPath || !outPath) { usage(); return 1; }
    if (strcmp(preset, "off") == 0) av_voice_defaults(&p);
    else if (!av_voice_preset(&p, preset)) { fprintf(stderr, "unknown preset '%s'\n", preset); usage(); return 1; }
    for (i = 0; i < nsets; i++) {
        char key[64];
        const char *eq = strchr(sets[i], '=');
        int idx;

        if (!eq || (size_t)(eq - sets[i]) >= sizeof(key)) { fprintf(stderr, "--set wants key=value, got '%s'\n", sets[i]); return 1; }
        memcpy(key, sets[i], (size_t)(eq - sets[i]));
        key[eq - sets[i]] = 0;
        idx = av_voice_param_find(key);
        if (idx < 0) { fprintf(stderr, "unknown parameter '%s' (see --list)\n", key); return 1; }
        *av_voice_param_ptr(&p, idx) = (float)atof(eq + 1);
    }
    av_voice_param_clamp(&p);

    if (!wav_read(inPath, &in, err, sizeof(err))) { fprintf(stderr, "%s: %s\n", inPath, err); return 1; }
    v = (av_voice *)calloc((size_t)in.channels, sizeof(av_voice));
    buf = (float *)malloc(sizeof(float) * (size_t)(in.frames + 1024));
    out = in;
    out.data = (float *)malloc(sizeof(float) * (size_t)in.frames * (size_t)in.channels + 1);
    if (!v || !buf || !out.data) { fprintf(stderr, "out of memory\n"); return 1; }

    printf("%s: %d ch, %d Hz, %.2f s, preset %s\n", inPath, in.channels, in.rate, (double)in.frames / in.rate, preset);
    for (c = 0; c < in.channels; c++) {
        av_voice_meters m;
        long f, total;

        av_voice_init(&v[c], (float)in.rate);
        av_voice_set(&v[c], &p);
        L = keep ? 0 : av_voice_latency(&v[c]);
        total = in.frames + L;                      /* run the tail out of the lookahead */
        for (f = 0; f < total; f++) buf[f] = f < in.frames ? in.data[f * in.channels + c] : 0.0f;
        for (f = 0; f < total; f += 512) av_voice_process(&v[c], buf + f, (int)((total - f) < 512 ? (total - f) : 512));
        for (f = 0; f < in.frames; f++) out.data[f * in.channels + c] = buf[f + L];
        av_voice_meters_take(&v[c], &m);
        printf("  ch %d: in peak %6.1f dBFS, out peak %6.1f dBFS | gate down to %6.1f dB | comp max %5.1f dB | de-ess max %5.1f dB | limiter max %5.1f dB\n",
               c + 1, m.in_peak, m.out_peak, m.gate_gain, m.comp_gr, m.deess_gr, m.lim_gr);
    }
    if (!wav_write(outPath, &out, pcm24)) { fprintf(stderr, "could not write %s\n", outPath); return 1; }
    printf("wrote %s (%s)\n", outPath, pcm24 ? "24-bit PCM" : "32-bit float");
    wav_free(&in);
    free(out.data);
    free(buf);
    free(v);
    return 0;
}
