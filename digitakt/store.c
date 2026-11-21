/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TELOS on the Digitakt mk1: the store as a file on the +Drive's file system (ekFS), through core 4.5's
 * fw_fs_* (the Digitone's is raw sectors; telos.h has the layout, the same in both).
 *
 *   /telos/store      16 zero bytes, then the store's sectors (TS_SECTORS of 512 bytes, written as used)
 *   /telos/projects   16 zero bytes, then a sector per project slot (0-127): the slot's mods (projset.c)
 *
 * The 16 zero bytes first: stock's SAMPLES browser lists every folder, and a file it loads as a sample
 * reads as an empty one (the Rytm's drive-loader does the same). ekFS writes through to the card, a
 * 16 KB block at a time; a write past a file's end grows it. Every write here starts at or before the
 * file's end (FORMAT writes the store's first 16 KB, the files follow on from sector 32, and the projects
 * file is made whole), so no file has a hole.
 *
 * From a task (the init task at ev_fs_ready, the UI task, the MIDI task), never an interrupt: ekFS takes
 * its own mutex in each call, and a task that finds it held waits.
 */
#include "../telos.h"

#define DIR     "/telos"
#define STORE   "/telos/store"
#define PROJ    "/telos/projects"
#define HEAD    16u
#define SLOTS   128u

static const u8 zero[512] __attribute__((aligned(16)));

static int pio(const char *path, u32 off, u32 n, void *buf, int write)
{
    struct fw_fs_file f;
    int32_t r;
    if (fw_fs_open(path, write ? "w" : "r", &f) != 0)
        return -1;
    f.pos = off;
    r = write ? fw_fs_write(buf, n, &f) : fw_fs_read(buf, n, &f);
    fw_fs_close(&f);
    return r == (int32_t)n ? 0 : -2;
}

int ts_read(u32 sector, u32 bytes, void *buf)
{
    if (sector + (bytes + 511) / 512 > TS_SECTORS)
        return -3;
    return pio(STORE, HEAD + sector * 512, bytes, buf, 0);
}

int ts_write(u32 sector, u32 bytes, const void *buf)
{
    if (sector + (bytes + 511) / 512 > TS_SECTORS)
        return -3;
    return pio(STORE, HEAD + sector * 512, bytes, (void *)buf, 1);
}

/* A file of `sectors` zero sectors after the header, from `from` on (made if it is not there). */
static int zeros(const char *path, u32 from, u32 sectors)
{
    struct fw_fs_file f;
    u32 i;
    int r = 0;
    if (fw_fs_open(path, "w", &f) != 0)
        return -1;
    if (!from) {
        f.pos = 0;
        if (fw_fs_write(zero, HEAD, &f) != (int32_t)HEAD)
            r = -2;
    }
    f.pos = HEAD + from * 512;
    for (i = from; i < sectors && !r; i++)
        if (fw_fs_write(zero, 512, &f) != 512)
            r = -2;
    fw_fs_close(&f);
    return r;
}

/* FORMAT: the folder, and the store's head (the directories, the guard and the index: TS_DATA sectors). */
int ts_create(void)
{
    fw_fs_mkdir(DIR);                    /* -1 when it is there already */
    return zeros(STORE, 0, TS_DATA);
}

int ts_proj_read(int32_t slot, void *buf)
{
    if (slot < 0 || (u32)slot >= SLOTS)
        return -3;
    return pio(PROJ, HEAD + (u32)slot * 512, 512, buf, 0);
}

int ts_proj_write(int32_t slot, const void *buf)
{
    struct fw_fs_file f;
    if (slot < 0 || (u32)slot >= SLOTS)
        return -3;
    if (fw_fs_open(PROJ, "r", &f) == 0)
        fw_fs_close(&f);
    else {                               /* the first save with TELOS: the whole file, no holes */
        fw_fs_mkdir(DIR);
        if (zeros(PROJ, 0, SLOTS) != 0)
            return -1;
    }
    return pio(PROJ, HEAD + (u32)slot * 512, 512, (void *)buf, 1);
}
