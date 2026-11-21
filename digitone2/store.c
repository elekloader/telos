/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TELOS on the Digitone II: the store in raw sectors of the card, through core-dn2 0.5's fw_card_read
 * and fw_card_write (the layout in telos.h, the same as the Digitone mk1's), and each project slot's
 * mods in a sector of their own after it (projset.c, as the Digitakt's /telos/projects file).
 *
 *   sectors 0x7FE000 .. 0x7FEFFF   the store (2 MB)
 *   sectors 0x7FF000 .. 0x7FF07F   a sector per project slot (0-127)
 *
 * Why there: stock writes no sector from 0x458000 up (MmcFs refuses them, FORMAT trims only below
 * 0x457FFF; ElekLabs digikit's finding 59), and ElekLabs digikit keeps its own store from 0x800000 to
 * the card's end, so TELOS stays just below it. The Data API's unchecked bank ids reach sectors below
 * 0x800000 (the same finding): only a crafted request from a computer could write here.
 *
 * From a task (the init task at ev_card_ready, the UI task, the MIDI task, the worker that saves a
 * project), never an interrupt.
 */
#include "../telos.h"

#define STORE_AT  0x7FE000u
#define PROJ_AT   0x7FF000u
#define SLOTS     128u

static const u8 zero[512] __attribute__((aligned(16)));

int ts_read(u32 sector, u32 bytes, void *buf)
{
    if (sector + (bytes + 511) / 512 > TS_SECTORS)
        return -3;
    return fw_card_read(STORE_AT + sector, bytes, buf) == 0 ? 0 : -2;
}

int ts_write(u32 sector, u32 bytes, const void *buf)
{
    if (sector + (bytes + 511) / 512 > TS_SECTORS)
        return -3;
    return fw_card_write(STORE_AT + sector, bytes, buf) == 0 ? 0 : -2;
}

/* FORMAT: the store's head (the directories, the guard and the index: TS_DATA sectors) zeroed. */
int ts_create(void)
{
    u32 i;
    for (i = 0; i < TS_DATA; i++)
        if (ts_write(i, 512, zero) != 0)
            return -2;
    return 0;
}

int ts_proj_read(int32_t slot, void *buf)
{
    if (slot < 0 || (u32)slot >= SLOTS)
        return -3;
    return fw_card_read(PROJ_AT + (u32)slot, 512, buf) == 0 ? 0 : -2;
}

int ts_proj_write(int32_t slot, const void *buf)
{
    if (slot < 0 || (u32)slot >= SLOTS)
        return -3;
    return fw_card_write(PROJ_AT + (u32)slot, 512, buf) == 0 ? 0 : -2;
}
