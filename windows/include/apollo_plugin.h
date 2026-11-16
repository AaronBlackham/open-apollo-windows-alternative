/*
 * apollo_plugin.h - UAD plug-in hosting on the Apollo DSPs, phase P0 (docs/PLUGINS.md 2.1):
 * the IOCTL block 0x40-0x43, interface version 8, plus staged startup (FEAT_STAGE: CREATE F_STAGE,
 * op START), stereo inserts on a linked pair (FEAT_STEREO) and the caller rule (FEAT_CALLER); [Warm]
 * plug-ins after a warm adoption (FEAT_WARM) and [Lat] op LATENCY (FEAT_LATENCY); all
 * additive, every layout unchanged. Included by apollo_ioctl.h; shared by
 * apollo.sys (driver\plugin.c), tools\plugin-host (apollo-plug) and the Python bench.
 *
 * Nothing raw reaches the device through these IOCTLs: no DSP address, command word or code
 * byte comes from user space (as open-apollo's, oa:driver/ua_ioctl.h:986-1024). Modules are
 * named by SHA-256 only; apollo.sys reads them itself from the admin-only store
 * %SystemRoot%\System32\drivers\ApolloModules\<sha256>.bill and re-hashes them. Every row and
 * WRITE is checked against the CREATE layout and relocated by the driver.
 *
 * Licences: the driver places nothing the Apollo itself does not authorize. Every CREATE reads
 * the device's board table fresh, in the same operation as the placement (no cache, no
 * override, no demo start, never 0x000D0000 / 0x000E0000). BOARD_QUERY hands the table to the
 * caller as the device sent it; the driver logs counts only, never the table.
 *
 * Gate: the registry value Services\apollo\Parameters\PluginHost (DWORD, default 0), read at
 * every call, and the unit: an x8p or (P7, FEAT_UNITS) a Twin X Gen 1, in Mode 2. With PluginHost 0 every
 * call but GET_PLUGIN_CAPS fails with STATUS_NOT_SUPPORTED and nothing is sent.
 *
 * Caller (FEAT_CALLER): all four codes are APOLLO_USER (FILE_READ_ACCESS) like
 * upstream's, since the driver builds every word, but that does NOT open BOARD_QUERY and
 * PLUGIN_INSTANCE to every local process. With hosting on, the driver takes them only from
 *   (a) a handle opened by an elevated administrator (the opener's token, as for the admin
 *       IOCTLs: apollo-plug and step-p1.ps1 run elevated), or
 *   (b) a handle used by the process that opened it (a handle inherited by or duplicated into
 *       another process does not count) when BOTH
 *       - that process's primary token belongs to the user that Services\apollo\Parameters
 *         value PluginHostUser names (APOLLO_PLUGIN_REG_USER, REG_SZ: a SID string as
 *         ConvertSidToStringSidW writes it, at most APOLLO_PLUGIN_USER_CCH_MAX characters), and
 *       - its image (SeLocateProcessImageName, the image section's NT path) is the file
 *         PluginHostImage names (APOLLO_PLUGIN_REG_IMAGE, REG_SZ): a DOS path "X:\...", whose
 *         drive letter the driver resolves at every call through \GLOBAL??\X: (the global
 *         namespace only), compared with ASCII letters case-insensitive. At most
 *         APOLLO_PLUGIN_IMAGE_CCH_MAX characters. A device path, UNC or \\?\ form is refused.
 *       Both values are read at every call like PluginHost; either absent, malformed or
 *       unreadable = no unelevated caller, only (a).
 * Anyone else: the IOCTL fails with STATUS_ACCESS_DENIED before anything is locked or sent (the
 * driver logs counts only, at most one line per 10 s). An administrator sets the values (Apollo
 * Control's elevated --plugin-admin step writes its own path and the user it runs for). What (b)
 * proves: this program, started by this user. Another account cannot pass by starting the same
 * executable (its token's user differs). What it cannot prove: that the program is unmodified in
 * memory. A process of the SAME user can still start it suspended or write into it (the user's
 * own session is the trust boundary); keep the program where only administrators can write
 * (Program Files). A new drive letter for its volume, or a moved program, needs the step again.
 * GET_PLUGIN_CAPS stays open to every handle and says in Enabled what this handle, from this
 * process, may do.
 *
 * Completion: the IOCTL itself fails only for a malformed request (sizes, flags, a buffer too
 * small), a disabled host (STATUS_NOT_SUPPORTED), a caller that is not the host
 * (STATUS_ACCESS_DENIED) or a busy host (STATUS_DEVICE_BUSY, nothing done). Otherwise it
 * succeeds and the operation's own outcome is in the output's Status (an NTSTATUS), with Step
 * saying where it ended and Detail what the device answered.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#define APOLLO_PLUGIN_ABI                   1

#define IOCTL_APOLLO_GET_PLUGIN_CAPS        APOLLO_CTL(0x40, APOLLO_USER)
#define IOCTL_APOLLO_BOARD_QUERY            APOLLO_CTL(0x41, APOLLO_USER)
#define IOCTL_APOLLO_PLUGIN_INSTANCE        APOLLO_CTL(0x42, APOLLO_USER)
#define IOCTL_APOLLO_PLUGIN_DELAY           APOLLO_CTL(0x43, APOLLO_USER)   /* reserved for P4: NOT_SUPPORTED */

