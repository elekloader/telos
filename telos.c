/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TELOS for the Digitone mk1 (OS 1.43 and 1.44), 0.1: mods kept on the unit's own +Drive, linked at
 * every boot. A build flashed once with TELOS links the mods its store has switched on, with no new
 * firmware; SETTINGS > MODS (TELOS) switches them (row.s, page.c).
 *
 * The store is 2 MB of raw eMMC sectors (telos.h). At core-dn1's ev_card_ready (the init task, the
 * card up, before the project loads and before the UI and MIDI tasks exist) the boot reads the
 * directory, writes the chosen files as an /mods/active text, and links them into the +Drive mods'
 * area (what the flashed build leaves of core's 1.25 MB) with the Rytm's boot linker
 * (vendor/drive-loader/dlink.c, json.c), against the build descriptor core_desc.
 *
 * Memory: the link's staging buffer (one file) and its heap are scratch in the bulk area, used only
 * here, at the join, before any mod runs: the bulk area's contract (the profile) is that the OS never
 * clears it and a mod clears what it reads before writing it, so a pedal that owns this piece finds
 * only what it would find at power-on. What the menu needs after the link is copied out first.
 */
#include "telos.h"

extern char core_desc[];
void dl_flush(void);
extern void core_param_install(void) __attribute__((weak));   /* core-dn1 5.4; 0 on older cores */

#if defined(TELOS_DN2)
/* The Digitone II: RAM no OS instruction names, above its DMA buffers and below core's area (elekloader's
 * devices/digitone_mk2.py: a canary over 0x46710000-0x48000000 held in digikit's emulator). Used only
 * during the link, at ev_card_ready, before any mod runs. */
#define STAGING      ((u8 *)0x47000000)
#define STAGING_SIZE 0x80000u            /* 512 KB: the largest file one boot reads */
#define HEAP         ((u8 *)0x47080000)
#define HEAP_SIZE    0x100000u           /* 1 MB: the linker's pools and records */
#define CRASH_TICKS  300                 /* ev_tick: about 30 times a second, as on the mk1 (unmeasured) */
#elif defined(TELOS_DT2)
/* The Digitakt II has no bulk area and no sample pool in its DDR: the scratch is RAM no OS instruction
 * names, between the DSP buffers and the boot stack (elekloader's devices/digitakt_mk2.py): the staging
 * buffer in the profile's ddr-low area, the heap between core's area and the boot stack's top 128 KB.
 * Used only during the link, at ev_fs_ready, before any mod runs. */
#define STAGING      ((u8 *)0x47E80000)
#define STAGING_SIZE 0x80000u            /* 512 KB: the largest file one boot reads */
#define HEAP         ((u8 *)0x47F40000)
#define HEAP_SIZE    0xA0000u            /* 640 KB: the linker's pools and records */
#define CRASH_TICKS  300                 /* ev_tick: about 30 times a second, as on the mk1 (unmeasured) */
#elif defined(TELOS_DT1)
/* The Digitakt has no bulk area: the scratch is the sample pool's top, above core's stock copy
 * (0x47800000, about 2.4 MB), below the pool's end (0x47BAF640; 1.54 0x47BBF640). Nothing is in the
 * pool before the first sample loads, after ev_fs_ready. */
#define STAGING      ((u8 *)0x47A60000)
#define STAGING_SIZE 0x80000u            /* 512 KB: the largest file one boot reads */
#define HEAP         ((u8 *)0x47AE0000)
#define HEAP_SIZE    0xC0000u            /* 768 KB: the linker's pools and records */
#define CRASH_TICKS  300                 /* ev_tick runs 30 times a second: 10 s */
#else
#define STAGING      ((u8 *)0x44000000)  /* the bulk area's start: scratch at the join (above) */
#define STAGING_SIZE 0x80000u            /* 512 KB: the largest file one boot reads */
#define HEAP         ((u8 *)0x44080000)
#define HEAP_SIZE    0x100000u           /* 1 MB: the linker's pools and records */
#define CRASH_TICKS  200                 /* ev_tick runs about 20 times a second: 10 s */
#endif

#ifndef TELOS_DT1
/* The Digitone's store: raw eMMC sectors from TS_SECTOR. */
int ts_read(u32 sector, u32 bytes, void *buf) { return fw_card_read(TS_SECTOR + sector, bytes, buf); }
int ts_write(u32 sector, u32 bytes, const void *buf) { return fw_card_write(TS_SECTOR + sector, bytes, buf); }
int ts_create(void) { return 0; }
#endif

struct ts_dir telos_dir;
struct ts_boot telos_boot_st[TS_MAX];
u32 telos_boot_flags[2];
volatile u32 telos_state[8];
int telos_dir_ok;
struct dl_choice *dl_boot_choice;

/* ---- the linker's host -------------------------------------------------------------------------- */
static u32 heap_used;

void *dl_alloc(u32 n)
{
    u8 *p;
    n = (n + 15) & ~15u;
    if (heap_used + n > HEAP_SIZE)
        return 0;
    p = HEAP + heap_used;
    heap_used += n;
    dl_memset(p, 0, n);
    return p;
}

void dl_free(void *p) { (void)p; }       /* the bump heap is dropped after the link */
void dl_breathe(void) {}                 /* the init task: nothing below it to let in */

/* GCC calls memcpy for dlink.c's struct copies; core-dn1 exports fw_memcpy only. */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *d, const void *s, unsigned long n)
{
    u8 *a = d;
    const u8 *b = s;
    while (n--)
        *a++ = *b++;
    return d;
}

