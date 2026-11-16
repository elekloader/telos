/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TELOS's page: SETTINGS > MODS (TELOS) (row.s) opens it over the SETTINGS menu. One row per mod in
 * the store, its state on the right, and APPLY & RESTART last:
 *
 *   ON     on, running              OFF    off
 *   +ON    on from the next boot    -OFF   off from the next boot (running until then)
 *   E<n>   on, but the boot refused it: n is the refusal code (docs/FORMAT.md, "+Drive refusal codes")
 *
 * UP and DOWN (or any knob) move; YES switches the mod on or off for the next boot, written to the store
 * at once (RIGHT on, LEFT off); YES on APPLY & RESTART reboots, and the boot links the new set. NO goes
 * back to SETTINGS. While the page is open it takes every key but PLAY, STOP and TEMPO.
 */
#include "telos.h"

#define EV_KEY(ev)   (*(uint32_t *)((char *)(ev) + 12))
#define EV_FLAGS(ev) (*(uint32_t *)((char *)(ev) + 16))
#define EV_DELTA(ev) (*(int32_t *)((char *)(ev) + 16))

#define ROWS_SHOWN  5                    /* rows under the title bar, 10 pixels each */

#ifdef TELOS_DT2                         /* the II's text call takes flags, not a length: 0, plain */
#define TEXTF(bmp, font, x, y, maxlen, ...) fw_textf(bmp, font, x, y, 0, __VA_ARGS__)
#else
typedef int32_t (*text_fn)(void *bmp, const void *font, int32_t x, int32_t y, int32_t maxlen, const char *fmt, ...);
#define TEXTF ((text_fn)fw_textf)
#endif

static uint8_t open, dirty, eat;        /* eat: a key whose release the page still takes */
static int32_t sel, top, acc;
static uint32_t sig;
static const char *msg;                  /* a line for the footer, after a failed write */

static int ran(int i) { return (telos_boot_flags[i >> 5] >> (i & 31)) & 1; }

static int rows(void) { return telos_dir_ok ? telos_dir.n + 1 : 1; }    /* the mods, then APPLY */

static int pending(void)
{
    int i;
    for (i = 0; i < telos_dir.n; i++)
        if (!(telos_dir.e[i].flags & TS_ON) != !ran(i))
            return 1;
    return 0;
}

/* A row's state, as the page shows it. */
static void state(int i, char *s)
{
    int on = (telos_dir.e[i].flags & TS_ON) != 0, loaded = ran(i) && telos_boot_st[i].state == DL_ST_LOADED;
    const char *t = on ? (loaded ? "ON" : ran(i) ? 0 : "+ON") : (loaded ? "-OFF" : "OFF");
    int k = 0;
    if (!t) {                            /* chosen at boot and not loaded: the refusal */
        uint32_t c = telos_boot_st[i].code;
        s[k++] = 'E';
        if (c >= 10)
            s[k++] = (char)('0' + c / 10);
        s[k++] = (char)('0' + c % 10);
        s[k] = 0;
        return;
    }
    while (*t)
        s[k++] = *t++;
    s[k] = 0;
}

static void move(int d)
{
    int n = rows();
    sel += d;
    if (sel < 0)
        sel = 0;
    if (sel >= n)
        sel = n - 1;
    if (sel < top)
        top = sel;
    if (sel >= top + ROWS_SHOWN)
        top = sel - ROWS_SHOWN + 1;
}

extern volatile uint8_t telos_dir_busy;  /* upload.c: SysEx requests wait while it is set */

static void set(int i, int on)
{
    uint8_t was = telos_dir.e[i].flags;
    if (!(was & TS_ON) == !on)
        return;
    telos_dir_busy = 1;
    telos_dir.e[i].flags ^= TS_ON;
    msg = 0;
    if (telos_dir_write() != 0) {
        telos_dir.e[i].flags = was;      /* not written: as it was */
        msg = "STORE WRITE FAILED";
    }
    telos_dir_busy = 0;
}

void telos_page_open(void)
{
    open = 1;
    sel = top = acc = 0;
    msg = 0;
    dirty = 1;
}

volatile uint32_t telos_lastkey;         /* the last key event, key << 8 | flags: for checking a port's codes */

int telos_key(void *brain, void *ev)
{
    uint32_t key = EV_KEY(ev), fl = EV_FLAGS(ev);
    (void)brain;
    telos_lastkey = key << 8 | (fl & 0xFF);
    if (!open) {
        if (eat && key == eat) {         /* NO's release: SETTINGS would close on it */
            if (!(fl & 1))
                eat = 0;
            return 1;
        }
        return 0;
    }
    if (key == KEY_PLAY || key == KEY_STOP || key == KEY_TEMPO)
        return 0;                        /* the sequencer stays the Digitone's */
    if (!(fl & 1))
        return 1;                        /* releases */
    dirty = 1;
    if (key == KEY_NO && !(fl & 8)) {
        open = 0;                        /* back to SETTINGS, under the page all along */
        eat = KEY_NO;
    } else if (key == KEY_UP || key == KEY_DOWN) {
        move(key == KEY_UP ? -1 : 1);    /* repeats too */
    } else if (fl & 8) {
        return 1;
    } else if (!telos_dir_ok) {
        return 1;
    } else if (sel < telos_dir.n && (key == KEY_YES || key == KEY_LEFT || key == KEY_RIGHT)) {
        set(sel, key == KEY_YES ? !(telos_dir.e[sel].flags & TS_ON) : key == KEY_RIGHT);
    } else if (sel == telos_dir.n && key == KEY_YES && telos_dir.n) {
        core_restart();                  /* the next boot links the store's set */
    }
    return 1;
}