#define APOLLO_PLUGIN_DSPS                  8
#define APOLLO_PLUGIN_INSTANCES_MAX         16
#define APOLLO_PLUGIN_RES_MAX               64      /* module hashes per CREATE (ua_ioctl.h:1109) */
#define APOLLO_PLUGIN_PRIV_MAX              127     /* private rows per CREATE (127 zero-fills + the memspec
                                                       fill one 128-entry transaction; upstream says 128) */
#define APOLLO_PLUGIN_CMD_WORDS_MAX         1016    /* WRITE words: one 4 KiB DMA page (ua_ioctl.h:1159) */
#define APOLLO_PLUGIN_READ_WORDS_MAX        64      /* SERVICE read */
#define APOLLO_PLUGIN_DSP_ANY               0xFFFFFFFFu
#define APOLLO_PLUGIN_SYNCH_MONO            0x20u   /* in2 of a mono START (a pair: in2 = in1 + 1, FEAT_STEREO) */

/* ---------------------------------------------------------------- GET_PLUGIN_CAPS (read only) */

/*
 * APOLLO_PLUGIN_CAPS.Enabled (bit 0 alone was version 8's first meaning; still "hosting on"):
 * HOST without CALLER = hosting is on but this handle, from this process, may not make the
 * plug-in calls (header comment, Caller): Operations is then 0 and Why says so.
 */
#define APOLLO_PLUGIN_ENABLED_HOST          0x01u   /* PluginHost != 0 and the unit allows hosting */
#define APOLLO_PLUGIN_ENABLED_CALLER        0x02u   /* and BOARD_QUERY / PLUGIN_INSTANCE are taken from this caller now */
#define APOLLO_PLUGIN_ENABLED_AUX           0x04u   /* [Ins] Parameters\AuxInserts != 0: chains on the aux returns taken */

/* APOLLO_PLUGIN_CAPS.Operations: accepted right now (from this caller) */
#define APOLLO_PLUGIN_OP_BOARD_QUERY        0x01u   /* DSP 0 rings live on a connected unit (warm is fine) */
#define APOLLO_PLUGIN_OP_CREATE             0x02u   /* + a DSP this driver may place on (cold-started, or [Warm] the
                                                       strip DSP of strips this start adopted or rebuilt, FEAT_WARM) */
#define APOLLO_PLUGIN_OP_DESTROY            0x04u   /* always while enabled (HOST | CALLER) */
#define APOLLO_PLUGIN_OP_SERVICE            0x08u   /* with CREATE */

/* APOLLO_PLUGIN_CAPS.Features (this build) */
#define APOLLO_PLUGIN_FEAT_CHECK            0x01u   /* CREATE F_CHECK */
#define APOLLO_PLUGIN_FEAT_PLAIN            0x02u   /* CREATE without START (unrouted) */
#define APOLLO_PLUGIN_FEAT_START_CHAIN      0x04u   /* CREATE F_START | F_CHAIN, mono, x8p inputs 1-8, Twin X 1-2 (a pair: FEAT_STEREO) */
#define APOLLO_PLUGIN_FEAT_SERVICE          0x08u   /* SERVICE writes + one read */
#define APOLLO_PLUGIN_FEAT_STAGE            0x10u   /* CREATE F_STAGE | F_CHAIN, SERVICE before routing, op START:
                                                       a startup over one 1016-word page (upstream's
                                                       UA_FEAT2_PLUGIN_START_STAGED, ua_ioctl.h:591-592) */
#define APOLLO_PLUGIN_FEAT_STEREO           0x20u   /* CREATE F_START / F_STAGE on a linked pair: Synch =
                                                       in1 | in2 << 16, in1 even, in2 = in1 + 1 (x8p 1/2, 3/4,
                                                       5/6, 7/8): both plain strips stopped, restored together */
#define APOLLO_PLUGIN_FEAT_CALLER           0x40u   /* BOARD_QUERY / PLUGIN_INSTANCE only from an administrator's
                                                       handle, or the PluginHostImage program run by PluginHostUser (header comment) */
#define APOLLO_PLUGIN_FEAT_UNITS            0x80u   /* (P7) hosting on the Twin X Gen 1 too, each unit through its own
                                                       handle: its strip DSP (DSP 1), a ledger with its own strips' room
                                                       (Twin X 2), its own licence answer (board query on its DSP 0),
                                                       its inputs (Twin X 1-2, the pair 1/2; the talkback refused).
                                                       Without it, only an x8p hosts (an older build) */
