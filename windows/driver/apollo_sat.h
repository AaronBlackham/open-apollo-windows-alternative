/*
 * apollo_sat.h - saturating 32-bit add for the playback ring (the engine's
 * clients ADD into it: ASIO, ACX render streams). Plain C, no kernel headers,
 * so tools\sat-test checks the same code.
 *
 * Not MINLONG: ntdef.h defines it as 0x80000000, an unsigned int, so a
 * LONGLONG compared with it is compared with +2^31 and "s < MINLONG" holds for
 * every sample. The first engine builds did exactly that and wrote 0x80000000
 * into every frame a client touched (found on the x8p Nov 14: the MON loopback
 * sat at -FS whatever ASIO played; /W4 and Code Analysis stay silent).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#define APOLLO_S32_MAX      2147483647LL
#define APOLLO_S32_MIN      (-2147483647LL - 1)

static __forceinline long ApolloSatAdd32(long A, long B)
{
    long long s = (long long)A + (long long)B;

    if (s > APOLLO_S32_MAX) {
        return (long)APOLLO_S32_MAX;
    }
    if (s < APOLLO_S32_MIN) {
        return (long)APOLLO_S32_MIN;
    }
    return (long)s;
}
