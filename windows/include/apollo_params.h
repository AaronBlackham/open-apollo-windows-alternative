/*
 * apollo_params.h - [Params] the per-unit driver Parameters at run time: apollo.sys (driver\params.c), apollo-diag
 * (params, param) and Apollo Control (Status view). The values themselves and where they come from are
 * driver\apollo_unit.h and driver\unit.c (Units\<unit key> -> Models\<TT> -> legacy global (x8p) -> built-in default).
 *
 *   GET_PARAMS  out APOLLO_PARAMS_INFO: every Parameter of this handle's unit as this start runs it, where the value
 *               came from, its built-in default for the model, and what Units\<key> holds right now (a SET_PARAM since
 *               the start shows there before it applies). User level, lock-free: it reads what the start resolved.
 *   SET_PARAM   in APOLLO_PARAM_SET, out APOLLO_PARAMS_INFO: writes (or, Flags DELETE, removes) the value under this
 *               unit's Units\<key>. Administrators only (the same rule as editing the service key by hand). The unit
 *               runs the new value from its next start (disable/enable, a driver update, a reboot or a power cycle):
 *               the values a start resolved stay fixed while it runs, as before.
 *
 * Function codes 0x57-0x58 (0x53-0x56 are apollo_clock.h's).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include "apollo_ioctl.h"

#define IOCTL_APOLLO_GET_PARAMS         APOLLO_CTL(0x57, APOLLO_USER)
#define IOCTL_APOLLO_SET_PARAM          APOLLO_CTL(0x58, APOLLO_ADMIN)

#define APOLLO_PARAMS_VERSION           1
#define APOLLO_PARAMS_MAX               32      /* room; the driver has APOLLO_PARAM_COUNT (19 today) */
#define APOLLO_PARAM_NAME_LEN           24

/* APOLLO_PARAM_ENTRY.Source: as the driver log's letters D U M G X */
#define APOLLO_PARAM_SRC_DEFAULT        0
#define APOLLO_PARAM_SRC_UNIT           1
#define APOLLO_PARAM_SRC_MODEL          2
#define APOLLO_PARAM_SRC_GLOBAL         3
#define APOLLO_PARAM_SRC_MODE           4

#define APOLLO_PARAM_F_HEX              0x01    /* shown in hex (masks) */
#define APOLLO_PARAM_F_STORED           0x02    /* Units\<key> holds a value now (Stored) */
#define APOLLO_PARAM_F_PENDING          0x04    /* Stored differs from Value, or was removed: applies at the next start */

typedef struct _APOLLO_PARAM_ENTRY {
    CHAR    Name[APOLLO_PARAM_NAME_LEN];        /* the registry value name, NUL-terminated */
    ULONG   Value;              /* in force for this start */
    ULONG   Default;            /* the built-in default for this unit's model */
    ULONG   Stored;             /* Units\<key>\<Name> right now (F_STORED) */
    UCHAR   Source;             /* APOLLO_PARAM_SRC_* of Value */
    UCHAR   Flags;              /* APOLLO_PARAM_F_* */
    UCHAR   Reserved[2];
} APOLLO_PARAM_ENTRY, *PAPOLLO_PARAM_ENTRY;

typedef struct _APOLLO_PARAMS_INFO {
    ULONG   Version;            /* APOLLO_PARAMS_VERSION */
    ULONG   Count;              /* entries filled */
    ULONG   Resolved;           /* 0: the unit was not identified (no values; Entry[] holds the safe defaults) */
    ULONG   Result;             /* SET_PARAM: 0 done, else an NTSTATUS from the registry write */
    CHAR    UnitKey[20];        /* Units\<this>, "" if the unit has no key */
    APOLLO_PARAM_ENTRY Entry[APOLLO_PARAMS_MAX];
} APOLLO_PARAMS_INFO, *PAPOLLO_PARAMS_INFO;

#define APOLLO_PARAM_SET_DELETE         0x01    /* remove Units\<key>\<Name>: back to the model / default */

typedef struct _APOLLO_PARAM_SET {
    ULONG   Version;            /* APOLLO_PARAMS_VERSION */
    CHAR    Name[APOLLO_PARAM_NAME_LEN];        /* a name GET_PARAMS lists (case-insensitive) */
    ULONG   Value;
    ULONG   Flags;              /* APOLLO_PARAM_SET_* */
} APOLLO_PARAM_SET, *PAPOLLO_PARAM_SET;