#define APOLLO_PLUGIN_FEAT_CHAIN4           0x100u  /* [Ins] chains of up to 4 effects (N = 2..5) composed by the host: the
                                                       driver checked them since P0; this build was reviewed for them */
#define APOLLO_PLUGIN_FEAT_UNISON           0x200u  /* [Ins] CREATE F_UNISON, ops BEGIN_UNISON / ACTIVATE_UNISON / UNISON_PARAMS
                                                       and the analog lease (driver\unison_plan.h); never run on hardware */
#define APOLLO_PLUGIN_FEAT_AUX              0x400u  /* [Ins] chains on the aux returns (Synch APOLLO_PLUGIN_AUX_SYNCH(a), Input Strip
                                                       carrier 0x83E), only while Enabled has APOLLO_PLUGIN_ENABLED_AUX */
#define APOLLO_PLUGIN_FEAT_WARM             0x800u  /* [Warm] (docs/STRIPS.md 12.5) the strip DSP takes plug-ins after a
                                                       warm start whose strips were adopted (x8p) or rebuilt (Twin X) from an anchored
                                                       record with no plug-in left there (StripWarmPlugins, default 1); Dsp[].Flags
                                                       carry DSP_WARM / DSP_WARM_PROVEN and, while a DSP is not owned, the reason
                                                       (APOLLO_PLUGIN_DSP_WHY). Without it a warm start never owns a DSP for a host
                                                       that waits for this bit */
#define APOLLO_PLUGIN_FEAT_LATENCY          0x1000u /* [Lat] op LATENCY: a routed chain's record-path latency, so the unit's ASIO and
                                                       Windows latencies include an over-budget chain (docs/ASIO.md, Latency) */

/* APOLLO_PLUGIN_DSP_INFO.Flags */
#define APOLLO_PLUGIN_DSP_OWNED             0x01u   /* boot word 3 at this cold start, nothing raw since; [Warm] or DSP_WARM */
#define APOLLO_PLUGIN_DSP_QUARANTINED       0x02u   /* an outcome could not be proven: power-cycle */
#define APOLLO_PLUGIN_DSP_LEDGER            0x04u   /* pool registers read, ledger valid this epoch */
#define APOLLO_PLUGIN_DSP_STRIPS            0x08u   /* the strip DSP (its strips are pre-booked) */
#define APOLLO_PLUGIN_DSP_ADMITTED          0x10u   /* placement allowed now */
#define APOLLO_PLUGIN_DSP_RINGS             0x20u   /* command and response rings live */
#define APOLLO_PLUGIN_DSP_WARM              0x40u   /* [Warm] owned at a warm start: the strips this start adopted (no ring connect: the
                                                       x8p, session byte 0x07) or rebuilt, proven by the anchored record */
#define APOLLO_PLUGIN_DSP_WARM_PROVEN       0x80u   /* [Warm] and it acknowledged an upload in this warm session (a plug-in module on the
                                                       x8p; the strip rebuild's own uploads on the Twin X) */
/*
 * [Warm] Bits 15:8 of Flags while OWNED is clear (FEAT_WARM): why this DSP is not this host's, for the waiting reason.
 * Every one of them but PENDING is ended by a power cycle of the unit (a cold start owns its DSPs again).
 */
#define APOLLO_PLUGIN_DSP_WHY_SHIFT         8
#define APOLLO_PLUGIN_DSP_WHY_MASK          0xFF00u
#define APOLLO_PLUGIN_DSP_WHY(f)            (((f) & APOLLO_PLUGIN_DSP_WHY_MASK) >> APOLLO_PLUGIN_DSP_WHY_SHIFT)
#define APOLLO_PLUGIN_WHY_NONE              0u      /* owned, or not the strip DSP of a warm start (nothing to say) */
#define APOLLO_PLUGIN_WHY_NOT_READ          1u      /* the boot words were not read yet (bring-up) */
#define APOLLO_PLUGIN_WHY_HOST_USED         2u      /* cold start, but a host (UA's driver) used the DSP since power-up */
#define APOLLO_PLUGIN_WHY_TAINTED           3u      /* a raw register write since this start */
#define APOLLO_PLUGIN_WHY_WARM_PENDING      4u      /* warm start: the strips' check (500 ms after the bring-up) has not run yet */
#define APOLLO_PLUGIN_WHY_WARM_NOT_ADOPTED  5u      /* warm start: this driver's strips were not adopted or rebuilt (GET_STRIPS Why) */
#define APOLLO_PLUGIN_WHY_WARM_PLUGIN_LEFT  6u      /* warm start: a plug-in was on the strip DSP when the driver last stopped */
#define APOLLO_PLUGIN_WHY_WARM_OFF          7u      /* warm start: StripWarmPlugins = 0 for this unit */
#define APOLLO_PLUGIN_WHY_WARM_UNANSWERED   8u      /* warm start: the strip DSP did not acknowledge the first upload (quarantined) */
#define APOLLO_PLUGIN_WHY_STRIPS            9u      /* the strip DSP's strips are not this driver's, or quarantined */

