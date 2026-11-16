/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TELOS's transfer: the store over SysEx (USB or DIN), for tools/telos.py's `unit` commands.
 *
 *   request  F0 7D 54 4C <cmd> <seq> <body, Elektron's 7-bit groups> F7
 *   reply    F0 7D 54 4C <cmd | 0x40> <seq> <status, then the reply's body, 7-bit groups> F7
 *
 * 0x7D is MIDI's non-commercial id, "TL" TELOS's (digihealth's channel is F0 00 20 3C 7D, apart). The
 * MIDI task (core-dn1's ev_sysex, priority 7) carries each request out and answers it: it is above the
 * UI task, so nothing the MODS page does can run in the middle of one. The page's own store writes run
 * under telos_dir_busy, and a request that comes meanwhile is answered BUSY (the host asks again).
 * The MIDI task waits for the card while it writes: other MIDI input waits that long (a sector a
 * DATA). One request at a time: the host waits for each reply. RESTART is left to the UI task's next
 * tick, so the reply leaves first.
 *
 * Commands (all words big-endian; a name is 32 bytes, a title 16, NUL-padded):
 *   01 INFO     -> u8 protocol 1, u8 store (1: there is one), u16 entries, u16 most, u32 the store's
 *                  sectors, u32 free sectors, u32 the largest free run, 8 bytes TELOS's version
 *   02 ENTRY    u8 index -> u8 flags, u8 boot state, u8 boot code, u8 0, u32 size, u32 CRC, name, title
 *   03 BEGIN    u32 size, u32 CRC, u8 on, name, title: room for a file (the first free run that holds it)
 *   04 DATA     u32 offset, up to 512 bytes: the file from offset, which is where the last DATA ended
 *   05 END      read back, CRC checked, then the directory gets the entry
 *   06 SET      u8 on, name
 *   07 DELETE   name
 *   08 RESTART  the reply, then core_restart
 *   09 ABORT    drops an upload in progress
 *   0A FORMAT   an empty directory in both copies (the files' sectors are left as they are)
 */
#include "telos.h"

#define PROTO       1
#define CHUNK       512
#define REQ_MAX     (CHUNK + 64)

enum { C_INFO = 1, C_ENTRY, C_BEGIN, C_DATA, C_END, C_SET, C_DELETE, C_RESTART, C_ABORT, C_FORMAT };
enum { S_OK, S_BUSY, S_BAD, S_NO_STORE, S_EXISTS, S_DIR_FULL, S_NO_ROOM, S_ORDER, S_CRC, S_WRITE,
       S_NOT_FOUND, S_NO_UPLOAD };

static const uint8_t HDR[4] = {0xF0, 0x7D, 0x54, 0x4C};

volatile uint8_t telos_dir_busy;         /* page.c: a store write of its own in progress */
static uint8_t req_cmd, req_seq;
static int32_t req_port, req_len;
static uint8_t req[REQ_MAX];

/* an upload in progress */
static struct {
    uint8_t on, active;
    char name[32], title[16];
    u32 size, crc, sector, next;
} up;

static uint8_t buf[4096] __attribute__((aligned(16)));
static uint8_t out[16 + 2 * 96];

void telos_page_changed(void);           /* page.c: the store changed under the page */
int telos_dir_write(void);

static u32 rd32(const uint8_t *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void wr32(uint8_t *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* ---- the MIDI task ---------------------------------------------------------------------------------- */
static int32_t unpack7(const uint8_t *in, int32_t n, uint8_t *o, int32_t max)
{
    int32_t k = 0, i, j;
    for (i = 0; i < n; i += 8) {
        uint8_t hi = in[i];
        for (j = 1; j < 8 && i + j < n; j++) {
            if (k >= max)
                return -1;
            o[k++] = (uint8_t)(in[i + j] | ((hi & (0x40 >> (j - 1))) ? 0x80 : 0));
        }
    }
    return k;
}

static void send(uint8_t cmd, uint8_t seq, uint8_t status, const uint8_t *body, int32_t n, int32_t port);
static void process(void);

int32_t telos_sysex(const uint8_t *msg, int32_t len, int32_t source)
{
    int32_t n, port = source == 2 || source == 4 ? 2 : 3;   /* answer where it came from */
    if (len < 7 || msg[0] != HDR[0] || msg[1] != HDR[1] || msg[2] != HDR[2] || msg[3] != HDR[3])
        return 0;
    if (msg[len - 1] != 0xF7)
        return 1;
    if (telos_dir_busy) {
        send(msg[4], msg[5], S_BUSY, 0, 0, port);
        return 1;
    }
    n = unpack7(msg + 6, len - 7, req, REQ_MAX);
    if (n < 0) {
        send(msg[4], msg[5], S_BAD, 0, 0, port);
        return 1;
    }
    req_cmd = msg[4];
    req_seq = msg[5];
    req_len = n;
    req_port = port;
    process();
    return 1;
}

/* ---- replies ----------------------------------------------------------------------------------------- */
static void send(uint8_t cmd, uint8_t seq, uint8_t status, const uint8_t *body, int32_t n, int32_t port)
{
    uint8_t plain[1 + 96];
    int32_t k = 0, i, j, m = n + 1;
    if (n > 96)
        return;
    plain[0] = status;
    for (i = 0; i < n; i++)
        plain[1 + i] = body[i];
    for (i = 0; i < 4; i++)
        out[k++] = HDR[i];
    out[k++] = (uint8_t)((cmd & 0x3F) | 0x40);
    out[k++] = seq & 0x7F;
    for (i = 0; i < m; i += 7) {         /* Elektron's 7-bit groups */
        uint8_t hi = 0;
        for (j = 0; j < 7 && i + j < m; j++)
            if (plain[i + j] & 0x80)
                hi |= (uint8_t)(0x40 >> j);
        out[k++] = hi;
        for (j = 0; j < 7 && i + j < m; j++)
            out[k++] = plain[i + j] & 0x7F;
    }
    out[k++] = 0xF7;
    fw_sysex_send(out, k, 0, port);
}

/* ---- the store's free space ----------------------------------------------------------------------- */
static u32 secs(u32 bytes) { return (bytes + 511) / 512; }

/* The first free run of `need` sectors from TS_DATA, or 0; *largest and *total for INFO. */
static u32 find_room(u32 need, u32 *largest, u32 *total)
{
    u32 at = TS_DATA, best = 0, sum = 0, found = 0;
    for (;;) {
        u32 next = TS_SECTORS, end_of = 0;
        int i;
        for (i = 0; i < telos_dir.n; i++) {     /* the entry that starts first at or after `at` */
            const struct ts_entry *e = &telos_dir.e[i];
            if (e->sector >= at && e->sector < next) {
                next = e->sector;
                end_of = e->sector + secs(e->size);
            }
        }
        if (next - at > best)
            best = next - at;
        sum += next - at;
        if (!found && need && next - at >= need)
            found = at;
        if (next == TS_SECTORS)
            break;
        at = end_of > at ? end_of : at + 1;
    }
    if (largest)
        *largest = best;
    if (total)
        *total = sum;
    return found;
}

static int find(const char *name)
{
    int i;
    for (i = 0; i < telos_dir.n; i++)
        if (!dl_strcmp(telos_dir.e[i].name, name))
            return i;
    return -1;
}

/* Drop entry i, and its boot records with it (they are by directory index). */
static void drop(int i)
{
    int k;
    for (k = i; k + 1 < telos_dir.n; k++) {
        telos_dir.e[k] = telos_dir.e[k + 1];
        telos_boot_st[k] = telos_boot_st[k + 1];
        if (telos_boot_flags[(k + 1) >> 5] >> ((k + 1) & 31) & 1)
            telos_boot_flags[k >> 5] |= 1u << (k & 31);
        else
            telos_boot_flags[k >> 5] &= ~(1u << (k & 31));
    }
    telos_dir.n--;
    dl_memset(&telos_dir.e[telos_dir.n], 0, sizeof telos_dir.e[0]);
    dl_memset(&telos_boot_st[telos_dir.n], 0, sizeof telos_boot_st[0]);
    telos_boot_flags[telos_dir.n >> 5] &= ~(1u << (telos_dir.n & 31));
}

static void copy_name(char *d, const uint8_t *s, int n)
{
    int k;
    for (k = 0; k < n - 1 && s[k]; k++)
        d[k] = (char)s[k];
    for (; k < n; k++)
        d[k] = 0;
}

static uint8_t format(void)
{
    u32 seq = telos_dir_ok ? telos_dir.seq : 0;
    dl_memset(&telos_dir, 0, sizeof telos_dir);
    telos_dir.magic[0] = 'T'; telos_dir.magic[1] = 'E'; telos_dir.magic[2] = 'L'; telos_dir.magic[3] = 'S';
    telos_dir.version = TS_VERSION;
    telos_dir.sectors = TS_SECTORS;
    telos_dir.data = TS_DATA;
    telos_dir.seq = seq;
    if (ts_create() != 0)                /* the Digitakt's store file */
        return S_WRITE;
    dl_memset(telos_boot_st, 0, sizeof telos_boot_st[0] * TS_MAX);
    telos_boot_flags[0] = telos_boot_flags[1] = 0;
    if (telos_dir_write() != 0 || telos_dir_write() != 0)  /* both copies */
        return S_WRITE;
    telos_dir_ok = 1;
    return S_OK;
}

/* ---- the UI task: RESTART's restart, a tick after its reply ------------------------------------------ */
static volatile uint8_t restarting;

void telos_upload_tick(void)
{
    if (restarting)
        core_restart();
}

/* ---- a request, in the MIDI task -------------------------------------------------------------------- */
static void restart_after(void) { restarting = 1; }

static void process(void)
{
    uint8_t body[96], st = S_OK;
    int32_t n = 0, i;
    switch (req_cmd) {
    case C_INFO: {
        u32 largest = 0, total = 0;
        if (telos_dir_ok)
            find_room(0, &largest, &total);
        body[0] = PROTO;
        body[1] = (uint8_t)telos_dir_ok;
        body[2] = 0; body[3] = telos_dir_ok ? (uint8_t)telos_dir.n : 0;
        body[4] = 0; body[5] = TS_MAX;
        wr32(body + 6, TS_SECTORS);
        wr32(body + 10, total);
        wr32(body + 14, largest);
        dl_memset(body + 18, 0, 8);
        body[18] = '0'; body[19] = '.'; body[20] = '2';
        n = 26;
        break;
    }
    case C_ENTRY: {
        const struct ts_entry *e;
        if (!telos_dir_ok) { st = S_NO_STORE; break; }
        if (req_len < 1 || req[0] >= telos_dir.n) { st = S_NOT_FOUND; break; }
        i = req[0];
        e = &telos_dir.e[i];
        body[0] = e->flags;
        body[1] = (telos_boot_flags[i >> 5] >> (i & 31) & 1) ? telos_boot_st[i].state : 0;
        body[2] = telos_boot_st[i].code;
        body[3] = 0;
        wr32(body + 4, e->size);
        wr32(body + 8, e->crc);
        dl_memcpy(body + 12, e->name, 32);
        dl_memcpy(body + 44, e->title, 16);
        n = 60;
        break;
    }
    case C_BEGIN: {
        char name[32];
        u32 size, at;
        if (!telos_dir_ok) { st = S_NO_STORE; break; }
        if (req_len < 9 + 32 + 16) { st = S_BAD; break; }
        size = rd32(req);
        copy_name(name, req + 9, 32);
        if (!name[0] || !size) { st = S_BAD; break; }
        if (find(name) >= 0) { st = S_EXISTS; break; }
        if (telos_dir.n >= TS_MAX) { st = S_DIR_FULL; break; }
        at = find_room(secs(size), 0, 0);
        if (!at) { st = S_NO_ROOM; break; }
        dl_memcpy(up.name, name, 32);
        copy_name(up.title, req + 41, 16);
        up.size = size;
        up.crc = rd32(req + 4);
        up.on = req[8] != 0;
        up.sector = at;
        up.next = 0;
        up.active = 1;
        break;
    }
    case C_DATA: {
        u32 off, len;
        if (!up.active) { st = S_NO_UPLOAD; break; }
        if (req_len < 5) { st = S_BAD; break; }
        off = rd32(req);
        len = (u32)req_len - 4;
        if (off != up.next || off % CHUNK || len > CHUNK || off + len > up.size
            || (len < CHUNK && off + len != up.size)) { st = S_ORDER; break; }
        dl_memcpy(buf, req + 4, len);
        if (len < CHUNK)
            dl_memset(buf + len, 0, CHUNK - len);
        if (ts_write(up.sector + off / 512, CHUNK, buf) != 0) { st = S_WRITE; break; }
        up.next = off + len;
        wr32(body, up.next);
        n = 4;
        break;
    }
    case C_END: {
        u32 c = 0xFFFFFFFFu, at;
        struct ts_entry *e;
        if (!up.active) { st = S_NO_UPLOAD; break; }
        if (up.next != up.size) { st = S_ORDER; break; }
        for (at = 0; at < up.size; at += sizeof buf) {     /* read back what the card has */
            u32 k = up.size - at < sizeof buf ? up.size - at : sizeof buf;
            if (ts_read(up.sector + at / 512, k, buf) != 0) { st = S_WRITE; break; }
            c = dl_crc32_add(c, buf, k);
        }
        if (st)
            break;
        if (~c != up.crc) { st = S_CRC; up.active = 0; break; }
        if (telos_dir.n >= TS_MAX) { st = S_DIR_FULL; break; }
        i = telos_dir.n;
        e = &telos_dir.e[i];
        dl_memset(e, 0, sizeof *e);
        dl_memcpy(e->name, up.name, 32);
        dl_memcpy(e->title, up.title, 16);
        e->sector = up.sector;
        e->size = up.size;
        e->crc = up.crc;
        e->flags = up.on ? TS_ON : 0;
        dl_memset(&telos_boot_st[i], 0, sizeof telos_boot_st[i]);
        telos_boot_flags[i >> 5] &= ~(1u << (i & 31));
        telos_dir.n++;
        if (telos_dir_write() != 0) {
            telos_dir.n--;
            st = S_WRITE;
            break;
        }
        up.active = 0;
        telos_page_changed();
        break;
    }
    case C_SET:
    case C_DELETE: {
        char name[32];
        int on = req_cmd == C_SET ? req[0] : 0;
        uint8_t was;
        if (!telos_dir_ok) { st = S_NO_STORE; break; }
        if (req_len < (req_cmd == C_SET ? 33 : 32)) { st = S_BAD; break; }
        copy_name(name, req + (req_cmd == C_SET), 32);
        i = find(name);
        if (i < 0) { st = S_NOT_FOUND; break; }
        if (req_cmd == C_SET) {
            was = telos_dir.e[i].flags;
            telos_dir.e[i].flags = on ? (was | TS_ON) : (was & ~TS_ON);
            if (telos_dir_write() != 0) {
                telos_dir.e[i].flags = was;
                st = S_WRITE;
            }
        } else {
            struct ts_dir keep;          /* as it was, if the write fails */
            struct ts_boot keep_st[TS_MAX];
            u32 keep_fl[2] = {telos_boot_flags[0], telos_boot_flags[1]};
            dl_memcpy(&keep, &telos_dir, sizeof keep);
            dl_memcpy(keep_st, telos_boot_st, sizeof keep_st);
            drop(i);
            if (telos_dir_write() != 0) {
                u32 seq = telos_dir.seq;
                dl_memcpy(&telos_dir, &keep, sizeof keep);
                telos_dir.seq = seq;
                dl_memcpy(telos_boot_st, keep_st, sizeof keep_st);
                telos_boot_flags[0] = keep_fl[0];
                telos_boot_flags[1] = keep_fl[1];
                st = S_WRITE;
            }
        }
        telos_page_changed();
        break;
    }
    case C_RESTART:
        restart_after();
        break;
    case C_ABORT:
        up.active = 0;
        break;
    case C_FORMAT:
        up.active = 0;
        st = format();
        telos_page_changed();
        break;
    default:
        st = S_BAD;
    }
    send(req_cmd, req_seq, st, body, st ? 0 : n, req_port);
}
