/*
 * regs.h - Apollo Thunderbolt BAR0 register map (Windows port).
 *
 * Every value is from open-apollo's driver/ua_apollo.h and the specs in
 * docs/ (spec-bringup.md section 2.2, spec-streaming.md section 0.1,
 * spec-dsp-mixer.md sections 2-3). All registers are 32-bit little endian.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

/* PCI identity */
#define APOLLO_PCI_VENDOR               0x1A00
#define APOLLO_PCI_DEVICE_APOLLO        0x0002
#define APOLLO_SUBSYS_X8P_FALLBACK      0x0014
#define APOLLO_SUBSYS_8P_ORIGINAL       0x0006
#define APOLLO_SUBSYS_SOLO              0x000F

#define APOLLO_BAR0_MIN                 0xC500  /* highest offset used + 1 */

/* Identity */
#define REG_FW_VERSION                  0x0000  /* really DSP 0 status word */
#define REG_ID_WORD                     0x0004
#define REG_FW_RELEASE                  0x0008
#define REG_SERIAL0                     0x0020  /* 16 bytes, 0x20-0x2F; text ends early (apollo_model.h) */
#define REG_HW_UID_LO                   0x0030  /* hardware UID: the per-unit registry key */
#define REG_HW_UID_HI                   0x0034
#define REG_DEV_INFO_1A4                0x01A4
#define REG_DEV_INFO_1AC                0x01AC

/* DSP register windows: n*0x800, +0x2000 for n > 3 */
#define DSP_WIN_STATUS                  0x000
#define DSP_WIN_LOAD                    0x00C
#define DSP_WIN_READY                   0x1A4
#define DSP_READY_NOT_STARTED           0xA8CAED0F
#define DSP_STATUS_FAULT_MASK           0x0080007F
#define DSP_STATUS_SLAVE_BITS           0x00001C00

/* DSP ring banks: 0x2000 + n*0x80 (n < 4), 0x5E00 + n*0x80 (n >= 4) */
#define DSP_BANK_LOW                    0x2000
#define DSP_BANK_HIGH                   0x5E00
#define DSP_BANK_STRIDE                 0x80
#define DSP_RING_CMD                    0x00
#define DSP_RING_RESP                   0x40
#define RING_PAGE_LO(k)                 (0x00 + (k) * 8)
#define RING_PAGE_HI(k)                 (0x04 + (k) * 8)
#define RING_WRITE_PTR                  0x20
#define RING_DOORBELL                   0x24
#define RING_POSITION                   0x28

#define RING_PAGES                      4
#define RING_PAGE_BYTES                 0x1000
#define RING_BYTES                      (RING_PAGES * RING_PAGE_BYTES)
#define RING_ENTRY_BYTES                16
#define RING_ENTRIES                    1024
#define RING_INDEX_MASK                 (RING_ENTRIES - 1)
#define RING_DMA_REF                    0x80000000

/* DSP commands (inline ring entries: word0 = opcode << 16 | dwords) */
#define DSP_CMD_CONNECT                 0x00230002  /* {cmd, 1}          */
#define DSP_CMD_DATE                    0x00100002  /* {cmd, date}       */
#define DSP_CMD_RATE                    0x001A0002  /* {cmd, rate_hz}    */

/* Global, interrupt and transport registers */
#define REG_DMA_CTRL                    0x2200
#define REG_IMR                         0x2204
#define REG_ISR                         0x2208      /* write 1 to clear */
#define REG_FPGA_REV                    0x2218
#define REG_DSP_RESET                   0x221C      /* never written */
#define REG_TRANSPORT                   0x2220      /* only ever written 0 */
#define REG_EXT_CAPS                    0x2234
#define REG_SERIAL                      0x2238
#define REG_AX_BUF_SIZE                 0x2240
#define REG_AX_SAMPLE_POS               0x2244
#define REG_AX_CONTROL                  0x2248
#define REG_AX_P2P_ROUTE                0x224C
#define REG_AX_PLAY_CH                  0x2250
#define REG_AX_FRAME_CTR                0x2254
#define REG_AX_FRAME_CFG                0x2258
#define REG_AX_REC_CH                   0x225C
#define REG_AX_CONNECT                  0x2260
#define REG_ISR_HI                      0x2264      /* write 1 to clear */
#define REG_IMR_HI                      0x2268
#define REG_AX_DMA_BLKSZ                0x226C
#define REG_AX_TIMER_CFG                0x2270
#define REG_AX_STATUS                   0x22C0
#define REG_AX_P2P_ENABLE               0x22C4

/* DMA_CTRL fields (v2 firmware) */
#define DMA_CTRL_GLOBAL                 0x00000001
#define DMA_CTRL_ENABLES                0x000001FE  /* bits [8:1], one per DSP */
#define DMA_CTRL_STROBES                0x0001FE00  /* bits [16:9], reset strobes */
#define DMA_CTRL_ENABLE_DSP(i)          (1u << ((i) + 1))