static int find(const char *name)
{
    int i;
    for (i = 0; i < telos_dir.n && i < TS_MAX; i++)
        if (!dl_strcmp(telos_dir.e[i].name, name))
            return i;
    return -1;
}

s32 dl_read_file(const char *name, u8 *buf, u32 max)
{
    int i = find(name);
    const struct ts_entry *e;
    if (i < 0)
        return -1;
    e = &telos_dir.e[i];
    if (e->size > max)
        return -2;
    if (e->sector < TS_DATA || e->sector + (e->size + 511) / 512 > TS_SECTORS)
        return -3;
    {
        u32 at, k;                       /* 16 KB at a time: a large file (DigiTide's 131 KB) read in one
                                          * call went past 64 KB, which the card layer splits by recursion,
                                          * and in digiemu's cold boot never finished in time */
        for (at = 0; at < e->size; at += k) {
            k = e->size - at < 0x4000u ? e->size - at : 0x4000u;
            if (ts_read(e->sector + at / 512, k, buf + at) != 0)
                return -3;
        }
    }
    return (s32)e->size;
}

#ifdef DL_SITES
/* ---- sites (POC): mods that write the OS's code, as the Octatrack's TELOS ----------------------------
 * The linker (vendor/drive-loader, DL_SITES) checks each site's bytes against stock's (sha256), refuses
 * two mods' overlapping sites and writes them between dl_commit_begin and dl_commit_end. At ev_card_ready
 * the init task runs and the UI, MIDI and project tasks do not exist yet; the commit masks interrupts and
 * writes once no task is stopped inside a site (dl_commit_begin, below). Stock bytes come from RAM as
 * the OS has them at the join, with the descriptor's shadow over the ranges the flashed build wrote. */
extern char IMAGE_END[];                 /* mod.json's defsym: the main OS image's end, per OS version */
#define IMAGE_LO 0x40000400u

static const u32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))

static void sha_block(u32 *h, const u8 *p)
{
    u32 w[64], a, b, c, d, e, g, hh, ff, i;
    for (i = 0; i < 16; i++)
        w[i] = dl_rd32(p + 4 * i);
    for (; i < 64; i++) {
        u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; ff = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 64; i++) {
        u32 t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & ff) ^ (~e & g)) + K[i] + w[i];
        u32 t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = ff; ff = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += ff; h[6] += g; h[7] += hh;
}

void dl_sha256(const u8 *p, u32 n, u8 out[32])
{
    static const u32 h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
                              0x1f83d9ab, 0x5be0cd19};
    u32 h[8];
    u8 blk[64];
    u32 i, done = 0, r;
    for (i = 0; i < 8; i++)             /* word by word: a struct copy would call memcpy */
        h[i] = h0[i];
    while (n - done >= 64) {
        sha_block(h, p + done);
        done += 64;
    }
    r = n - done;
    dl_memset(blk, 0, 64);
    dl_memcpy(blk, p + done, r);
    blk[r] = 0x80;
    if (r >= 56) {
        sha_block(h, blk);
        dl_memset(blk, 0, 64);
    }
    dl_wr32(blk + 56, 0);
    dl_wr32(blk + 60, n << 3);
    sha_block(h, blk);
    for (i = 0; i < 8; i++)
        dl_wr32(out + 4 * i, h[i]);
}

