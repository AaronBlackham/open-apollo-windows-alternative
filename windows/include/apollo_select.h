/*
 * apollo_select.h - user mode: list the Apollos apollo.sys runs and pick one (docs/spec-twinx.md
 * 6.9, gate G2). Header-only, C and C++; include after apollo_ioctl.h, link cfgmgr32.lib.
 *
 * Every Apollo on apollo.sys has its own GUID_DEVINTERFACE_APOLLO interface, in no promised
 * order. To tell them apart a client opens each one read-only for a moment, asks GET_INFO and
 * closes it again (no handle is kept, so nothing here holds a unit PnP wants to remove):
 *   DeviceType 0x22  Apollo x8p             the unit every client means by default
 *   DeviceType 0x99  Apollo Twin X Gen 1    (or PCI subsystem 0x0019 if not identified as such)
 * A version 6 driver also reports Model, Mode and UnitFlags; APOLLO_UNIT_FLAG_OBSERVE means the
 * driver does not run that unit (read-only: no settings, transport, ASIO or audio I/O). A
 * version 5 driver (release7 and older) reports DeviceType only; it ran only an accepted x8p.
 *
 * Rules the clients follow:
 *   apollo-diag       --unit x8p|twinx|<index>; default the x8p, else the first interface
 *   Apollo Control    the x8p (settings); any other unit read-only in the Status view
 *   ASIO, asio-test,  the x8p only, never another unit
 *   wasapi-loop,
 *   loopback-test
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <windows.h>
#include <cfgmgr32.h>
#include <string.h>
#include <wchar.h>

#define APOLLO_SELECT_MAX           8       /* units listed at most */
#define APOLLO_SELECT_PATH          512     /* WCHARs of an interface path */

/* ApolloPickUnit: Want */
#define APOLLO_PICK_DEFAULT         (-1)    /* the x8p, else the first interface */
#define APOLLO_PICK_X8P             (-2)    /* the x8p only */
#define APOLLO_PICK_TWINX           (-3)    /* the Twin X only */

typedef struct _APOLLO_UNIT_ENTRY {
    WCHAR       Path[APOLLO_SELECT_PATH];   /* device interface path */
    DWORD       Error;          /* 0 = GET_INFO answered; else the Win32 error (ERROR_BUSY = skipped) */
    DWORD       InfoBytes;      /* bytes GET_INFO returned */
    APOLLO_INFO Info;           /* zero past InfoBytes */
} APOLLO_UNIT_ENTRY;

/* GET_INFO answered with the version 6 unit fields. */
static __inline BOOL ApolloInfoHasUnit(const APOLLO_INFO *I, DWORD Bytes)
{
    return Bytes >= (DWORD)sizeof(APOLLO_INFO) && I->Version >= 6;
}

/*
 * The driver runs this unit, so settings and audio may go to it. Version 6: UnitFlags ACTIVE.
 * Version 5: only an x8p was ever run (a unit without that type was refused and idle).
 */
static __inline BOOL ApolloInfoActive(const APOLLO_INFO *I, DWORD Bytes)
{
    if (Bytes < APOLLO_INFO_V5_BYTES) {
        return FALSE;
    }
    if (ApolloInfoHasUnit(I, Bytes)) {
        return (I->UnitFlags & APOLLO_UNIT_FLAG_ACTIVE) != 0;
    }
    return I->DeviceType == APOLLO_TYPE_X8P_ID;
}

static __inline BOOL ApolloInfoIsX8p(const APOLLO_INFO *I, DWORD Bytes)
{
    return Bytes >= APOLLO_INFO_V5_BYTES && I->DeviceType == APOLLO_TYPE_X8P_ID;
}

static __inline BOOL ApolloInfoIsTwinX(const APOLLO_INFO *I, DWORD Bytes)
{
    return Bytes >= APOLLO_INFO_V5_BYTES &&
           (I->DeviceType == APOLLO_TYPE_TWINX_ID ||
            (I->DeviceType != APOLLO_TYPE_X8P_ID && I->SubsystemId == 0x0019));
}

/* GET_INFO on an open handle; any size from version 5 on. Returns 0 or the Win32 error. */
static __inline DWORD ApolloQueryInfo(HANDLE H, APOLLO_INFO *Out, DWORD *Bytes)
{
    DWORD r = 0;

    memset(Out, 0, sizeof(*Out));
    *Bytes = 0;
    if (!DeviceIoControl(H, IOCTL_APOLLO_GET_INFO, NULL, 0, Out, (DWORD)sizeof(*Out), &r, NULL)) {
        DWORD e = GetLastError();
        return e ? e : ERROR_GEN_FAILURE;
    }
    if (r < APOLLO_INFO_V5_BYTES) {
        return ERROR_INVALID_DATA;
    }
    *Bytes = r;
    return 0;
}