typedef struct _APOLLO_PLUGIN_POOL_USAGE {
    ULONG   Base;               /* rounded base */
    ULONG   Total;              /* rounded capacity, scratch included */
    ULONG   Scratch;            /* current scratch (pools 1-3) */
    ULONG   Used;
    ULONG   Free;
    ULONG   Largest;            /* largest request that fits now */
    ULONG   Allocations;
    ULONG   Holds;
} APOLLO_PLUGIN_POOL_USAGE;

typedef struct _APOLLO_PLUGIN_DSP_INFO {
    ULONG   Flags;              /* APOLLO_PLUGIN_DSP_* */
    ULONG   BootWord;           /* window +0x1A4 at the last bring-up */
    ULONG   Load;               /* window +0x0C, read for this call */
    ULONG   Regs[11];           /* pool registers: size 0-3, base 0-3, scratch 1-3 (when read) */
    APOLLO_PLUGIN_POOL_USAGE Pool[4];
    ULONG   Xacts;              /* transactions this host published there */
    ULONG   Uploads;
    ULONG   RepliesOk;
    ULONG   RepliesBad;
    ULONG   ZeroFills;
    ULONG   Memspecs;
    ULONG   Writes;
    ULONG   Stops;
    ULONG   Releases;
    ULONG   Timeouts;
} APOLLO_PLUGIN_DSP_INFO;

/* APOLLO_PLUGIN_INSTANCE_INFO.Flags */
#define APOLLO_PLUGIN_INST_PLACED           0x01u
#define APOLLO_PLUGIN_INST_LOADED           0x02u   /* everything taken by the DSP */
#define APOLLO_PLUGIN_INST_STARTED          0x04u   /* SYNCH'd on its DSP input(s) */
#define APOLLO_PLUGIN_INST_LOST             0x08u   /* taken down by the host: DESTROY just forgets it */
#define APOLLO_PLUGIN_INST_OVERRIDE         0x10u   /* replaces a retained plain strip */
#define APOLLO_PLUGIN_INST_MINE             0x20u   /* owned by the handle that asked */
#define APOLLO_PLUGIN_INST_STAGED           0x40u   /* created F_STAGE, not routed yet: its plain strip still runs */
#define APOLLO_PLUGIN_INST_UNISON           0x80u   /* [Ins] F_UNISON chain */
#define APOLLO_PLUGIN_INST_ANALOG           0x100u  /* [Ins] holds the analog lease of its input(s) */
#define APOLLO_PLUGIN_INST_AUX              0x200u  /* [Ins] runs on an aux return pair */

typedef struct _APOLLO_PLUGIN_INSTANCE_INFO {
    ULONG   Handle;
    ULONG   RenderId;
    ULONG   Dsp;
    ULONG   Synch;              /* in1 | in2 << 16 */
    ULONG   Flags;              /* APOLLO_PLUGIN_INST_* */
    ULONG   Epoch;
    ULONG   Modules;            /* module leases (strip modules shared with the strips not counted) */
    ULONG   Rows;
    ULONG   HeadAddress;        /* DSP address of private row 0 (diagnostics) */
    ULONG   Reserved;
} APOLLO_PLUGIN_INSTANCE_INFO;

typedef struct _APOLLO_PLUGIN_CAPS {
    ULONG   Size;               /* out: sizeof(APOLLO_PLUGIN_CAPS) */
    ULONG   Abi;                /* APOLLO_PLUGIN_ABI */
    ULONG   Enabled;            /* APOLLO_PLUGIN_ENABLED_*: bit 0 hosting on, bit 1 this caller may use it */
    ULONG   Operations;         /* APOLLO_PLUGIN_OP_* */
    ULONG   Features;           /* APOLLO_PLUGIN_FEAT_* */
    ULONG   Epoch;              /* host epoch (= the strips' epoch: every bring-up, every taint) */
    ULONG   NumDsps;
    ULONG   StripDsp;
    ULONG   StripsState;        /* APOLLO_STRIPS_STATE_* */
    ULONG   StripsStarted;      /* bit i: the plain strip of input i+1 runs */
    ULONG   OwnedMask;
    ULONG   QuarantinedMask;
    ULONG   Instances;
    ULONG   PagesAllocated;     /* 4 KiB transaction pages */
    ULONG   PagesParked;        /* published, never proven: kept until the device is gone */
    ULONG   CatalogEntries;
    ULONG   BoardQueries;
    ULONG   BoardQueryFailures;
    ULONG   Admitted;
    ULONG   Refused;
    LONG    LastStatus;         /* the last operation's outcome (NTSTATUS) */
    ULONG   LastStep;           /* APOLLO_PLUGIN_STEP_* where it ended */
    APOLLO_PLUGIN_DSP_INFO Dsp[APOLLO_PLUGIN_DSPS];
    APOLLO_PLUGIN_INSTANCE_INFO Inst[APOLLO_PLUGIN_INSTANCES_MAX];
    CHAR    Why[96];            /* why the host is (not) enabled, or the last refusal */
} APOLLO_PLUGIN_CAPS, *PAPOLLO_PLUGIN_CAPS;

