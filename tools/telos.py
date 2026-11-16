# SPDX-License-Identifier: GPL-2.0-or-later
"""TELOS's store from the computer: on a unit over USB MIDI SysEx (upload.c has the protocol), or in a
card image (an emulator's plusdrive.img), in the layout telos.h describes.

    python tools/telos.py unit [--port NAME] info|list|add|on|off|remove|clear|format|restart
    python tools/telos.py CARD list
    python tools/telos.py CARD add MOD.elemod [MOD.elemod ...] [--off]
    python tools/telos.py CARD on NAME | off NAME | remove NAME
    python tools/telos.py CARD clear

It needs elekloader importable (a checkout on PYTHONPATH, or installed) for its MIDI ports and SysEx
packing.

The store is STORE (first sector, sectors): two directory copies of
4096 bytes (sectors +0 and +8), the crash guard's sector (+16) and the files from sector +32, each
from a sector of its own. A directory: "TELS", u16 version 1, u16 entries, u32 sequence, u32 CRC-32
of the 4096 bytes with this word zero, u32 the store's sectors, u32 the first file sector, 8 zero
bytes, then 63 entries of 64 bytes (name 32, title 16, u32 sector, u32 size, u32 CRC-32, u8 flags
(bit 0 on), 3 zero). The unit reads the valid copy with the higher sequence; a write goes to the
other copy. Every change here rewrites the files packed from sector +32 and both copies.
"""
import argparse
import json
import os
import struct
import sys
import time
import zlib

from elekloader.elemod import ModError

SECTOR = 512
DIR_BYTES = 4096
DIR_A, DIR_B, ARMED, DATA = 0, 8, 16, 32
MAX = 63
VERSION = 1
ON = 1
ENTRY = struct.Struct('>32s16sIIIB3x')
HEAD = struct.Struct('>4sHHIIII8x')


# The store's place on each device TELOS runs on: 2 MB of the Digitone mk1's eMMC that no stock code
# reaches (README, "The store"). telos.h's TS_SECTOR and TS_SECTORS are the same.
# The Digitakt mk1's is a file on its +Drive (/telos/store, digitakt/store.c): over USB only.
STORE = {'digitone-mk1': (0x380000, 0x1000), 'digitakt-mk1': (None, 0x1000)}


class _Dev:
    def __init__(self, key):
        self.key = key


def store_of(device_key='digitone-mk1'):
    if device_key not in STORE:
        raise ModError('TELOS has no store on %s' % device_key)
    return _Dev(device_key), STORE[device_key]


def _cstr(b):
    return b.split(b'\0', 1)[0].decode('utf-8', 'replace')


def parse_dir(raw, sectors):
    """One directory copy -> (seq, entries) or None when it is not a valid one."""
    if len(raw) != DIR_BYTES:
        return None
    magic, ver, n, seq, crc, secs, data = HEAD.unpack_from(raw)
    if magic != b'TELS' or ver != VERSION or n > MAX or secs != sectors:
        return None
    if zlib.crc32(raw[:12] + b'\0\0\0\0' + raw[16:]) != crc:
        return None
    out = []
    for i in range(n):
        name, title, sec, size, c, flags = ENTRY.unpack_from(raw, 32 + 64 * i)
        out.append({'name': _cstr(name), 'title': _cstr(title), 'sector': sec, 'size': size,
                    'crc': c, 'on': bool(flags & ON)})
    return seq, out


def build_dir(entries, seq, sectors):
    body = bytearray(DIR_BYTES)
    HEAD.pack_into(body, 0, b'TELS', VERSION, len(entries), seq, 0, sectors, DATA)
    for i, e in enumerate(entries):
        ENTRY.pack_into(body, 32 + 64 * i, e['name'].encode()[:31], e['title'].encode()[:15],
                        e['sector'], e['size'], e['crc'], ON if e['on'] else 0)
    struct.pack_into('>I', body, 12, zlib.crc32(bytes(body)))
    return bytes(body)


