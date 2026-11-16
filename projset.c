/* SPDX-License-Identifier: GPL-2.0-or-later */
/* TELOS: a project's mods.
 *
 * Every project save carries the mods that were running (a core_projdata block, "TLMS": per mod a hash of
 * its id and one of its version). When a loaded project's mods differ from what runs, LOAD PROJECT MODS? lists the switches (+ on, - off, ~ the project's version)
 * and what cannot be switched (? not on the unit, ! another version or refused); YES writes the store and
 * restarts, and that boot says only what is still not as the project had it. The boot's own load of the
 * last project says nothing otherwise, nor does a project without the block.
 *
 * The names come from TELOS's index on the +Drive (sectors +17..+31 of the store): the id, version and
 * title of every mod TELOS has met, in the store or built into the firmware, kept after a mod is removed
 * so a missing mod still has a name. A store file is indexed from its first sector (the elekloader writer
 * puts "id", "version" and "title" first), once.
 *
 * The block goes last in the project area (order 95): when the area is full, the pedals' settings keep
 * their room and the project has no list. */
#include "telos.h"

#define EV_KEY(ev)   (*(uint32_t *)((char *)(ev) + 12))
#define EV_FLAGS(ev) (*(uint32_t *)((char *)(ev) + 16))

#ifdef TELOS_MK2                         /* the II's text call takes flags, not a length: 0, plain */
#define TEXTF(bmp, font, x, y, maxlen, ...) fw_textf(bmp, font, x, y, 0, __VA_ARGS__)
#else
typedef int32_t (*text_fn)(void *bmp, const void *font, int32_t x, int32_t y, int32_t maxlen, const char *fmt, ...);
#define TEXTF ((text_fn)fw_textf)
#endif

extern const char core_desc[];
extern volatile uint8_t telos_dir_busy;

/* ---- the index ------------------------------------------------------------------------------------- */
#define IX_HEAD     17u                  /* the header sector: "TLX2", the count, the pending list */
#define IX_FIRST    18u                  /* the entries: sectors +18..+31 */
#define IX_MAX      112
#define PEND_MAX    48

struct ix_entry {                        /* 64 bytes */
    u32 crc;                             /* the store file's CRC-32; 0: none (built in, or removed) */
    u32 id, ver;                         /* dl_hash of the id and of the version */
    char sid[20];                        /* the id, the version, the title (each NUL-terminated, cut) */
    char sver[8];
    char title[24];
};

static struct {
    char magic[4];                       /* "TLX2" */
    u32 n;
    u32 pending;                         /* pairs in pend: the set a TELOS restart is to load */
    u32 pad;
    u32 pend[PEND_MAX * 2];
    u8 tail[512 - 16 - PEND_MAX * 8];
} ixh __attribute__((aligned(16)));
static struct ix_entry ix[IX_MAX] __attribute__((aligned(16)));
static int ix_ok;
static u8 sec[512] __attribute__((aligned(16)));

static u32 hash(const char *s)
{
    u32 n = 0;
    while (s[n])
        n++;
    return dl_hash(s, n);
}