/* ---------------------------------------------------------------- BOARD_QUERY */

/*
 * One transaction on DSP 0 (oa:driver/ua_plugin_license.c:98-187): a 770-dword reply
 * descriptor prefilled with 0x83000000, then {0x00100002, date} (it SETS THE DEVICE'S DATE, as
 * UA's driver does on every query) and {0x00110002, cookie}. Accepted only after the response
 * ring retired the descriptor within 2 s, with word 0 0x80030302 and word 1 the cookie, epoch
 * unchanged. A timeout does not quarantine DSP 0. Words[i] = the device's word for render id
 * 0x800 + i, exactly as sent.
 */
#define APOLLO_BOARD_QUERY_WORDS            768
#define APOLLO_BOARD_QUERY_BASE             0x800u

typedef struct _APOLLO_BOARD_QUERY {
    ULONG   Size;               /* in: sizeof(APOLLO_BOARD_QUERY) */
    ULONG   Flags;              /* in: 0 */
    LONG    Status;             /* out: NTSTATUS of the query */
    ULONG   Date;               /* out: the date word sent */
    ULONG   Epoch;              /* out */
    ULONG   Reply0;             /* out: reply word 0 as the device left it (0x80030302 = the answer) */
    ULONG   Reply1;             /* out: reply word 1 (the cookie echo) */
    ULONG   Cookie;             /* out: the cookie sent */
    ULONG   WordsWritten;       /* out: reply words that are not the prefill (diagnostics) */
    ULONG   ElapsedMs;          /* out: publish to retirement */
    ULONG   Reserved[2];
    ULONG   Words[APOLLO_BOARD_QUERY_WORDS];    /* out, valid only when Status is 0 */
} APOLLO_BOARD_QUERY, *PAPOLLO_BOARD_QUERY;

/* The record state of one table word (ua_plugin_license.c:62-96): only AUTHORIZED and DEMO admit. */
#define APOLLO_PLUGIN_LICENSE_AUTHORIZED    0u      /* 0x81000000, 0x82000000 */
#define APOLLO_PLUGIN_LICENSE_DEMO          1u      /* any other word: a running demo */
#define APOLLO_PLUGIN_LICENSE_NOT_STARTED   2u      /* 0x80000000 */
#define APOLLO_PLUGIN_LICENSE_EXPIRED       3u      /* 0 */
#define APOLLO_PLUGIN_LICENSE_UNAVAILABLE   4u      /* 0x83000000 (also the prefill) */
#define APOLLO_PLUGIN_LICENSE_UNKNOWN       0xFFFFFFFFu

/* ---------------------------------------------------------------- PLUGIN_INSTANCE */

#define APOLLO_PLUGIN_INST_CREATE           1u
#define APOLLO_PLUGIN_INST_DESTROY          2u
#define APOLLO_PLUGIN_INST_SERVICE          3u
#define APOLLO_PLUGIN_INST_START            8u      /* upstream's number (ua_ioctl.h:1094-1101): route a staged chain */
/*
 * [Ins] Unison (upstream's numbers, ua_ioctl.h:1088-1090 and 1189-1214; docs/PLUGINS.md 1.9, driver\unison_plan.h).
 * Every field zero but Size, Op, Handle and Epoch (of an F_UNISON instance of this handle), and for
 * UNISON_PARAMS NCommands = the word count (a multiple of 3, at most APOLLO_PLUGIN_UNISON_WORDS_MAX) with the
 * triples {input (DSP input = preamp index), preamp parameter (the kext's ids 0x00-0x17 but Link 0x02), value}
 * where the commands go. Status as for every op; Detail[0] = the mailbox SEQ the fence saw consumed.
 *   BEGIN_UNISON    (staged, not begun, no other routed owner on the input): the ordinary baseline captured
 *                   (readback flags and GainC over the write cache), the lane owned (every other write to its
 *                   fields: STATUS_DEVICE_BUSY), PGA 0 written, the mailbox fence, 80 ms. Nothing routed.
 *   ACTIVATE_UNISON (begun): routes the staged chain (as op START) and fences. CUSTOM is NOT set here: the
 *                   caller journals CUSTOM 1 (param 0x08) and waits 80 ms with UNISON_PARAMS, as UA's worker.
 *   UNISON_PARAMS   (begun or activated, owning): the whole batch validated (inputs of the lease, owned fields),
 *                   applied in setting order (duplicates coalesce), fenced. 0 words = a fence only.
 * DESTROY / handle close: the chain stopped and its plain strip restored, then the baseline put back (CUSTOM
 * off and the other changed fields, fence, 80 ms, the gains, fence, GainC read back). An unproven completion
 * keeps the fields blocked and quarantines the DSP until the unit is started again.
 * Statuses: STATUS_INVALID_DEVICE_STATE wrong state, STATUS_DEVICE_BUSY owned / another route owner / a busy
 * field, STATUS_RETRY the mailbox was not settled (try again), STATUS_ACCESS_DENIED an input or field outside
 * the lease, STATUS_IO_TIMEOUT the fence did not complete (the fields stay blocked).
 */