/*
 * Lists the present Apollo interfaces into Out[0..Max-1] (interface order) and asks each one
 * GET_INFO through a short-lived read-only handle. Skip/SkipCount: interface paths not to open
 * (a unit the caller holds off while PnP removes it); they are listed with Error ERROR_BUSY.
 * Returns the number listed (0 = none), or -1 if the interface list could not be read.
 */
static __inline int ApolloListUnits(APOLLO_UNIT_ENTRY *Out, int Max, const WCHAR *const *Skip, int SkipCount)
{
    ULONG len = 0;
    WCHAR *list = NULL, *p;
    CONFIGRET cr;
    int n = 0, k, tries;

    for (tries = 0; tries < 4; tries++) {
        cr = CM_Get_Device_Interface_List_SizeW(&len, (LPGUID)&GUID_DEVINTERFACE_APOLLO, NULL,
                                                CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr != CR_SUCCESS) {
            return -1;
        }
        if (len <= 1) {
            return 0;
        }
        list = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)len * sizeof(WCHAR));
        if (list == NULL) {
            return -1;
        }
        cr = CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_APOLLO, NULL, list, len,
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr == CR_SUCCESS) {
            break;
        }
        HeapFree(GetProcessHeap(), 0, list);
        list = NULL;
        if (cr != CR_BUFFER_SMALL) {
            return -1;
        }
    }
    if (list == NULL) {
        return -1;
    }
    for (p = list; *p != 0 && n < Max; p += wcslen(p) + 1) {
        APOLLO_UNIT_ENTRY *e = &Out[n];
        BOOL skip = FALSE;
        HANDLE h;

        memset(e, 0, sizeof(*e));
        if (wcslen(p) >= APOLLO_SELECT_PATH) {
            continue;                   /* cannot happen for a PCI device: never truncate a path */
        }
        wcscpy_s(e->Path, APOLLO_SELECT_PATH, p);
        n++;
        for (k = 0; k < SkipCount && Skip != NULL; k++) {
            if (Skip[k] != NULL && Skip[k][0] != 0 && _wcsicmp(Skip[k], p) == 0) {
                skip = TRUE;
            }
        }
        if (skip) {
            e->Error = ERROR_BUSY;
            continue;
        }
        h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            e->Error = GetLastError();
            if (e->Error == 0) {
                e->Error = ERROR_GEN_FAILURE;
            }
            continue;
        }
        e->Error = ApolloQueryInfo(h, &e->Info, &e->InfoBytes);
        CloseHandle(h);
    }
    HeapFree(GetProcessHeap(), 0, list);
    return n;
}

/* Which entry (APOLLO_PICK_* or an interface index >= 0); -1 if none fits. */
static __inline int ApolloPickUnit(const APOLLO_UNIT_ENTRY *E, int N, int Want)
{
    int i;

    if (Want >= 0) {
        return Want < N ? Want : -1;
    }
    for (i = 0; i < N; i++) {
        if (E[i].Error != 0) {
            continue;
        }
        if ((Want == APOLLO_PICK_DEFAULT || Want == APOLLO_PICK_X8P) && ApolloInfoIsX8p(&E[i].Info, E[i].InfoBytes)) {
            return i;
        }
        if (Want == APOLLO_PICK_TWINX && ApolloInfoIsTwinX(&E[i].Info, E[i].InfoBytes)) {
            return i;
        }
    }
    return (Want == APOLLO_PICK_DEFAULT && N > 0) ? 0 : -1;
}

/* Short model text for messages: the driver's ModelName (version 6), else by DeviceType. */
static __inline const char *ApolloInfoModelText(const APOLLO_INFO *I, DWORD Bytes)
{
    if (ApolloInfoHasUnit(I, Bytes) && I->ModelName[0] != 0 && I->ModelName[sizeof(I->ModelName) - 1] == 0) {
        return I->ModelName;
    }
    if (Bytes >= APOLLO_INFO_V5_BYTES && I->DeviceType == APOLLO_TYPE_X8P_ID) {
        return "Apollo x8p";
    }
    if (ApolloInfoIsTwinX(I, Bytes)) {
        return "Apollo Twin X";
    }
    return "Apollo";
}
