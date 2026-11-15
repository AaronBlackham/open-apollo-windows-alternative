/*
 * sat-test.c - checks driver\apollo_sat.h, the saturating add every playback
 * ring client uses (ASIO, ACX render). The first engine builds compared with
 * MINLONG (0x80000000, an unsigned int in ntdef.h) and returned -FS for every
 * sample; this test fails on that. Exit code 0 = all passed.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include "..\..\driver\apollo_sat.h"

#define S32MIN  (-2147483647L - 1)
#define S32MAX  2147483647L

int main(void)
{
    static const struct { long a, b, want; } t[] = {
        { 0, 0, 0 },
        { 0, 0x10000000L, 0x10000000L },                 /* +0.125 FS into silence */
        { 0x10000000L, -0x20000000L, -0x10000000L },
        { -5, 3, -2 },
        { 0x40000000L, 0x40000000L, S32MAX },            /* +0.5 + +0.5 saturates */
        { S32MAX, 1, S32MAX },
        { S32MAX, 0, S32MAX },
        { -0x40000000L, -0x40000001L, S32MIN },          /* -0.5 + -0.5 saturates */
        { S32MIN, -1, S32MIN },
        { S32MIN, 0, S32MIN },
        { S32MIN, S32MAX, -1 },
        { 214748364L, -214748364L, 0 },                  /* -20 dBFS tone peaks cancel */
    };
    int fail = 0;

    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        long got = ApolloSatAdd32(t[i].a, t[i].b);

        if (got != t[i].want) {
            printf("FAIL %08lx + %08lx = %08lx, want %08lx\n", (unsigned long)t[i].a, (unsigned long)t[i].b,
                   (unsigned long)got, (unsigned long)t[i].want);
            fail++;
        }
    }
    printf("sat-test: %u checks, %d failed\n", (unsigned)(sizeof(t) / sizeof(t[0])), fail);
    return fail ? 1 : 0;
}