int dl_site_ok(u32 addr, u32 len) { return addr >= IMAGE_LO && addr + len <= (u32)IMAGE_END; }

/* A mod's region must lie inside one of the device's free areas, as a flashed mod's must (elekloader's
 * devices/digitone_mk1.py, areas: the same on 1.43 and 1.44). The linker's scratch at 0x44000000 and
 * core's stock copy at 0x44200000 are only used during the link, before any mod runs. */
int dl_region_ok(u32 lo, u32 hi)
{
#if defined(TELOS_DN2)
    return lo >= 0x47F00000u && hi <= 0x47F40000u;          /* ddr: core's 256 KB */
#elif defined(TELOS_DT2)
    return (lo >= 0x47E7B000u && hi <= 0x47F00000u)       /* ddr-low */
        || (lo >= 0x47F00000u && hi <= 0x47F40000u);      /* ddr: core's 256 KB */
#elif defined(TELOS_DT1)
    return lo >= 0x47BC0000u && hi <= 0x47C00000u;          /* ddr: core's 256 KB (no bulk area) */
#else
    return (lo >= 0x44000000u && hi <= 0x47be0000u)       /* bulk */
        || (lo >= 0x47be0000u && hi <= 0x47c00000u);      /* ddr */
#endif
}

/* Stock bytes: core-dn1 5.4 copies the OS image before the OS runs (core_stock_info: where, bytes, from
 * 0x40000400); the OS changes some of its data before the join, so the copy is read where it covers. On
 * an older core, RAM as it is at the join. The descriptor's shadow goes over either (dlink.c). */
#ifdef TELOS_MK2                         /* core-dt2 and core-dn2 keep no copy of the OS */
void dl_stock_read(u8 *dst, u32 a, u32 n)
{
    dl_memcpy(dst, (const void *)a, n);
}
#else
extern const struct core_stock_info core_stock_info __attribute__((weak));   /* core3.h */

void dl_stock_read(u8 *dst, u32 a, u32 n)
{
    if (&core_stock_info && a >= IMAGE_LO && a + n <= IMAGE_LO + core_stock_info.len)
        dl_memcpy(dst, (const void *)(core_stock_info.at + (a - IMAGE_LO)), n);
    else
        dl_memcpy(dst, (const void *)a, n);
}
#endif
void dl_site_saving(u32 addr, u32 len) { (void)addr; (void)len; }

u32 telos_irq_off(void);
void telos_irq_restore(u32 sr);
u32 telos_sp(void);
static u32 commit_sr;
volatile u32 telos_sites_written;
volatile u32 telos_commit_tries;         /* how many tries the last commit took (50 + 1: none quiet) */

/* ---- quiescence: no task may be stopped inside a site ----------------------------------------------
 * The OS's ten tasks are static, with fixed TCBs and stacks: core-dn1 5.4 lists them (core_tasks: the
 * count, the init task's row, the TCB's saved-SP offset, then TCB, stack and size per task). A site may
 * be written only when no live stack word points strictly inside one: a task stopped there, or a return
 * address into its middle. A task not created yet has a saved SP outside its stack and is skipped; the
 * init task (the one TELOS runs in) is scanned from its live SP. Without core_tasks (an older core)
 * nothing tells, so no site is written (26). */
extern const struct core_tasks core_tasks __attribute__((weak));   /* core3.h */

static int inside(u32 v)
{
    u32 i, a, n;
    for (i = 0; i < dl_sites_n(); i++)
        if (dl_site_live(i, &a, &n) && v > a && v < a + n)
            return 1;
    return 0;
}

static int stack_busy(u32 lo, u32 hi)
{
    for (lo &= ~1u; lo + 4 <= hi; lo += 2)
        if (inside(*(volatile u32 *)lo))
            return 1;
    return 0;
}