/* AX_CONTROL */
#define AX_CTRL_STOP                    0x0
#define AX_CTRL_ARM                     0x1         /* DMA_EN */
#define AX_CTRL_START                   0xF         /* DMA|PLAY|REC|IRQ, own clock */
#define AX_CTRL_FAULT_BIT7              0x80
#define AX_CTRL_FAULT_BIT8              0x100

/* AX_CONNECT doorbell values */
#define AX_CONNECT_CONNECT              0x01
#define AX_CONNECT_CLOCK                0x04
#define AX_CONNECT_DISCONNECT           0x10

/* Interrupt vectors (logical -> hardware bit) */
#define VEC_NOTIFICATION                0x28        /* HW bit 32 = IMR_HI bit 0  */
#define VEC_PERIOD_TIMER                0x46        /* HW bit 62 = IMR_HI bit 30 */
#define VEC_END_OF_BUFFER               0x47        /* HW bit 63 = IMR_HI bit 31 */
#define IMR_HI_NOTIFICATION             0x00000001
#define IMR_HI_PERIOD_TIMER             0x40000000
#define IMR_HI_END_OF_BUFFER            0x80000000

/* Mixer mailbox window */
#define REG_MIXER_BASE                  0x3800
#define REG_MIXER_SEQ_WR                0x3808
#define REG_MIXER_SEQ_RD                0x380C
#define REG_MIXER_RB_STATUS             0x3810      /* 1 = ready, write 0 to re-arm */
#define REG_MIXER_RB_DATA               0x3814      /* 40 words */
#define MIXER_NUM_SETTINGS              38
#define MIXER_SETTING_STRIDE            8

/* Audio rings and SG tables */
#define REG_PLAY_SG                     0x8000      /* 1024 x {lo, hi}, write only */
#define REG_REC_SG                      0xA000
#define AUDIO_SG_ENTRIES                1024
#define AUDIO_SG_PAGE                   0x1000
#define AUDIO_RING_BYTES                (AUDIO_SG_ENTRIES * AUDIO_SG_PAGE)  /* 4 MiB */
#define AUDIO_RING_FRAMES               8192
#define AUDIO_MAX_CHANNELS              64

/* Notification bank (0xC000 + bank shift 0x28) */
#define REG_NOTIF_CONFIG                0xC000      /* 10 dwords after connect */
#define REG_NOTIF_READBACK              0xC028      /* 95 dwords */
#define REG_NOTIF_ACEFACE               0xC02C
#define REG_NOTIF_STATUS                0xC030      /* clear by writing 0, not W1C */
#define REG_NOTIF_CLOCK_CFG             0xC074      /* (rate_index << 8) | source */
#define REG_NOTIF_RATE_INFO             0xC07C      /* [15:8] rate index, [1:0] lock */
#define REG_NOTIF_XPORT_INFO            0xC080
#define REG_NOTIF_CLOCK_INFO            0xC084
#define REG_NOTIF_CLOCK_SRCS            0xC08C
#define REG_NOTIF_CLOCK_NAMES           0xC090      /* 256 bytes */
#define REG_FW_LATENCY_OUT              0xC198
#define REG_FW_LATENCY_IN               0xC19C
#define REG_IO_DESC_IN                  0xC1A4      /* 72 dwords */
#define REG_IO_DESC_OUT                 0xC2C4      /* 72 dwords */
#define IO_DESC_WORDS                   72
#define NOTIF_READBACK_WORDS            95

#define ACEFACE_MAGIC                   0x0ACEFACE
#define NOTIF_STATUS_CONNECT_ACK        (1u << 21)
#define NOTIF_CONNECT_WORD0             0x0000000A
#define NOTIF_CONNECT_WORD1             0x0000017C

#define CLOCK_SOURCE_INTERNAL           0x0C        /* 0 is S/PDIF: never acks */

/* Rate indexes (1..6) */
#define RATE_IDX_44100                  1
#define RATE_IDX_48000                  2
#define RATE_IDX_88200                  3
#define RATE_IDX_96000                  4
#define RATE_IDX_176400                 5
#define RATE_IDX_192000                 6

/* Device types (internal numbering) */
#define APOLLO_TYPE_X8P                 0x22

/* x8p legacy layout (spec-streaming 1.6): verified on Linux */
#define X8P_PLAY_CHANNELS               34
#define X8P_REC_CHANNELS                32
#define X8P_SAFETY_FRAMES_48K           0x08        /* AX_FRAME_CFG at 44.1/48k */

/* Timing (spec-bringup 3.11) */
#define ACEFACE_RETRIES                 20
#define ACEFACE_POLLS                   10
#define ACEFACE_POLL_MS                 100
#define DSP_READY_POLL_MS               300

/* FILETIME of 2007-01-01 00:00 UTC, for the DSP connect date */
#define FILETIME_2007                   128120832000000000ULL
