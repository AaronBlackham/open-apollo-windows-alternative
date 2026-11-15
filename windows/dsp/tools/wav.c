/*
 * wav.c - minimal WAV reader/writer (see wav.h).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _CRT_SECURE_NO_WARNINGS
#include "wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void seterr(char *err, size_t n, const char *msg)
{
    if (err && n) {
        strncpy(err, msg, n - 1);
        err[n - 1] = 0;
    }
}

int wav_read(const char *path, wav_audio *out, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL;
    long size;
    size_t pos;
    int fmt = 0, ch = 0, rate = 0, bits = 0, haveFmt = 0;
    const unsigned char *data = NULL;
    uint32_t dataLen = 0;
    long frames, i;
    int bytes;

    memset(out, 0, sizeof(*out));
    if (!f) { seterr(err, errlen, "cannot open the file"); return 0; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 44 || size > 0x7FFFFFFF) { fclose(f); seterr(err, errlen, "not a WAV file (size)"); return 0; }
    buf = (unsigned char *)malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f); free(buf); seterr(err, errlen, "read failed"); return 0;
    }
    fclose(f);
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        free(buf); seterr(err, errlen, "not a RIFF/WAVE file"); return 0;
    }

    pos = 12;
    while (pos + 8 <= (size_t)size) {
        uint32_t len = rd32(buf + pos + 4);
        const unsigned char *body = buf + pos + 8;

        if (len > (size_t)size - pos - 8) len = (uint32_t)((size_t)size - pos - 8);
        if (memcmp(buf + pos, "fmt ", 4) == 0 && len >= 16) {
            fmt = rd16(body);
            ch = rd16(body + 2);
            rate = (int)rd32(body + 4);
            bits = rd16(body + 14);
            if (fmt == 0xFFFE && len >= 40) fmt = rd16(body + 24);    /* SubFormat GUID's first word */
            haveFmt = 1;
        } else if (memcmp(buf + pos, "data", 4) == 0) {
            data = body;
            dataLen = len;
        }
        pos += 8 + len + (len & 1);
    }
    if (!haveFmt || !data) { free(buf); seterr(err, errlen, "missing fmt or data chunk"); return 0; }
    if (ch < 1 || ch > 64 || rate < 8000 || rate > 768000) { free(buf); seterr(err, errlen, "unsupported channel count or rate"); return 0; }
    if (!((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && (bits == 32 || bits == 64)))) {
        free(buf); seterr(err, errlen, "unsupported sample format (PCM 16/24/32 or float 32/64 only)"); return 0;
    }
    bytes = bits / 8;
    frames = (long)(dataLen / (uint32_t)(bytes * ch));
    out->data = (float *)malloc(sizeof(float) * (size_t)frames * (size_t)ch + 1);
    if (!out->data) { free(buf); seterr(err, errlen, "out of memory"); return 0; }
    for (i = 0; i < frames * ch; i++) {
        const unsigned char *s = data + (size_t)i * (size_t)bytes;
        float v;

        if (fmt == 3 && bits == 32) { float t; memcpy(&t, s, 4); v = t; }
        else if (fmt == 3) { double t; memcpy(&t, s, 8); v = (float)t; }
        else if (bits == 16) v = (float)(int16_t)rd16(s) / 32768.0f;
        else if (bits == 24) v = (float)((int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) >> 8) / 8388608.0f;
        else v = (float)((double)(int32_t)rd32(s) / 2147483648.0);
        out->data[i] = v;
    }
    out->channels = ch;
    out->rate = rate;
    out->frames = frames;
    free(buf);
    return 1;
}

static void wr32(FILE *f, uint32_t v) { unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) }; fwrite(b, 1, 4, f); }
static void wr16(FILE *f, uint16_t v) { unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) }; fwrite(b, 1, 2, f); }

int wav_write(const char *path, const wav_audio *a, int pcm24)
{
    FILE *f = fopen(path, "wb");
    int bytes = pcm24 ? 3 : 4;
    uint32_t dataLen = (uint32_t)((size_t)a->frames * (size_t)a->channels * (size_t)bytes);
    long i;

    if (!f) return 0;
    fwrite("RIFF", 1, 4, f);
    wr32(f, 36 + dataLen + (dataLen & 1));
    fwrite("WAVEfmt ", 1, 8, f);
    wr32(f, 16);
    wr16(f, (uint16_t)(pcm24 ? 1 : 3));
    wr16(f, (uint16_t)a->channels);
    wr32(f, (uint32_t)a->rate);
    wr32(f, (uint32_t)(a->rate * a->channels * bytes));
    wr16(f, (uint16_t)(a->channels * bytes));
    wr16(f, (uint16_t)(bytes * 8));
    fwrite("data", 1, 4, f);
    wr32(f, dataLen);
    for (i = 0; i < a->frames * a->channels; i++) {
        float v = a->data[i];

        if (pcm24) {
            double d = (double)v * 8388608.0;
            int32_t s;
            unsigned char b[3];

            if (d > 8388607.0) d = 8388607.0;
            if (d < -8388608.0) d = -8388608.0;
            s = (int32_t)(d < 0 ? d - 0.5 : d + 0.5);
            b[0] = (unsigned char)s; b[1] = (unsigned char)(s >> 8); b[2] = (unsigned char)(s >> 16);
            fwrite(b, 1, 3, f);
        } else {
            fwrite(&v, 4, 1, f);
        }
    }
    if (dataLen & 1) fputc(0, f);
    return fclose(f) == 0;
}

void wav_free(wav_audio *a)
{
    free(a->data);
    a->data = NULL;
}