int dl_commit_begin(void)
{
    const struct core_tasks *k = &core_tasks;
    const struct core_task *t;
    u32 tries, i;
    if (!k) {
        telos_commit_tries = 0;
        return 0;
    }
    t = k->t;
    for (tries = 0; tries < 50; tries++) {
        int busy = 0;
        commit_sr = telos_irq_off();
        for (i = 0; i < k->n && !busy; i++) {
            u32 lo, hi = t[i].stack + t[i].size;
            lo = i == k->init ? telos_sp() : *(volatile u32 *)(t[i].tcb + k->sp_at);
            if (lo < t[i].stack || lo >= hi)
                continue;                /* not created yet, or not on its own stack */
            busy = stack_busy(lo, hi);
        }
        telos_commit_tries = tries + 1;
        if (!busy)
            return 1;                    /* interrupts stay masked until dl_commit_end */
        telos_irq_restore(commit_sr);
        for (i = 0; i < 200000; i++)     /* let the other tasks move on */
            __asm__ volatile("nop");
    }
    telos_commit_tries = 51;
    return 0;
}

void dl_commit_end(void)
{
    u32 i, a, n;
    for (i = 0; i < dl_sites_n(); i++)
        if (dl_site_live(i, &a, &n))
            telos_sites_written++;
    dl_flush();
    telos_irq_restore(commit_sr);
}
#endif

/* ---- the directory ------------------------------------------------------------------------------- */
static u8 dirbuf[TS_DIR_BYTES] __attribute__((aligned(16)));
static u8 cur_copy;                      /* the copy telos_dir came from */

static int dir_valid(const struct ts_dir *d)
{
    u32 c;
    if (d->magic[0] != 'T' || d->magic[1] != 'E' || d->magic[2] != 'L' || d->magic[3] != 'S')
        return 0;
    if (d->version != TS_VERSION || d->n > TS_MAX || d->sectors != TS_SECTORS)
        return 0;
    c = d->crc;
    ((struct ts_dir *)d)->crc = 0;
    if (dl_crc32((const u8 *)d, TS_DIR_BYTES) != c) {
        ((struct ts_dir *)d)->crc = c;
        return 0;
    }
    ((struct ts_dir *)d)->crc = c;
    return 1;
}

static int dir_read(void)
{
    int k, ok = 0;
    for (k = 0; k < 2; k++) {
        if (ts_read((k ? TS_DIR_B : TS_DIR_A), TS_DIR_BYTES, dirbuf) != 0)
            continue;
        if (!dir_valid((const struct ts_dir *)dirbuf))
            continue;
        if (ok && ((const struct ts_dir *)dirbuf)->seq <= telos_dir.seq)
            continue;
        dl_memcpy(&telos_dir, dirbuf, TS_DIR_BYTES);
        cur_copy = (u8)k;
        ok = 1;
    }
    return ok;
}

int telos_dir_write(void)
{
    u8 to = cur_copy ^ 1;
    int r;
    telos_dir.seq++;
    telos_dir.crc = 0;
    telos_dir.crc = dl_crc32((const u8 *)&telos_dir, TS_DIR_BYTES);
    dl_memcpy(dirbuf, &telos_dir, TS_DIR_BYTES);
    r = ts_write((to ? TS_DIR_B : TS_DIR_A), TS_DIR_BYTES, dirbuf);
    if (r == 0)
        cur_copy = to;
    return r;
}

/* ---- the crash guard -------------------------------------------------------------------------------
 * "ARMD" in its sector while a boot that linked mods has not had 10 s of ticks: a boot that finds it
 * links nothing (and clears it), so a mod that crashes or hangs the unit early costs one restart.
 * A crash in those 10 s restarts at once (ev_crash), into that safe boot. */
static u8 guard[512] __attribute__((aligned(16)));
static u8 armed;
static u32 armed_ticks;

static int guard_read(void)
{
    if (ts_read(TS_ARMED, 512, guard) != 0)
        return 0;
    return guard[0] == 'A' && guard[1] == 'R' && guard[2] == 'M' && guard[3] == 'D';
}

static void guard_write(int on)
{
    dl_memset(guard, 0, sizeof guard);
    if (on) {
        guard[0] = 'A'; guard[1] = 'R'; guard[2] = 'M'; guard[3] = 'D';
    }
    ts_write(TS_ARMED, 512, guard);
}

void telos_page_tick(void *ctrl);

void telos_tick(void *ctrl)
{
    telos_page_tick(ctrl);               /* page.c's redraws */
    if (!armed || ++armed_ticks < CRASH_TICKS)
        return;
    armed = 0;
    guard_write(0);
}

