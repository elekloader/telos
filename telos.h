/* SPDX-License-Identifier: GPL-2.0-or-later */
/* TELOS for the Digitone mk1: the store on the card, and what telos.c and menu.c share.
 *
 * The store is 2 MB of the eMMC's raw sectors that no stock code reaches (README, "The store").
 * tools/telos.py writes the same layout on the computer.
 * All words big-endian (the unit's own order).
 *
 *   sectors +0..+7    directory copy A (4096 bytes)
 *   sectors +8..+15   directory copy B
 *   sector  +16       the crash guard: "ARMD" while a boot with mods has not run 10 s
 *   sectors +17..+31  the index of mods TELOS has met, for a project's mods (projset.c)
 *   sectors +32..     the files, each from a sector of its own, in the order the directory gives
 *
 * A directory: +0 "TELS", +4 u16 version (1), +6 u16 entries, +8 u32 sequence, +12 u32 CRC-32
 * (zlib's) of all 4096 bytes with this word zero, +16 u32 the store's sectors, +20 u32 the first
 * file sector, +24 8 bytes zero, then 63 entries of 64 bytes. The valid copy with the higher
 * sequence counts; a write goes to the other one with the sequence + 1, so a write cut off by a
 * power loss leaves the older copy standing.
 */
#ifndef TELOS_H
#define TELOS_H

#include "vendor/drive-loader/dl.h"
#if defined(TELOS_DT2) || defined(TELOS_DN2)
#define TELOS_MK2                        /* the Digitakt II and Digitone II: the mk1 Digitakt's project lists (TELOS_DT1 too) */
#endif
#if defined(TELOS_DT2)                   /* the Digitakt II: the mk1's file store */
#include "digitakt-mk2/core.h"
#elif defined(TELOS_DN2)                 /* the Digitone II: raw sectors (digitone2/store.c) */
#include "digitone-mk2/core.h"
#elif defined(TELOS_DT1)
#include "digitakt-mk1/core3.h"
#else
#include "digitone-mk1/core3.h"
#endif

/* The store's sectors, from its start (0 .. TS_SECTORS - 1): 0, or < 0. The Digitone's are raw eMMC
 * sectors from TS_SECTOR; the Digitakt's a file on the +Drive (digitakt/store.c). */
int ts_read(u32 sector, u32 bytes, void *buf);
int ts_write(u32 sector, u32 bytes, const void *buf);
int ts_create(void);                     /* FORMAT: makes the store where it must be made (0, or < 0) */

/* The keys, as the firmware's key events give them (the Digitakt's are one lower; the Digitakt II's
 * and Digitone II's are their panel codes, in another order). */
#if defined(TELOS_MK2)
#define KEY_TEMPO   9
#define KEY_PLAY    20
#define KEY_STOP    21
#define KEY_YES     10
#define KEY_NO      12
#define KEY_UP      11
#define KEY_DOWN    14
#define KEY_LEFT    13
#define KEY_RIGHT   15
#define FONT_TITLE  fw_font5
#elif defined(TELOS_DT1)
#define KEY_TEMPO   8
#define KEY_PLAY    10
#define KEY_STOP    11
#define KEY_YES     12
#define KEY_NO      13
#define KEY_UP      14
#define KEY_DOWN    15
#define KEY_LEFT    16
#define KEY_RIGHT   17
#define FONT_TITLE  fw_font5
#else
#define KEY_TEMPO   9
#define KEY_PLAY    11
#define KEY_STOP    12
#define KEY_YES     13
#define KEY_NO      14
#define KEY_UP      15
#define KEY_DOWN    16
#define KEY_LEFT    17
#define KEY_RIGHT   18
#define FONT_TITLE  fw_font_title
#endif

#define TS_SECTOR     0x380000u          /* tools/telos.py's STORE */
#define TS_SECTORS    0x1000u            /* 2 MB */
#define TS_DIR_A      0u
#define TS_DIR_B      8u
#define TS_DIR_BYTES  4096u
#define TS_ARMED      16u
#define TS_DATA       32u
#define TS_MAX        63
#define TS_VERSION    1

#define TS_ON         1u                 /* an entry's flags: chosen for the next boot */

struct ts_entry {
    char name[32];                       /* the file's name, NUL-terminated: what dl_read_file reads */
    char title[16];                      /* the MODS page's name for it, NUL-terminated */
    u32 sector;                          /* its first sector, from the store's start */
    u32 size;                            /* bytes */
    u32 crc;                             /* CRC-32 of the file */
    u8 flags;                            /* TS_ON */
    u8 pad[3];
};

struct ts_dir {
    char magic[4];                       /* "TELS" */
    u16 version, n;
    u32 seq, crc, sectors, data;
    u8 pad[8];
    struct ts_entry e[TS_MAX];
    u8 tail[TS_DIR_BYTES - 32 - TS_MAX * 64];
};

/* What the boot did, per entry of the directory it read (menu.c shows it). */
struct ts_boot {
    u8 state;                            /* DL_ST_* (DL_ST_NONE: not chosen at boot) */
    u8 code;                             /* DL_R_* when refused */
    char detail[30];
};

extern struct ts_dir telos_dir;          /* the directory as the menu has it (its copy is current) */
extern struct ts_boot telos_boot_st[TS_MAX];
extern u32 telos_boot_flags[2];          /* bit i: entry i was chosen at boot (63 entries) */
extern volatile u32 telos_state[8];      /* [0] phase, [1] loaded, [2] chosen, [3] skip, [4] dir found */
extern int telos_dir_ok;

int telos_dir_write(void);               /* the directory to the other copy: 0, or < 0 */

#endif