#define APOLLO_PLUGIN_INST_UNISON_PARAMS    5u
#define APOLLO_PLUGIN_INST_BEGIN_UNISON     6u
#define APOLLO_PLUGIN_INST_ACTIVATE_UNISON  7u
#define APOLLO_PLUGIN_UNISON_WORDS_MAX      192u
/*
 * [Lat] LATENCY (ours, FEAT_LATENCY; nothing is sent to the device): Handle and Epoch of a routed input chain of this
 * handle (F_START, or a staged one after op START), Scratch[0] = the plug-in latency through its record tap in frames
 * at the unit's rate (UAD REC: the active inserts' sum; MON: 0), Scratch[1] = the record delay its CREATE wrote (Mic
 * strip m0 word 16), every other field 0. The chain's record path is Scratch[1] + Scratch[0]; the plain strip's is 200
 * (docs/PLUGINS.md 1.5): whatever a chain's path is over 200 (an over-budget chain, "overloaded") is added to the unit's
 * reported input latency (the largest such chain counts) until the chain goes. Detail[0] = the unit's input path now
 * (frames beyond the copy engine and the ADC: the strip DSP's block, the strips' record delay and that excess),
 * Detail[1] = this chain's excess. Send it again after every rebuild; an unrouted, aux or Unison-only chain counts 0.
 * STATUS_NOT_FOUND: not this handle's; STATUS_INVALID_DEVICE_STATE: not routed or another epoch; Scratch[0] above
 * 48000 or Scratch[1] above 2048: STATUS_INVALID_PARAMETER.
 */
#define APOLLO_PLUGIN_INST_LATENCY          9u
#define APOLLO_PLUGIN_LATENCY_MAX           48000u
#define APOLLO_PLUGIN_RECORD_DELAY_MAX      2048u

/*
 * [Ins] The aux buses' returns as a chain's Synch (DSP inputs 0x10/0x11 aux 1, 0x12/0x13 aux 2; the bus ids of
 * oa:driver/ua_apollo.h:816-817): a CREATE F_START or F_STAGE | F_CHAIN with this Synch and the Input Strip
 * carrier (head word 7 = 0x83E) puts a chain on the aux return before the monitor and cue mixes. No plain strip
 * runs there: nothing is retained, the teardown stops the chain (SYNCH 0) and restores nothing. While it runs,
 * the aux return's bus coefficients go to its DSP and DSP 0 (cues.c ApolloCuesAuxOwner). Experimental: taken
 * only with Parameters\AuxInserts = 1 (APOLLO_PLUGIN_ENABLED_AUX), never run on hardware.
 */
#define APOLLO_PLUGIN_AUX_BUSES             2u
#define APOLLO_PLUGIN_AUX_SYNCH(a)          ((0x10u + 2u * (a)) | ((0x11u + 2u * (a)) << 16))