void telos_crash(void *frame)
{
    (void)frame;
    if (armed)
        core_restart_now();              /* the next boot finds the guard and links nothing */
}

/* ---- the boot ------------------------------------------------------------------------------------- */
static char active[TS_MAX * (32 + 10) + 1];

static u32 put_hex(char *p, u32 v)
{
    int k;
    for (k = 0; k < 8; k++)
        p[k] = "0123456789abcdef"[(v >> (28 - 4 * k)) & 15];
    return 8;
}

static void boot(void)
{
    u32 len = 0, i;
    int n, loaded, k;
    signed char line_of[TS_MAX];                 /* active's line for each entry, or -1 */
    struct dl_choice *choice;

    telos_state[0] = 1;
    if (!dir_read()) {
        telos_state[0] = 4;              /* no store: nothing to link */
        return;
    }
    telos_dir_ok = 1;
    telos_state[4] = telos_dir.n;
    if (guard_read()) {
        guard_write(0);
        dl_skip_reason = DL_SKIP_ARMED;
        telos_state[3] = DL_SKIP_ARMED;
        telos_state[0] = 3;
        return;
    }
    n = 0;
    for (i = 0; i < telos_dir.n; i++) {
        const struct ts_entry *e = &telos_dir.e[i];
        line_of[i] = -1;
        if (!(e->flags & TS_ON))
            continue;
        telos_boot_flags[i >> 5] |= 1u << (i & 31);
        line_of[i] = (signed char)n++;
        for (k = 0; e->name[k] && k < 31; k++)
            active[len++] = e->name[k];
        active[len++] = ' ';
        len += put_hex(active + len, e->crc);
        active[len++] = '\n';
    }
    telos_state[2] = (u32)n;
    if (!n) {
        telos_state[0] = 2;
        return;
    }
    heap_used = 0;
    choice = dl_alloc((u32)n * sizeof *choice);
    if (!choice || dl_parse_active(active, len, choice) != n) {
        telos_state[3] = DL_SKIP_MEMORY;
        return;
    }
    guard_write(1);                      /* before the link: a link that faults is caught too */
    loaded = dl_link((u32)core_desc, choice, n, STAGING, STAGING_SIZE);
    dl_flush();
    telos_state[1] = (u32)(loaded < 0 ? 0 : loaded);
    telos_state[3] = dl_skip_reason;
    for (i = 0; i < telos_dir.n; i++) {  /* the records, out of the scratch heap */
        int l = line_of[i];
        if (l < 0 || !dl_status || (u32)l >= dl_status_n)
            continue;
        telos_boot_st[i].state = dl_status[l].state;
        telos_boot_st[i].code = dl_status[l].code;
        if (dl_detail)
            for (k = 0; k < 29 && dl_detail[l][k]; k++)
                telos_boot_st[i].detail[k] = dl_detail[l][k];
    }
    telos_state[0] = 2;
    if (loaded <= 0) {
        guard_write(0);
        return;
    }
    armed = 1;
    armed_ticks = 0;
#ifndef TELOS_DT1                        /* the Digitakt's core has no core_param_install: its weak import
                                          * would resolve to core_zero, which is not 0, and be called */
    if (core_param_install)              /* core-dn1 5.4: the linked mods' parameter slots */
        core_param_install();
#endif
    for (i = 0; i < dl_inits_n; i++)
        if (dl_inits[i])
            ((void (*)(void))dl_inits[i])();
}

void telos_proj_boot(void);

#ifdef TELOS_DT1
/* The Digitakt: the store is a file, so the boot waits for the file system (core 4.5's ev_fs_ready). */
void telos_fs_ready(int32_t mounted)
{
    if (!mounted) {
        dl_skip_reason = DL_SKIP_DRIVE;
        telos_state[3] = DL_SKIP_DRIVE;
        return;
    }
    boot();
    telos_proj_boot();
}
#endif

void telos_card_ready(int32_t status)
{
    if (status != 0) {
        dl_skip_reason = DL_SKIP_DRIVE;
        telos_state[3] = DL_SKIP_DRIVE;
        return;
    }
    boot();
    telos_proj_boot();                   /* projset.c: the index, and what runs, before the project loads */
}