class Card:
    """A card image, opened for TELOS's store."""

    def __init__(self, path, device_key='digitone-mk1'):
        if STORE.get(device_key, (0,))[0] is None:
            raise ModError('the TELOS store on %s is a file on its +Drive: use unit' % device_key)
        self.path = path
        self.dev, (self.first, self.sectors) = store_of(device_key)

    def _read(self, sector, n):
        with open(self.path, 'rb') as f:
            f.seek((self.first + sector) * SECTOR)
            b = f.read(n)
        return b + bytes(n - len(b))         # past the image's end: never written, zeros

    def _write(self, sector, data):
        with open(self.path, 'r+b') as f:
            f.seek((self.first + sector) * SECTOR)
            f.write(data)

    def read(self):
        """-> (seq, entries with their bytes), (0, []) for a store never written."""
        best = None
        for at in (DIR_A, DIR_B):
            d = parse_dir(self._read(at, DIR_BYTES), self.sectors)
            if d and (best is None or d[0] > best[0]):
                best = d
        if best is None:
            return 0, []
        seq, entries = best
        for e in entries:
            e['data'] = self._read(e['sector'], e['size'])
        return seq, entries

    def write(self, seq, entries):
        """Pack the files from sector DATA and write both directory copies with seq + 1."""
        if len(entries) > MAX:
            raise ModError('the store holds %d mods at most' % MAX)
        at = DATA
        for e in entries:
            e['sector'] = at
            at += (len(e['data']) + SECTOR - 1) // SECTOR
        if at > self.sectors:
            raise ModError('the mods take %d KB, the store has %d KB'
                           % ((at - DATA) // 2, (self.sectors - DATA) // 2))
        for e in entries:
            e['size'] = len(e['data'])
            e['crc'] = zlib.crc32(e['data'])
            pad = (-len(e['data'])) % SECTOR
            self._write(e['sector'], e['data'] + bytes(pad))
        d = build_dir(entries, seq + 1, self.sectors)
        self._write(DIR_A, d)
        self._write(DIR_B, d)
        return seq + 1

    def armed(self):
        return self._read(ARMED, 4) == b'ARMD'


def mod_entry(path, dev, on=True):
    data = open(path, 'rb').read()
    try:
        m = json.loads(data)
    except ValueError as e:
        raise ModError('%s: not an .elemod (%s)' % (path, e))
    if not isinstance(m, dict):
        raise ModError('%s: not an .elemod' % path)
    key = (m.get('target') or {}).get('device') or m.get('device')
    if key != dev.key:
        raise ModError('%s: made for %s, not %s' % (path, key, dev.key))
    name = os.path.basename(path)
    if len(name.encode()) > 31:
        raise ModError('%s: the file name takes 31 bytes at most' % name)
    title = (m.get('title') or m.get('id') or name).upper()
    for cut in (' (', ':'):
        title = title.split(cut)[0]
    return {'name': name, 'title': title[:14], 'on': on, 'data': data}


# ---- the unit, over SysEx (mods/telos-dn1/upload.c) -----------------------------------------------

HDR = bytes((0xF0, 0x7D, 0x54, 0x4C))
C_INFO, C_ENTRY, C_BEGIN, C_DATA, C_END, C_SET, C_DELETE, C_RESTART, C_ABORT, C_FORMAT = range(1, 11)
CHUNK = 512
STATUS = {1: 'busy', 2: 'a message it could not read', 3: 'no store yet (format it first)',
          4: 'a file of that name is in the store', 5: 'the store holds %d mods at most' % MAX,
          6: 'no free run of sectors holds it', 7: 'out of order', 8: 'the CRC read back does not match',
          9: 'the card write failed', 10: 'not in the store', 11: 'no upload in progress'}
S_BUSY = 1
STATE = {0: '', 1: 'loaded', 2: 'refused', 3: 'dropped'}


def request(cmd, seq, body=b''):
    from elekloader.datarpc import pack7
    return HDR + bytes((cmd, seq & 0x7F)) + pack7(body) + b'\xf7'


def parse_reply(raw):
    """A SysEx message -> (cmd, seq, status, body), or None if it is not a TELOS reply."""
    from elekloader.datarpc import unpack7
    raw = bytes(raw)
    if len(raw) < 9 or raw[:4] != HDR or raw[-1] != 0xF7 or not raw[4] & 0x40:
        return None
    plain = unpack7(raw[6:-1])
    if not plain:
        return None
    return raw[4] & 0x3F, raw[5], plain[0], plain[1:]


def _name(s, n):
    b = s.encode()[:n - 1]
    return b + bytes(n - len(b))


class Unit:
    """TELOS on a unit, through a port: an object with send(bytes) and receive(timeout) -> bytes or None
    (one whole SysEx message), as elekloader.datarpc's. The unit answers each request once its UI task
    has carried it out (about a tick), so one request at a time."""

    def __init__(self, port, timeout=5.0, device_key='digitone-mk1'):
        self.port, self.timeout, self.seq = port, timeout, 0
        self.dev, (self.first, self.sectors) = store_of(device_key)

    def call(self, cmd, body=b''):
        for _ in range(100):                     # BUSY: the last request is still being carried out
            self.seq = (self.seq + 1) & 0x7F
            self.port.send(request(cmd, self.seq, body))
            end = time.monotonic() + self.timeout
            while True:
                left = end - time.monotonic()
                raw = self.port.receive(left) if left > 0 else None
                if raw is None:
                    raise ModError('no reply from the unit to command 0x%02x (is TELOS flashed?)' % cmd)
                r = parse_reply(raw)
                if r and r[0] == cmd and r[1] == self.seq:
                    break
            if r[2] != S_BUSY:
                break
            time.sleep(0.05)
        if r[2]:
            raise ModError('the unit refused command 0x%02x: %s' % (cmd, STATUS.get(r[2], r[2])))
        return r[3]

    def info(self):
        b = self.call(C_INFO)
        n, most = struct.unpack_from('>HH', b, 2)
        sectors, free, largest = struct.unpack_from('>III', b, 6)
        return {'protocol': b[0], 'store': bool(b[1]), 'entries': n, 'most': most, 'sectors': sectors,
                'free': free, 'largest': largest, 'version': _cstr(b[18:26])}

    def read(self):
        """-> the entries, as Card.read's (without their bytes), with their boot state and code."""
        out = []
        for i in range(self.info()['entries']):
            b = self.call(C_ENTRY, bytes((i,)))
            size, crc = struct.unpack_from('>II', b, 4)
            out.append({'name': _cstr(b[12:44]), 'title': _cstr(b[44:60]), 'on': bool(b[0] & ON),
                        'size': size, 'crc': crc, 'state': b[1], 'code': b[2]})
        return out

    def format(self):
        self.call(C_FORMAT)

    def add(self, e, progress=None):
        data = e['data']
        self.call(C_BEGIN, struct.pack('>IIB', len(data), zlib.crc32(data), 1 if e['on'] else 0)
                  + _name(e['name'], 32) + _name(e['title'], 16))
        try:
            for off in range(0, len(data), CHUNK):
                self.call(C_DATA, struct.pack('>I', off) + data[off:off + CHUNK])
                if progress:
                    progress(min(off + CHUNK, len(data)), len(data))
            self.call(C_END)
        except ModError:
            try:
                self.call(C_ABORT)
            except ModError:
                pass
            raise

    def set(self, name, on):
        self.call(C_SET, bytes((1 if on else 0,)) + _name(name, 32))

    def remove(self, name):
        self.call(C_DELETE, _name(name, 32))

    def restart(self):
        self.call(C_RESTART)


def unit_main(args, port=None):
    """`python tools/telos.py unit ...`: the store's commands on a unit, over USB MIDI."""
    if port is not None:
        return _unit_run(args, port)
    from elekloader.datarpc import open_port
    port = open_port(args.port)
    try:
        return _unit_run(args, port)
    finally:
        if hasattr(port, 'close'):
            port.close()


def _unit_run(args, port):
    u = Unit(port, device_key=getattr(args, 'device', None) or 'digitone-mk1')
    inf = u.info()
    if args.cmd in ('info', 'list'):
        print('TELOS %s on the unit: %s, %d of %d mods, %d KB free (largest run %d KB) of %d KB' % (
            inf['version'], 'a store' if inf['store'] else 'no store yet', inf['entries'], inf['most'],
            inf['free'] // 2, inf['largest'] // 2, (inf['sectors'] - DATA) // 2))
        if args.cmd == 'list':
            for e in u.read():
                st = STATE.get(e['state'], '?') + (' %d' % e['code'] if e['code'] else '')
                print('  %-3s %-31s %-14s %7d bytes  crc %08x  %s' % (
                    'ON' if e['on'] else 'off', e['name'], e['title'], e['size'], e['crc'], st))
        return 0
    if args.cmd == 'format' or (args.cmd == 'add' and not inf['store']):
        u.format()
        print('FORMATTED the TELOS store on the unit')
        if args.cmd == 'format':
            return 0
    if args.cmd == 'add':
        have = {e['name'] for e in u.read()}
        for p in args.mods:
            e = mod_entry(p, u.dev, on=not args.off)
            if e['name'] in have:
                u.remove(e['name'])
            u.add(e, lambda d, n: print('\r  %s %d / %d' % (e['name'], d, n), end='', flush=True))
            print('\nSENT %s (%s)' % (e['name'], 'on' if e['on'] else 'off'))
        print('They load after a restart: MODS (TELOS) > APPLY & RESTART, or python tools/telos.py '
              'unit restart')
    elif args.cmd in ('on', 'off'):
        u.set(args.name, args.cmd == 'on')
        print('%s is %s from the next boot' % (args.name, args.cmd.upper()))
    elif args.cmd == 'remove':
        u.remove(args.name)
        print('REMOVED %s' % args.name)
    elif args.cmd == 'clear':
        for e in u.read():
            u.remove(e['name'])
        print('CLEARED the store')
    elif args.cmd == 'restart':
        u.restart()
        print('the unit restarts')
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog='python tools/telos.py',
                                 description="TELOS's store, on a unit or in a card image.")
    ap.add_argument('card', help='the card image (an emulator\'s plusdrive.img), or unit: the unit over '
                                 'USB MIDI SysEx')
    ap.add_argument('--device', default='digitone-mk1')
    ap.add_argument('--port', default='Digitone', help='with unit: a MIDI port whose name has this in it')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('list')
    a = sub.add_parser('add')
    a.add_argument('mods', nargs='+')
    a.add_argument('--off', action='store_true', help='add them switched off')
    for c in ('on', 'off', 'remove'):
        sub.add_parser(c).add_argument('name')
    sub.add_parser('clear')
    for c in ('info', 'restart', 'format'):
        sub.add_parser(c, help='unit only')
    args = ap.parse_args(argv)
    try:
        if args.card == 'unit':
            return unit_main(args)
        if args.cmd in ('info', 'restart', 'format'):
            raise ModError('%s is for the unit: python tools/telos.py unit %s' % (args.cmd, args.cmd))
        card = Card(args.card, args.device)
        seq, entries = card.read()
        if args.cmd == 'list':
            print('TELOS store at sector 0x%x, %d KB, sequence %d%s' % (
                card.first, card.sectors // 2, seq, ', crash guard set' if card.armed() else ''))
            used = sum((e['size'] + SECTOR - 1) // SECTOR for e in entries)
            for e in entries:
                print('  %-3s %-31s %-14s %7d bytes  crc %08x' % (
                    'ON' if e['on'] else 'off', e['name'], e['title'], e['size'], e['crc']))
            print('%d mods, %d KB of %d KB' % (len(entries), used // 2, (card.sectors - DATA) // 2))
            return 0
        if args.cmd == 'add':
            for p in args.mods:
                e = mod_entry(p, card.dev, on=not args.off)
                entries = [x for x in entries if x['name'] != e['name']] + [e]
        elif args.cmd in ('on', 'off', 'remove'):
            hit = [e for e in entries if e['name'] == args.name]
            if not hit:
                raise ModError('no %s in the store' % args.name)
            if args.cmd == 'remove':
                entries = [e for e in entries if e['name'] != args.name]
            else:
                hit[0]['on'] = args.cmd == 'on'
        elif args.cmd == 'clear':
            entries = []
        seq = card.write(seq, entries)
        print('WROTE the TELOS store: %d mods, sequence %d' % (len(entries), seq))
        return 0
    except (ModError, OSError) as e:
        print('REFUSED: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
