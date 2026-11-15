/*
 * log.c - driver log: DbgPrintEx for a kernel debugger, plus a 256 KiB ring
 * in memory so apollo-diag can show it without one. Callable at
 * IRQL <= DISPATCH_LEVEL (not from the ISR).
 *
 * [Twin X G2] One ring for every device on the driver (two units interleave, which keeps a
 * combined start-up readable). A line about one unit carries its tag right after the time:
 * "[  12.345] [twx] text" (ApolloLogU, apollo.h); driver-wide lines have none.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"
#include <ntstrsafe.h>
#include <stdarg.h>

#define LOG_LINE_MAX    256
C_ASSERT((APOLLO_LOG_BYTES & (APOLLO_LOG_BYTES - 1)) == 0);     /* offsets survive the ULONG wrap */
C_ASSERT(LOG_LINE_MAX < APOLLO_LOG_BYTES);                     /* a line wraps the ring at most once */

static CHAR       g_Log[APOLLO_LOG_BYTES];
static ULONG      g_LogTotal;
static KSPIN_LOCK g_LogLock;

VOID ApolloLogInit(VOID)
{
    KeInitializeSpinLock(&g_LogLock);
    g_LogTotal = 0;
}

VOID ApolloLogPrint(_In_opt_z_ PCSTR Tag, _In_z_ _Printf_format_string_ PCSTR Fmt, ...)
{
    CHAR line[LOG_LINE_MAX];
    size_t len = 0, off, first;
    va_list ap;
    KIRQL irql;
    ULONG ms = (ULONG)(KeQueryInterruptTime() / 10000);

    if (Tag != NULL && Tag[0] != '\0') {
        (VOID)RtlStringCbPrintfA(line, sizeof(line), "[%5lu.%03lu] [%.7s] ", ms / 1000, ms % 1000, Tag);
    } else {
        (VOID)RtlStringCbPrintfA(line, sizeof(line), "[%5lu.%03lu] ", ms / 1000, ms % 1000);
    }
    (VOID)RtlStringCbLengthA(line, sizeof(line), &len);

    va_start(ap, Fmt);
    /* Leave room for the newline; a truncated line is still terminated. */
    (VOID)RtlStringCbVPrintfA(line + len, sizeof(line) - len - 1, Fmt, ap);
    va_end(ap);
    (VOID)RtlStringCbLengthA(line, sizeof(line), &len);
    if (len > sizeof(line) - 2) {
        len = sizeof(line) - 2;
    }
    line[len++] = '\n';
    line[len] = '\0';

    DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "apollo: %s", line);

    /* [G2 review] Two block copies at most (the ring wraps once), not a byte loop under the lock. */
    KeAcquireSpinLock(&g_LogLock, &irql);
    off = g_LogTotal % APOLLO_LOG_BYTES;
    first = min(len, (size_t)(APOLLO_LOG_BYTES - off));
    RtlCopyMemory(g_Log + off, line, first);
    if (len > first) {
        RtlCopyMemory(g_Log, line + first, len - first);
    }
    g_LogTotal += (ULONG)len;
    KeReleaseSpinLock(&g_LogLock, irql);
}

/*
 * Copy the most recent text, oldest first. Returns bytes copied. [G2 review] Two RtlCopyMemory
 * instead of a byte loop: up to 256 KiB under a spin lock at DISPATCH_LEVEL was ~1 ms of byte
 * copies with modulo; the block copies take a few tens of microseconds. APOLLO_LOG_BYTES is a
 * power of two, so the offsets stay right when the 32-bit total wraps.
 */
ULONG ApolloLogSnapshot(_Out_writes_bytes_(OutLen) PCHAR Out, _In_ ULONG OutLen,
                        _Out_ PULONG TotalWritten)
{
    KIRQL irql;
    ULONG avail, n, start, first;

    KeAcquireSpinLock(&g_LogLock, &irql);
    avail = min(g_LogTotal, (ULONG)APOLLO_LOG_BYTES);
    n = min(avail, OutLen);
    start = (g_LogTotal - n) % APOLLO_LOG_BYTES;
    first = min(n, (ULONG)APOLLO_LOG_BYTES - start);
    RtlCopyMemory(Out, g_Log + start, first);
    if (n > first) {
        RtlCopyMemory(Out + first, g_Log, n - first);
    }
    *TotalWritten = g_LogTotal;
    KeReleaseSpinLock(&g_LogLock, irql);
    return n;
}
