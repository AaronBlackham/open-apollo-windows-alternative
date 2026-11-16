/*
 * dsp_status.h - [DSP] the DSPs' reply status words by name, for the log lines of a refused reply (strips.c, plugin.c).
 * A reply's word 1 is the DSP's status: 0 = done; the classes 0xF001xxxx and 0xF002xxxx are the DSP's errors, which UA's
 * kext turns into its _sendBlock result codes (oa:driver/ua_dsp.c ua_kext_block_status / ua_kext_err_name, parity note 13
 * section 4.2). The kext gives them no names, so neither does this: the text says which status and which kext result it
 * maps to, so a log line can be compared with open-apollo's and with a macOS trace. Plain C, no kernel headers.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

/* The kext's result for status word S (word 2 = Arg, which 0xF0010007 looks at); 0 = done. */
static __inline int ApolloDspKextResult(unsigned long S, unsigned long Arg)
{
    switch (S) {
    case 0:           return 0;
    case 0xF0010001u: return -49;
    case 0xF0010002u: return -48;
    case 0xF0010006u: return -68;
    case 0xF0010007u: return Arg == 1 ? -79 : -67;
    case 0xF0010011u: return -66;
    case 0xF0010012u: return -73;
    case 0xF0010013u: return -72;
    default:
        return ((S & 0xFFFF0000u) == 0xF0020000u) ? -50 : -51;    /* class 0xF002, or a bad response */
    }
}

/* Is S one of the DSP's status classes (0xF001xxxx, 0xF002xxxx)? */
static __inline int ApolloDspIsStatus(unsigned long S)
{
    return (S & 0xFFFF0000u) == 0xF0010000u || (S & 0xFFFF0000u) == 0xF0020000u;
}

/*
 * A short text for a 4-word reply: "" when no word is a DSP status, else e.g. "DSP status 0xF0010006, kext result -68"
 * (the first status-class word among words 1-3). Buf gets the text; it is returned.
 */
static __inline const char *ApolloDspReplyText(const unsigned long R[4], char *Buf, unsigned long BufLen)
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned long k, s = 0, n = 0;
    int r, i;
    char num[8];
    const char *p;

    if (Buf == 0 || BufLen < 48) {
        return "";
    }
    Buf[0] = '\0';
    for (k = 1; k < 4 && s == 0; k++) {
        if (ApolloDspIsStatus(R[k])) {
            s = R[k];
        }
    }
    if (s == 0) {
        return Buf;
    }
    r = ApolloDspKextResult(s, R[2]);
    for (p = "DSP status 0x"; *p; p++) Buf[n++] = *p;
    for (i = 7; i >= 0; i--) Buf[n++] = hex[(s >> (4 * i)) & 0xF];
    for (p = ", kext result -"; *p; p++) Buf[n++] = *p;
    r = -r;
    i = 0;
    do { num[i++] = (char)('0' + r % 10); r /= 10; } while (r > 0 && i < 7);
    while (i > 0) Buf[n++] = num[--i];
    Buf[n] = '\0';
    return Buf;
}