/*
 * CREATE flags (oa:driver/ua_ioctl.h:1063-1159):
 *   F_CHECK  the fresh licence answer only: nothing placed or sent, Handle 0.
 *   (none)   modules uploaded and acknowledged, private rows zero-filled and linked (memspec):
 *            nothing routed or started. NCommands, Synch and Scratch must be 0.
 *   F_START | F_CHAIN  then one transaction: stop each input's plain strip, the WRITEs (checked,
 *            relocated), SYNCH the chain head on Synch, span none twice and route 0x1FFFF per
 *            input. Needs an explicit Dsp = the strip DSP with each input's plain strip live (it
 *            is retained and restored by DESTROY and by closing the handle).
 *            Synch = in1 | in2 << 16: mono, in2 0x20 (in1 = the DSP input: x8p 0-7 = inputs 1-8;
 *            Twin X 0-1 = inputs 1-2, its Hi-Z jack = input 1, its talkback (10) has no Mic strip);
 *            or (FEAT_STEREO) a linked pair, in1 even and in2 = in1 + 1 below the unit's inputs
 *            (x8p 0x00010000, 0x00030002, 0x00050004, 0x00070006 = inputs 1/2, 3/4, 5/6, 7/8;
 *            Twin X 0x00010000 only;
 *            mono in1 is not limited to the unit's inputs, as before: a missing strip is the
 *            operation's STATUS_NOT_FOUND, not the IOCTL's refusal): both plain strips must be
 *            live, both are stopped, the head is SYNCH'd on the pair, and both are restored at
 *            the end. Refused: in1 >= 0x20, in2 > 0x20 or in1 == in2 (STATUS_INVALID_PARAMETER,
 *            upstream's contract); any other pairing (STATUS_NOT_SUPPORTED).
 *            F_START without F_CHAIN is refused (STATUS_INVALID_PARAMETER): chains only.
 *   F_STAGE | F_CHAIN  (FEAT_STAGE) a chain whose startup is over one 1016-word page: the same
 *            CREATE with the startup's FIRST page (whole WRITE blocks, the chain directory in it),
 *            sent as one transaction of WRITEs only: the plain strip keeps running, nothing is
 *            stopped, SYNCH'd or routed. The caller then sends the remaining pages as SERVICE
 *            writes (each one bounded transaction; the directory may not be written again), may
 *            read the startup back, and routes the chain with op START. Same arguments as F_START
 *            (explicit strip DSP, live plain strip(s), Synch mono or a pair, scratch). F_STAGE with
 *            F_START or F_CHECK, or without F_CHAIN, is refused. A staged chain never routed
 *            changes no routing when it goes (DESTROY, handle close): only its modules and rows
 *            are released.
 *   Placement on the strip DSP waits for its strip pass: refused (STATUS_DEVICE_BUSY) unless the
 *   strips are Live, Off or Failed; there the 5 Mic strip module ids are reserved to the strips.
 * SERVICE: a started (or strip-replacing: started or staged) instance only; an unrouted one is refused.
 * START (op 8, ua_plugin_instance.c:1277-1332): Handle and Epoch of a staged instance of this
 *   handle, every other field 0. Refused before anything is sent: not staged or already routed
 *   (STATUS_INVALID_DEVICE_STATE), another instance routed on one of its inputs (STATUS_DEVICE_BUSY),
 *   its epoch, DSP or plain-strip generation gone, or one of its plain strips not live any more
 *   (STATUS_CANCELLED). Then one transaction, as F_START without WRITEs: stop the plain strip(s),
 *   SYNCH the head, span none twice and route 0x1FFFF per input.
 *   An unproven outcome quarantines the DSP, as every transaction does.
 * One routed owner per input, whatever the handle: a pair overlaps a mono chain on either of its
 *   inputs and a pair sharing one (F_START and START refuse it: STATUS_DEVICE_BUSY).
 * Instances belong to the handle that created them: closing it destroys them.
 */
#define APOLLO_PLUGIN_F_CHECK               0x01u
#define APOLLO_PLUGIN_F_START               0x02u
#define APOLLO_PLUGIN_F_STAGE               0x04u
#define APOLLO_PLUGIN_F_CHAIN               0x08u
#define APOLLO_PLUGIN_F_UNISON              0x10u   /* [Ins] with STAGE | CHAIN only: member 0 is a licensed integrated
                                                       preamp header (head word 7), N = 1..5; then BEGIN / ACTIVATE */

/* Where an operation ended (APOLLO_PLUGIN_INSTANCE.Step, CAPS LastStep) */
#define APOLLO_PLUGIN_STEP_NONE             0u
#define APOLLO_PLUGIN_STEP_VALIDATE         1u      /* request checked: rows, WRITEs, chain directory */
#define APOLLO_PLUGIN_STEP_LICENCE          2u      /* the board query and the decision */
#define APOLLO_PLUGIN_STEP_MODULES          3u      /* store files read and checked */
#define APOLLO_PLUGIN_STEP_PLACE            4u      /* ownership, strip target, ledger */
#define APOLLO_PLUGIN_STEP_UPLOAD           5u      /* Detail[0] = resource index, [1..3] reply words 0-2 */
#define APOLLO_PLUGIN_STEP_PRIVATES         6u      /* zero-fills + memspec */
#define APOLLO_PLUGIN_STEP_START            7u      /* stop strip, WRITEs, SYNCH, span/route (F_STAGE: WRITEs only) */
#define APOLLO_PLUGIN_STEP_DONE             8u
#define APOLLO_PLUGIN_STEP_TEARDOWN         9u      /* stop, strip restore, releases */
#define APOLLO_PLUGIN_STEP_SERVICE_WRITE    10u
#define APOLLO_PLUGIN_STEP_SERVICE_READ     11u     /* Detail[0..1] = reply words 0-1 on a mismatch */
#define APOLLO_PLUGIN_STEP_ROUTE            12u     /* op START: stop strip, SYNCH the staged head, span/route */
#define APOLLO_PLUGIN_STEP_ANALOG           13u     /* [Ins] the Unison lease: capture, writes, fence, restore */