static void copy(char *d, const char *s, u32 max, int upper)
{
    u32 i;
    for (i = 0; i + 1 < max && s[i]; i++)
        d[i] = (char)(upper && s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
    d[i] = 0;
}

static void ix_load(void)
{
    if (ix_ok)
        return;
    if (ts_read(IX_HEAD, 512, &ixh) == 0 && ixh.magic[0] == 'T' && ixh.magic[1] == 'L'
        && ixh.magic[2] == 'X' && ixh.magic[3] == '2' && ixh.n <= IX_MAX && ixh.pending <= PEND_MAX
        && ts_read(IX_FIRST, sizeof ix, ix) == 0) {
        ix_ok = 1;
        return;
    }
    dl_memset(&ixh, 0, sizeof ixh);
    dl_memset(ix, 0, sizeof ix);
    ixh.magic[0] = 'T'; ixh.magic[1] = 'L'; ixh.magic[2] = 'X'; ixh.magic[3] = '2';
    ix_ok = 1;
}

static void ix_save(void)
{
    if (ts_write(IX_HEAD, 512, &ixh) == 0)
        ts_write(IX_FIRST, sizeof ix, ix);
}

static const struct ix_entry *by_crc(u32 crc)
{
    u32 i;
    for (i = 0; i < ixh.n; i++)
        if (ix[i].crc == crc)
            return &ix[i];
    return 0;
}

/* The entry for (id, ver); ver 0: any version, the last met. */
static const struct ix_entry *by_id(u32 id, u32 ver)
{
    int i;
    for (i = (int)ixh.n - 1; i >= 0; i--)
        if (ix[i].id == id && (!ver || ix[i].ver == ver))
            return &ix[i];
    return 0;
}

/* Keep (crc, id, version, title): an entry of its own, or the oldest one no store file holds. */
static int ix_put(u32 crc, const char *id, const char *ver, const char *title)
{
    u32 hi = hash(id), hv = hash(ver), i;
    struct ix_entry *e = 0;
    for (i = 0; i < ixh.n; i++)
        if (ix[i].id == hi && ix[i].ver == hv && (ix[i].crc == crc || !ix[i].crc || !crc)) {
            if (ix[i].crc == crc || !crc)
                return 0;                /* known already */
            e = &ix[i];                  /* a removed file's entry comes back */
            break;
        }
    if (!e && ixh.n < IX_MAX)
        e = &ix[ixh.n++];
    if (!e) {                            /* full: the oldest no store file holds goes */
        for (i = 0; i < IX_MAX && ix[i].crc; i++)
            ;
        if (i == IX_MAX)
            return 0;
        for (; i + 1 < IX_MAX; i++)
            ix[i] = ix[i + 1];
        e = &ix[IX_MAX - 1];
    }
    e->crc = crc;
    e->id = hi;
    e->ver = hv;
    copy(e->sid, id, sizeof e->sid, 0);
    copy(e->sver, ver, sizeof e->sver, 1);
    copy(e->title, title && title[0] ? title : id, sizeof e->title, 1);
    return 1;
}

/* A string value's bytes at p (after its opening quote) to d: -> past its closing quote, or 0. */
static const u8 *str_at(const u8 *p, const u8 *end, char *d, u32 max)
{
    u32 k = 0;
    while (p < end && *p != '"') {
        if (*p == '\\' && p + 1 < end)
            p++;
        if (k + 1 < max)
            d[k++] = (char)*p;
        p++;
    }
    d[k] = 0;
    return p < end ? p + 1 : 0;
}

/* A store file's id, version and title from its first sector: the top-level string values of those keys
 * before anything else nests (the writer puts them first). -> 1 with an id and a version. */
static int head_of(const struct ts_entry *e, char *id, char *ver, char *title)
{
    const u8 *p = sec, *end = sec + 512;
    char key[12];
    id[0] = ver[0] = title[0] = 0;
    if (ts_read(e->sector, 512, sec) != 0)
        return 0;
    while (p < end && *p != '{')
        p++;
    for (p++; p < end;) {
        while (p < end && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t'))
            p++;
        if (p >= end || *p != '"')
            break;
        p = str_at(p + 1, end, key, sizeof key);
        while (p && p < end && (*p == ' ' || *p == ':'))
            p++;
        if (!p || p >= end)
            break;
        if (*p == '"') {
            char v[28];
            p = str_at(p + 1, end, v, sizeof v);
            if (!p)
                break;
            if (!dl_strcmp(key, "id"))
                copy(id, v, 28, 0);
            else if (!dl_strcmp(key, "version"))
                copy(ver, v, 16, 0);
            else if (!dl_strcmp(key, "title"))
                copy(title, v, 24, 0);
        } else if ((*p >= '0' && *p <= '9') || *p == '-' || *p == 't' || *p == 'f' || *p == 'n') {
            while (p < end && *p != ',' && *p != '}')
                p++;
        } else {
            break;                       /* an object or a list: the head is over */
        }
        if (id[0] && ver[0] && title[0])
            break;
    }
    return id[0] && ver[0];
}

/* Every store file and every module built into the firmware, in the index. */
static void ix_update(void)
{
    u32 i, n, pool, mods;
    int changed = 0;
    char id[28], ver[16], title[24];
    ix_load();
    if (telos_dir_ok)
        for (i = 0; i < telos_dir.n; i++) {
            const struct ts_entry *e = &telos_dir.e[i];
            if (by_crc(e->crc))
                continue;
            if (!head_of(e, id, ver, title)) {   /* not a file the writer made: its name stands in */
                copy(id, e->name, sizeof id, 0);
                copy(ver, "?", sizeof ver, 0);
                title[0] = 0;
            }
            changed |= ix_put(e->crc, id, ver, title[0] ? title : e->title);
        }
    pool = dl_rd32((const u8 *)core_desc + 0x18);
    n = dl_rd16((const u8 *)core_desc + 0x20);
    mods = dl_rd32((const u8 *)core_desc + 0x28);
    for (i = 0; i < n; i++) {
        const u8 *m = (const u8 *)core_desc + mods + 16 * i;
        changed |= ix_put(0, core_desc + pool + dl_rd32(m), core_desc + pool + dl_rd32(m + 4), 0);
    }
    if (changed)
        ix_save();
}

/* ---- the sets ------------------------------------------------------------------------------------- */
#define SET_MAX     32
struct set { u32 n, id[SET_MAX], ver[SET_MAX]; };

static struct {                          /* the project's block: what a save writes */
    u32 used;                            /* CORE_PROJDATA_VARIABLE: bytes held, this word included */
    u32 magic;                           /* "TM01" */
    u32 n;
    u32 pair[SET_MAX * 2];               /* id, version */
} pdata __attribute__((aligned(4)));

static struct set run;                   /* what runs: built-in mods (not core, not TELOS) and the store's */

static int has(const struct set *s, u32 id)
{
    u32 i;
    for (i = 0; i < s->n; i++)
        if (s->id[i] == id)
            return (int)i;
    return -1;
}

static void add(struct set *s, u32 id, u32 ver)
{
    if (s->n < SET_MAX && has(s, id) < 0) {
        s->id[s->n] = id;
        s->ver[s->n++] = ver;
    }
}

static int ran(int i) { return (telos_boot_flags[i >> 5] >> (i & 31)) & 1; }

static void run_build(void)
{
    u32 i, n, pool, mods;
    run.n = 0;
    pool = dl_rd32((const u8 *)core_desc + 0x18);
    n = dl_rd16((const u8 *)core_desc + 0x20);
    mods = dl_rd32((const u8 *)core_desc + 0x28);
    for (i = 0; i < n; i++) {
        const u8 *m = (const u8 *)core_desc + mods + 16 * i;
        if (m[8] != 1)                   /* flashed mods only: core and the loader are the firmware */
            continue;
        add(&run, hash(core_desc + pool + dl_rd32(m)), hash(core_desc + pool + dl_rd32(m + 4)));
    }
    if (telos_dir_ok)
        for (i = 0; i < telos_dir.n; i++) {
            const struct ix_entry *x;
            if (!ran((int)i) || telos_boot_st[i].state != DL_ST_LOADED)
                continue;
            x = by_crc(telos_dir.e[i].crc);
            if (x)
                add(&run, x->id, x->ver);
        }
    pdata.magic = 0x544d3031u;           /* "TM01" */
    pdata.n = run.n;
    for (i = 0; i < run.n; i++) {
        pdata.pair[2 * i] = run.id[i];
        pdata.pair[2 * i + 1] = run.ver[i];
    }
    pdata.used = 12 + 8 * run.n;
}

/* ---- the check ------------------------------------------------------------------------------------ */
#define LINES       24
static struct { u8 entry, on; } act[LINES];   /* store entries to switch, for YES */
static u8 nact;
static char lines[LINES][22];
static u8 nlines, scroll;
enum { M_NONE, M_MODS_ASK, M_MODS_INFO };
static u8 mode;
static u8 eat;                           /* a key whose release the screen still takes */
static u8 dirty;
static u32 boot_ticks;
static int boot_load;                    /* the next load is the boot's own */
static u32 proj_pair[SET_MAX * 2], proj_n;    /* the loaded project's list, for YES's restart */
volatile u32 telos_pj[4];                /* [0] loads, [1] lines | actions << 8 | mode << 16, [2] found */

static void line(char tag, const char *name, const char *tail)
{
    char *d;
    u32 k = 0, tl = 0, room;
    if (nlines >= LINES)
        return;
    d = lines[nlines++];
    while (tail && tail[tl])
        tl++;
    room = tl < 12 ? 21 - tl : 9;        /* the name is cut, so its tail shows */
    d[k++] = tag;
    while (*name && k < room)
        d[k++] = *name++;
    if (tail)
        while (*tail && k < 21)
            d[k++] = *tail++;
    d[k] = 0;
}

static void action(int entry, int on)
{
    if (nact < LINES) {
        act[nact].entry = (u8)entry;
        act[nact++].on = (u8)on;
    }
}

/* " 0.2>0.3" */
static void vers(char *v, u32 from, u32 to, u32 id)
{
    const struct ix_entry *a = by_id(id, from), *b = by_id(id, to);
    const char *x = a ? a->sver : "?", *y = b ? b->sver : "?";
    u32 k = 0;
    v[k++] = ' ';
    while (*x && k < 9)
        v[k++] = *x++;
    v[k++] = '>';
    while (*y && k < 18)
        v[k++] = *y++;
    v[k] = 0;
}

/* The store entry of (id, ver), any version when ver is 0, or -1. */
static int entry_of(u32 id, u32 ver)
{
    int i;
    if (!telos_dir_ok)
        return -1;
    for (i = 0; i < telos_dir.n; i++) {
        const struct ix_entry *x = by_crc(telos_dir.e[i].crc);
        if (x && x->id == id && (!ver || x->ver == ver))
            return i;
    }
    return -1;
}

static int loaded_now(int i) { return ran(i) && telos_boot_st[i].state == DL_ST_LOADED; }

/* What it takes for the project's mods to run: the switches (act), and a line for each. issues_only: only
 * what cannot be switched (after TELOS's own restart). */
static void check(const u32 *pair, u32 n, int issues_only)
{
    u32 i;
    int k;
    struct set saved;
    char v[22];
    nlines = nact = 0;
    scroll = 0;
    saved.n = 0;
    for (i = 0; i < n && i < SET_MAX; i++)
        add(&saved, pair[2 * i], pair[2 * i + 1]);
    for (i = 0; i < saved.n; i++) {          /* the project's mods */
        u32 id = saved.id[i], ver = saved.ver[i];
        int r = has(&run, id), want = entry_of(id, ver);
        const struct ix_entry *x = by_id(id, ver), *any = by_id(id, 0);
        const char *name = x ? x->title : any ? any->title : "UNKNOWN MOD";
        if (r >= 0 && run.ver[r] == ver)
            continue;                        /* running as saved */
        if (r >= 0) {                        /* running at another version */
            int cur = entry_of(id, run.ver[r]);
            if (want >= 0 && cur >= 0 && !issues_only) {
                vers(v, run.ver[r], ver, id);    /* the store has the project's: switch */
                action(cur, 0);
                action(want, 1);
                line('~', name, v);
            } else {
                vers(v, ver, run.ver[r], id);
                line('!', name, v);
            }
            continue;
        }
        if (want < 0)
            want = entry_of(id, 0);          /* another version of it, at least */
        if (want >= 0 && ran(want) && telos_boot_st[want].state == DL_ST_REFUSED) {
            u32 c = telos_boot_st[want].code;
            v[0] = ' '; v[1] = 'E';
            v[2] = (char)('0' + (c >= 10 ? c / 10 : c));
            v[3] = (char)(c >= 10 ? '0' + c % 10 : 0);
            v[4] = 0;
            line('!', name, v);              /* on, and the boot refused it */
        } else if (want >= 0 && !issues_only) {
            action(want, 1);
            line('+', name, 0);
        } else if (want < 0) {
            line('?', name, " NOT HERE");
        }
    }
    if (!issues_only && telos_dir_ok)        /* the store's mods the project did not use: off */
        for (k = 0; k < telos_dir.n; k++) {
            const struct ix_entry *x;
            if (!loaded_now(k))
                continue;
            x = by_crc(telos_dir.e[k].crc);
            if (x && has(&saved, x->id) < 0) {
                action(k, 0);
                line('-', x->title, 0);
            }
        }
    mode = nact ? M_MODS_ASK : nlines ? M_MODS_INFO : M_NONE;
    telos_pj[1] = nlines | (u32)nact << 8 | (u32)mode << 16;
    dirty = 1;
}

static void keep_project(void)
{
    u32 i;
    proj_n = pdata.n < SET_MAX ? pdata.n : SET_MAX;
    for (i = 0; i < 2 * proj_n; i++)
        proj_pair[i] = pdata.pair[i];
}

static void proj_loaded(int32_t found)
{
    int boot = boot_load;
    telos_pj[0]++;
    telos_pj[2] = (u32)found;
    boot_load = 0;
    ix_update();                         /* files sent since the boot */
    if (boot) {
        if (ixh.pending) {               /* TELOS restarted for a project's mods: what still is not right */
            check(ixh.pend, ixh.pending, 1);
            ixh.pending = 0;
            ix_save();
        }
    } else if (found && pdata.magic == 0x544d3031u) {
        keep_project();
        check(proj_pair, proj_n, 0);
    }
    run_build();                         /* a save writes what runs */
}

#ifdef TELOS_DT1
/* ---- the Digitakt: no project data in core, so a project's list is a sector of TELOS's own --------------
 * The list file (digitakt/store.c) holds a sector per project slot (0-127), keyed by the slot: core 4.5's
 * ev_project_slot_write clears a slot's (a SAVE, or a CLEAR that makes a new project there), ev_project_saved
 * writes what runs after a SAVE's write, and ev_project_loaded reads the loaded slot's. A project copied
 * to another slot, or sent over Transfer, has no list there until it is saved. */
int ts_proj_read(int32_t slot, void *buf);
int ts_proj_write(int32_t slot, const void *buf);
static u8 slotbuf[512] __attribute__((aligned(16)));

void telos_proj_slot_write(void *fs, int32_t slot, int32_t ctx)
{
    (void)fs; (void)ctx;
    if (slot < 0 || slot > 127)
        return;
    dl_memset(slotbuf, 0, sizeof slotbuf);
    ts_proj_write(slot, slotbuf);
}

void telos_proj_saved(void *block, int32_t slot, int32_t ok)
{
    (void)block;
    if (!ok || slot < 0 || slot > 127)
        return;
    run_build();
    dl_memset(slotbuf, 0, sizeof slotbuf);
    dl_memcpy(slotbuf, &pdata, sizeof pdata);
    ts_proj_write(slot, slotbuf);
}

void telos_proj_loaded(int32_t slot, int32_t err)
{
    int found = 0;
    dl_memset(&pdata, 0, sizeof pdata);
    if (!err && slot >= 0 && slot <= 127 && ts_proj_read(slot, slotbuf) == 0) {
        dl_memcpy(&pdata, slotbuf, sizeof pdata);
        found = pdata.magic == 0x544d3031u && pdata.n <= SET_MAX;
        if (!found)
            dl_memset(&pdata, 0, sizeof pdata);
    }
    telos_pj[3] = (u32)slot;
    proj_loaded(found);
}
#else
const struct core_projdata telos_projdata = { 0x544c4d53u /* "TLMS" */, CORE_PROJDATA_VARIABLE | sizeof pdata,
                                              &pdata, proj_loaded };
#endif

/* telos.c, after the boot's link: the index, and what runs. */
void telos_proj_boot(void)
{
    ix_update();
    run_build();
    boot_load = 1;
    boot_ticks = 0;
}

/* YES on LOAD PROJECT MODS?: the switches, and the project's list kept for the check after the restart. */
static void apply_and_restart(void)
{
    u32 i;
    telos_dir_busy = 1;
    for (i = 0; i < nact; i++) {
        if (act[i].on)
            telos_dir.e[act[i].entry].flags |= TS_ON;
        else
            telos_dir.e[act[i].entry].flags &= (u8)~TS_ON;
    }
    if (telos_dir_write() != 0) {
        telos_dir_busy = 0;
        return;
    }
    telos_dir_busy = 0;
    ixh.pending = proj_n < PEND_MAX ? proj_n : PEND_MAX;
    for (i = 0; i < 2 * ixh.pending; i++)
        ixh.pend[i] = proj_pair[i];
    ix_save();
    core_restart();
}

/* ---- the screens ---------------------------------------------------------------------------------- */
#define SHOWN 4

int telos_proj_key(void *brain, void *ev)
{
    uint32_t key = EV_KEY(ev), fl = EV_FLAGS(ev);
    (void)brain;
    if (mode == M_NONE) {
        if (eat && key == eat) {
            if (!(fl & 1))
                eat = 0;
            return 1;
        }
        return 0;
    }
    if (key == KEY_PLAY || key == KEY_STOP || key == KEY_TEMPO)
        return 0;
    if (!(fl & 1))
        return 1;                        /* releases */
    if (key == KEY_UP || key == KEY_DOWN) {
        if (key == KEY_UP && scroll)
            scroll--;
        if (key == KEY_DOWN && scroll + SHOWN < nlines)
            scroll++;
        dirty = 1;
        return 1;
    }
    if ((fl & 8) || (key != KEY_YES && key != KEY_NO))
        return 1;
    eat = (u8)key;
    dirty = 1;
    if (mode == M_MODS_ASK && key == KEY_YES) {
        apply_and_restart();
        mode = M_NONE;
    } else {
        mode = M_NONE;
    }
    return 1;
}

void telos_proj_draw(void *bmp, void *ctrl)
{
    int i, y;
    (void)ctrl;
    if (mode == M_NONE)
        return;
    fw_fillrect(bmp, 0, 0, 127, 63, 0);
    TEXTF(bmp, FONT_TITLE, 2, 56, -1, "%s", mode == M_MODS_ASK ? "LOAD PROJECT MODS?" : "PROJECT MODS");
    fw_fillrect(bmp, 0, 54, 127, 63, -1);
    for (i = scroll, y = 44; i < nlines && i < scroll + SHOWN; i++, y -= 10)
        TEXTF(bmp, fw_font5, 3, y, -1, "%s", lines[i]);
    if (nlines > SHOWN) {                /* the scroll bar */
        int h = 40 * SHOWN / nlines, p = (40 - h) * scroll / (nlines - SHOWN);
        fw_fillrect(bmp, 126, 51 - p - h, 127, 51 - p, -1);
    }
    TEXTF(bmp, fw_font5, 3, 2, -1, "%s", mode == M_MODS_ASK ? "YES LOAD+RESTART  NO KEEP" : "YES OK");
}

void telos_proj_tick(void *ctrl)
{
#ifdef TELOS_DT1
    if (boot_load && ++boot_ticks > 60) {         /* 2 s at 30 Hz */
#else
    if (boot_load && ++boot_ticks > 300) {
#endif
        boot_load = 0;                   /* no load at boot (a new unit): the next one is the user's */
#ifdef TELOS_DT1
        if (ixh.pending) {               /* the Digitakt's boot loads its working copy without a load job:
                                          * what a restart for a project's mods still has not right */
            check(ixh.pend, ixh.pending, 1);
            ixh.pending = 0;
            ix_save();
        }
#endif
    }
    if (dirty) {
        dirty = 0;
        *((unsigned char *)ctrl + 0x20) = 1;
    }
}