int telos_enc(void *brain, void *ev)
{
    int32_t d;
    (void)brain;
    if (!open)
        return 0;
    acc += EV_DELTA(ev);
    d = acc / 4;                         /* 4 counts a detent */
    acc -= d * 4;
    if (d) {
        move(d);
        dirty = 1;
    }
    return 1;
}

void telos_draw(void *bmp, void *ctrl)
{
    int i, y;
    char s[8];
    uint32_t used = 0;
    (void)ctrl;
    if (!open)
        return;
    fw_fillrect(bmp, 0, 0, 127, 63, 0);
    TEXTF(bmp, FONT_TITLE, 2, 56, -1, "MODS (TELOS)");
    if (telos_dir_ok) {
        for (i = 0; i < telos_dir.n; i++)
            used += (telos_dir.e[i].size + 1023) / 1024;
        TEXTF(bmp, FONT_TITLE, 84, 56, -1, "%dK/2M", (int)used);
    }
    fw_fillrect(bmp, 0, 54, 127, 63, -1);        /* the title bar, inverted */
    for (i = top, y = 44; i < rows() && i < top + ROWS_SHOWN; i++, y -= 10) {
        if (!telos_dir_ok) {
            TEXTF(bmp, fw_font5, 3, y, -1, "NO STORE ON THE +DRIVE");
        } else if (i < telos_dir.n) {
            const struct ts_entry *e = &telos_dir.e[i];
            TEXTF(bmp, fw_font5, 3, y, 20, "%s", e->title[0] ? e->title : e->name);
            state(i, s);
            TEXTF(bmp, fw_font5, 100, y, -1, "%s", s);
        } else {
            TEXTF(bmp, fw_font5, 3, y, -1, telos_dir.n ? (pending() ? "APPLY & RESTART" : "RESTART")
                                                       : "NO MODS IN THE STORE");
        }
        if (i == sel)
            fw_fillrect(bmp, 0, y - 2, 123, y + 6, -1);
    }
    if (rows() > ROWS_SHOWN) {           /* the scroll bar */
        int h = 50 * ROWS_SHOWN / rows(), p = (50 - h) * top / (rows() - ROWS_SHOWN);
        fw_fillrect(bmp, 126, 51 - p - h, 127, 51 - p, -1);
    }
    if (msg)
        TEXTF(bmp, fw_font5, 3, 1, -1, "%s", msg);
}

/* upload.c: the store changed (an upload, a SET or DELETE over SysEx, a FORMAT) */
void telos_page_changed(void)
{
    int n = rows();
    if (sel >= n)
        sel = n - 1;
    if (top > sel)
        top = sel;
    dirty = 1;
}

void telos_upload_tick(void);

void telos_page_tick(void *ctrl)
{
    uint32_t h = (uint32_t)(open | sel << 1 | top << 8) ^ telos_dir.seq * 977u;
    telos_upload_tick();                 /* upload.c's requests, in this task */
    if (!open && !dirty)
        return;
    if (dirty || h != sig) {             /* dirty when closed too: SETTINGS drawn again */
        dirty = 0;
        sig = h;
        *((unsigned char *)ctrl + 0x20) = 1;    /* a redraw */
    }
}

/* ---- the SETTINGS row's icon ------------------------------------------------------------------------
 * A stock SETTINGS row's icon is its draw callback blitting a 9 x 9 bitmap object (the item constructor
 * 0x400dfc74 wraps one in a draw functor, 0x400dfaa0) at the x, y the menu gives. The object, as the
 * firmware's constructor 0x400dc59a (1.43) builds it: +0 vtable, +4 height, +8 width, +12 words a row,
 * +16 the pixels, +20 the mask (one word a row, bit 31 the left pixel), +24 a byte 0. The vtable is
 * ICON_VTABLE (mod.json's defsym, 1.44's in its port). */
#ifndef TELOS_DT1
extern const char ICON_VTABLE[];
static const uint32_t gear_px[9] = {0x14000000, 0x5D000000, 0x3E000000, 0xE3800000, 0x63000000,
                                    0xE3800000, 0x3E000000, 0x5D000000, 0x14000000};
static const uint32_t gear_mask[9] = {0xFF800000, 0xFF800000, 0xFF800000, 0xFF800000, 0xFF800000,
                                      0xFF800000, 0xFF800000, 0xFF800000, 0xFF800000};
static struct { const void *vt; int32_t h, w, words; const uint32_t *px, *mask; uint32_t flag; }
    __attribute__((aligned(4))) gear = {ICON_VTABLE, 9, 9, 1, gear_px, gear_mask, 0};

/* draw(payload, item, bmp, x, y): the row's icon, as a stock row draws its own */
void telos_row_draw(void *payload, void *item, void *bmp, int32_t x, int32_t y)
{
    (void)payload; (void)item;
    fw_blit(bmp, &gear, x, y, 0);
}
#else
/* The Digitakt's SETTINGS rows have no icon. */
void telos_row_draw(void *payload, void *item, void *bmp, int32_t x, int32_t y)
{
    (void)payload; (void)item; (void)bmp; (void)x; (void)y;
}
#endif