typedef struct _APOLLO_PLUGIN_ROW {
    ULONG   Pool;               /* 1, 2 or 3 */
    ULONG   Dwords;
    ULONG   Link;               /* row << 24 | word; 0xFFFFFFFF = none (row 0 only) */
    ULONG   Flags;              /* 0 or 2 */
} APOLLO_PLUGIN_ROW;

/*
 * Input: this header, then NResources x 32-byte SHA-256, NPrivate x APOLLO_PLUGIN_ROW, then
 * NCommands x ULONG (UA's WRITE blocks {0x0014LLLL, row << 24 | word, skip mask, values}).
 * Output: this header, then (SERVICE with ReadWords) ReadWords x ULONG.
 */
typedef struct _APOLLO_PLUGIN_INSTANCE {
    ULONG   Size;               /* in: sizeof(APOLLO_PLUGIN_INSTANCE) */
    ULONG   Op;                 /* APOLLO_PLUGIN_INST_* */
    ULONG   Flags;              /* CREATE: APOLLO_PLUGIN_F_*; else 0 */
    ULONG   Handle;             /* CREATE: out; DESTROY, SERVICE, START: in */
    ULONG   RenderId;           /* CREATE: the plug-in's (first effect's) render id */
    ULONG   Dsp;                /* CREATE: in (index or ANY), out the DSP used */
    ULONG   NResources;         /* CREATE: 1..64 (0 with F_CHECK) */
    ULONG   NPrivate;           /* CREATE: 1..127 (0 with F_CHECK) */
    ULONG   NCommands;          /* CREATE F_START: 4..1016 (F_STAGE: the first page); SERVICE: 0 or 4..1016 */
    ULONG   Synch;              /* CREATE F_START / F_STAGE: in1 | in2 << 16 */
    ULONG   Scratch[3];         /* CREATE F_START / F_STAGE: scratch words of pools 1, 2, 3 (0 = none) */
    ULONG   ReadRef;            /* SERVICE: row << 24 | word */
    ULONG   ReadWords;          /* SERVICE: 0..64 */
    ULONG   Epoch;              /* CREATE: out; SERVICE, START: in (the CREATE's) */
    /* out */
    LONG    Status;             /* NTSTATUS of the operation */
    ULONG   Step;               /* APOLLO_PLUGIN_STEP_* where it ended */
    ULONG   LicenseState;       /* APOLLO_PLUGIN_LICENSE_* (CREATE, also when refused) */
    ULONG   LicenseWord;        /* the device's word for RenderId */
    ULONG   Detail[4];
    ULONG   Uploads;            /* modules this CREATE uploaded (each acknowledged) */
    ULONG   Resident;           /* modules already resident there (shared) */
    ULONG   ElapsedMs;
    ULONG   Reserved[3];
} APOLLO_PLUGIN_INSTANCE, *PAPOLLO_PLUGIN_INSTANCE;

#define APOLLO_PLUGIN_INSTANCE_IN_MAX \
    (sizeof(APOLLO_PLUGIN_INSTANCE) + APOLLO_PLUGIN_RES_MAX * 32 + \
     APOLLO_PLUGIN_PRIV_MAX * sizeof(APOLLO_PLUGIN_ROW) + APOLLO_PLUGIN_CMD_WORDS_MAX * sizeof(ULONG))

/* The module store apollo.sys reads (admin-only, like ApolloStrips; apollo-plug modules install). */
#define APOLLO_MODULE_DIR_NT                L"\\SystemRoot\\System32\\drivers\\ApolloModules\\"
#define APOLLO_MODULE_DIR_UNDER_ROOT        L"System32\\drivers\\ApolloModules"
#define APOLLO_MODULE_BYTES_MAX             262136u     /* (256 KiB) - 8: one upload transaction */
#define APOLLO_PLUGIN_REG_ENABLE            L"PluginHost"
#define APOLLO_PLUGIN_REG_IMAGE             L"PluginHostImage"  /* REG_SZ: the one program taking the calls unelevated, "X:\..." */
#define APOLLO_PLUGIN_IMAGE_CCH_MAX         520u    /* PluginHostImage characters (terminator not counted); longer = absent */
#define APOLLO_PLUGIN_REG_USER              L"PluginHostUser"   /* REG_SZ: that program's user, a SID string ("S-1-5-21-...") */
#define APOLLO_PLUGIN_REG_AUX               L"AuxInserts"       /* [Ins] DWORD, default 0: chains on the aux returns */
#define APOLLO_PLUGIN_USER_CCH_MAX          184u    /* the longest SID string (15 sub-authorities); longer = absent */
