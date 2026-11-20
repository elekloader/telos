"""TELOS's store from the computer (tools/telos.py) and its agreement with the unit's side (telos.h,
upload.c). No firmware needed; elekloader must be importable (PYTHONPATH=<elekloader checkout>).

    python tests/test_telos.py
"""
import json
import os
import re
import struct
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools'))

import telos                                              # noqa: E402
from elekloader.elemod import ModError                    # noqa: E402

HEADER = os.path.join(ROOT, 'telos.h')


class _dn1:
    telos_store = telos.STORE['digitone-mk1']


def _mod(folder, name, device='digitone-mk1', title='HELLO (TELOS)', pad=0):
    m = {'format': 2, 'id': name.split('.')[0], 'version': '0.1', 'title': title,
         'target': {'device': device}, 'pad': 'x' * pad}
    p = os.path.join(folder, name)
    with open(p, 'w') as f:
        json.dump(m, f)
    return p


def _card(folder):
    p = os.path.join(folder, 'plusdrive.img')
    open(p, 'wb').close()
    return p


def test_the_header_matches_the_profile():
    text = open(HEADER).read()
    first = int(re.search(r'#define TS_SECTOR\s+(0x[0-9a-fA-F]+)u', text).group(1), 16)
    count = int(re.search(r'#define TS_SECTORS\s+(0x[0-9a-fA-F]+)u', text).group(1), 16)
    assert (first, count) == _dn1.telos_store
    assert count * 512 == 2 * 1024 * 1024
    for name, value in (('TS_DIR_A', telos.DIR_A), ('TS_DIR_B', telos.DIR_B), ('TS_ARMED', telos.ARMED),
                        ('TS_DATA', telos.DATA), ('TS_MAX', telos.MAX), ('TS_VERSION', telos.VERSION)):
        assert int(re.search(r'#define %s\s+(\d+)' % name, text).group(1)) == value, name
    assert telos.HEAD.size == 32 and telos.ENTRY.size == 64


def test_the_store_is_outside_every_stock_access():
    first, count = _dn1.telos_store
    assert first >= 0x180000                       # the stock store writes below this
    assert first + count <= 0x3A4000               # the smallest part the firmware accepts


def test_add_list_switch_remove():
    with tempfile.TemporaryDirectory() as d:
        card = _card(d)
        a, b = _mod(d, 'hello.elemod'), _mod(d, 'solo.elemod', title='SOLO & MUTE GROUPS', pad=3000)
        assert telos.main([card, 'add', a, b]) == 0
        seq, entries = telos.Card(card).read()
        assert seq == 1 and [e['name'] for e in entries] == ['hello.elemod', 'solo.elemod']
        assert [e['title'] for e in entries] == ['HELLO', 'SOLO & MUTE GR']
        assert all(e['on'] for e in entries)
        assert entries[0]['sector'] == telos.DATA
        assert entries[1]['sector'] == telos.DATA + (entries[0]['size'] + 511) // 512
        assert entries[1]['data'] == open(b, 'rb').read()
        assert entries[1]['crc'] == zlib.crc32(open(b, 'rb').read())
        assert telos.main([card, 'off', 'hello.elemod']) == 0
        seq, entries = telos.Card(card).read()
        assert seq == 2 and not entries[0]['on'] and entries[1]['on']
        assert telos.main([card, 'remove', 'hello.elemod']) == 0
        seq, entries = telos.Card(card).read()
        assert [e['name'] for e in entries] == ['solo.elemod'] and entries[0]['sector'] == telos.DATA
        assert telos.main([card, 'off', 'nothing.elemod']) == 1


def test_the_newer_valid_copy_counts():
    """The unit writes one copy with the sequence + 1 (telos.c, telos_dir_write); a torn or older
    copy never wins."""
    with tempfile.TemporaryDirectory() as d:
        card = _card(d)
        telos.main([card, 'add', _mod(d, 'a.elemod')])
        c = telos.Card(card)
        seq, entries = c.read()
        entries[0]['on'] = False
        c._write(telos.DIR_B, telos.build_dir(entries, seq + 1, c.sectors))
        assert c.read()[0] == seq + 1 and not c.read()[1][0]['on']
        torn = bytearray(telos.build_dir(entries, seq + 5, c.sectors))
        torn[100] ^= 1                             # cut off by a power loss
        c._write(telos.DIR_A, bytes(torn))
        assert c.read()[0] == seq + 1              # B, the valid one
        assert telos.parse_dir(bytes(torn), c.sectors) is None


def test_refusals():
    with tempfile.TemporaryDirectory() as d:
        card = _card(d)
        assert telos.main([card, 'add', _mod(d, 'dt.elemod', device='digitakt-mk1')]) == 1
        assert telos.main([card, 'add', _mod(d, 'x' * 30 + '.elemod')]) == 1     # name past 31 bytes
        big = _mod(d, 'big.elemod', pad=(telos.Card(card).sectors - telos.DATA) * 512)
        assert telos.main([card, 'add', big]) == 1                              # past 2 MB
        assert telos.Card(card).read() == (0, [])
        try:
            telos.Card(card, 'octatrack')
            raise AssertionError('the Octatrack has no TELOS store here')
        except ModError:
            pass


def test_the_crash_guard_is_read():
    with tempfile.TemporaryDirectory() as d:
        card = _card(d)
        c = telos.Card(card)
        assert not c.armed()
        c._write(telos.ARMED, b'ARMD' + bytes(508))
        assert c.armed()


def test_the_directory_layout():
    entries = [{'name': 'a.elemod', 'title': 'A', 'sector': 32, 'size': 5, 'crc': 7, 'on': True}]
    raw = telos.build_dir(entries, 9, 0x1000)
    assert raw[:4] == b'TELS' and len(raw) == 4096
    assert struct.unpack_from('>HHIIII', raw, 4) == (1, 1, 9, struct.unpack_from('>I', raw, 12)[0], 0x1000, 32)
    assert zlib.crc32(raw[:12] + bytes(4) + raw[16:]) == struct.unpack_from('>I', raw, 12)[0]
    assert raw[32:40] == b'a.elemod' and raw[64:65] == b'A'
    assert struct.unpack_from('>IIIB', raw, 32 + 48) == (32, 5, 7, 1)


class FakeUnit:
    """The unit's side of the SysEx protocol (upload.c), rule for rule where the client
    can tell, over a card image: a port for telos.Unit. The emulator runs the real one (README)."""

    def __init__(self, card, busy=0):
        self.card, self.busy, self.replies = telos.Card(card), busy, []
        self.up = None
        self.store = False

    def send(self, raw):
        from elekloader.datarpc import pack7, unpack7
        assert raw[:4] == telos.HDR and raw[-1] == 0xF7
        cmd, seq, body = raw[4], raw[5], unpack7(raw[6:-1])
        if self.busy:
            self.busy -= 1
            st, out = telos.S_BUSY, b''
        else:
            st, out = self.do(cmd, body)
        self.replies.append(telos.HDR + bytes((cmd | 0x40, seq)) + pack7(bytes((st,)) + (out if not st else b''))
                            + b'\xf7')

    def receive(self, timeout):
        return self.replies.pop(0) if self.replies else None

    def do(self, cmd, b):
        c = self.card
        seq, entries = c.read()
        if cmd == telos.C_INFO:
            return 0, struct.pack('>BBHHIII', 1, int(self.store), len(entries), telos.MAX, c.sectors, 4000, 4000) \
                + b'0.2'.ljust(8, b'\0')
        if cmd == telos.C_FORMAT:
            c.write(seq, [])
            self.store = True
            return 0, b''
        if not self.store:
            return 3, b''
        if cmd == telos.C_ENTRY:
            if b[0] >= len(entries):
                return 10, b''
            e = entries[b[0]]
            return 0, struct.pack('>BBBBII', int(e['on']), 0, 0, 0, e['size'], e['crc']) \
                + e['name'].encode().ljust(32, b'\0') + e['title'].encode().ljust(16, b'\0')
        if cmd == telos.C_BEGIN:
            size, crc, on = struct.unpack_from('>IIB', b)
            name = telos._cstr(b[9:41])
            if any(e['name'] == name for e in entries):
                return 4, b''
            self.up = {'name': name, 'title': telos._cstr(b[41:57]), 'on': bool(on), 'crc': crc, 'size': size,
                       'data': b''}
            return 0, b''
        if cmd == telos.C_DATA:
            off = struct.unpack_from('>I', b)[0]
            if not self.up or off != len(self.up['data']) or len(b) - 4 > telos.CHUNK:
                return 7, b''
            self.up['data'] += b[4:]
            return 0, struct.pack('>I', len(self.up['data']))
        if cmd == telos.C_END:
            u, self.up = self.up, None
            if zlib.crc32(u['data']) != u['crc'] or len(u['data']) != u['size']:
                return 8, b''
            c.write(seq, entries + [{'name': u['name'], 'title': u['title'], 'on': u['on'], 'data': u['data']}])
            return 0, b''
        if cmd in (telos.C_SET, telos.C_DELETE):
            name = telos._cstr(b[1:33] if cmd == telos.C_SET else b[:32])
            hit = [e for e in entries if e['name'] == name]
            if not hit:
                return 10, b''
            if cmd == telos.C_SET:
                hit[0]['on'] = bool(b[0])
            else:
                entries.remove(hit[0])
            c.write(seq, entries)
            return 0, b''
        if cmd in (telos.C_ABORT, telos.C_RESTART):
            self.up = None
            return 0, b''
        return 2, b''


class _Args:
    port = None
    off = False
    mods = ()
    name = ''


def _unit(port, cmd, **kw):
    a = _Args()
    a.cmd = cmd
    for k, v in kw.items():
        setattr(a, k, v)
    return telos.unit_main(a, port)


def test_the_unit_over_sysex():
    with tempfile.TemporaryDirectory() as d:
        card = _card(d)
        unit = FakeUnit(card)
        big = _mod(d, 'solo.elemod', title='SOLO & MUTE GROUPS', pad=3000)     # several DATA chunks
        assert _unit(unit, 'add', mods=[_mod(d, 'hello.elemod'), big]) == 0      # formats first
        seq, entries = telos.Card(card).read()
        assert [e['name'] for e in entries] == ['hello.elemod', 'solo.elemod']
        assert entries[1]['data'] == open(big, 'rb').read()
        u = telos.Unit(unit)
        assert [(e['name'], e['on'], e['title']) for e in u.read()] == [
            ('hello.elemod', True, 'HELLO'), ('solo.elemod', True, 'SOLO & MUTE GR')]
        assert _unit(unit, 'off', name='solo.elemod') == 0
        assert not telos.Card(card).read()[1][1]['on']
        assert _unit(unit, 'add', mods=[big], off=True) == 0                      # again: replaced
        assert [e['name'] for e in telos.Card(card).read()[1]] == ['hello.elemod', 'solo.elemod']
        assert _unit(unit, 'remove', name='hello.elemod') == 0
        assert [e['name'] for e in u.read()] == ['solo.elemod']
        try:
            u.remove('nothing.elemod')
            raise AssertionError('removed a file that is not there')
        except ModError as e:
            assert 'not in the store' in str(e)


def test_the_unit_busy_and_silent():
    with tempfile.TemporaryDirectory() as d:
        unit = FakeUnit(_card(d), busy=3)
        assert telos.Unit(unit).info()['protocol'] == 1                          # asks again after BUSY

        class Silent:
            def send(self, raw):
                pass

            def receive(self, timeout):
                return None
        try:
            telos.Unit(Silent(), timeout=0.01).info()
            raise AssertionError('no reply was taken for one')
        except ModError as e:
            assert 'no reply' in str(e)


def test_the_request_frame():
    raw = telos.request(telos.C_DATA, 0x85, b'\x80\x01')
    assert raw[:6] == bytes((0xF0, 0x7D, 0x54, 0x4C, 4, 0x05)) and raw[-1] == 0xF7
    assert all(b < 0x80 for b in raw[1:-1])
    assert telos.parse_reply(bytes((0xF0, 0x7D, 0x54, 0x4C, 0x44, 5, 0, 0, 0xF7))) == (4, 5, 0, b'')
    assert telos.parse_reply(raw) is None                                         # a request, not a reply



def test_the_octatracks_card_folder():
    """octatrack/: a CF card's folder takes the same commands; active names the files ON with their
    CRC-32 (telos.c's dl_parse_active), in the order they were added, and list shows status.txt."""
    with tempfile.TemporaryDirectory() as tmp:
        card = os.path.join(tmp, 'card')
        os.makedirs(card)
        a = _mod(tmp, 'tuner-0.1.elemod', device='octatrack', title='TUNER')
        b = _mod(tmp, 'scenes-p2-0.1.elemod', device='octatrack', title='SCENES P2')
        assert telos.main([card, 'add', a, b]) == 0
        mods = os.path.join(card, 'ELEKLOADER', 'MODS')
        crc = lambda n: zlib.crc32(open(os.path.join(mods, n), 'rb').read()) & 0xFFFFFFFF
        act = lambda: open(os.path.join(mods, 'active')).read()
        assert act() == 'tuner-0.1.elemod %08x\nscenes-p2-0.1.elemod %08x\n' % (
            crc('tuner-0.1.elemod'), crc('scenes-p2-0.1.elemod'))
        assert telos.main([card, 'off', 'tuner-0.1.elemod']) == 0
        assert act() == 'scenes-p2-0.1.elemod %08x\n' % crc('scenes-p2-0.1.elemod')
        assert os.path.exists(os.path.join(mods, 'tuner-0.1.elemod'))          # off keeps the file
        assert telos.main([card, 'remove', 'tuner-0.1.elemod']) == 0
        assert not os.path.exists(os.path.join(mods, 'tuner-0.1.elemod'))
        assert telos.main([card, 'add', _mod(tmp, 'dn.elemod')]) == 1          # a Digitone mk1 mod
        with open(os.path.join(mods, 'status.txt'), 'w') as f:
            f.write('telos 0.4: 1 loaded of 1, skip 0, tries 1\nscenes-p2 loaded\n')
        assert telos.main([card, 'list']) == 0
        assert telos.main([card, 'clear']) == 0 and act() == ''
        assert telos.main([card, 'restart']) == 1


if __name__ == '__main__':
    import traceback
    ok = failed = 0
    for n in sorted(k for k in globals() if k.startswith('test_')):
        try:
            globals()[n]()
            print('ok      %s' % n)
            ok += 1
        except Exception:
            print('FAIL    %s' % n)
            traceback.print_exc()
            failed += 1
    print('%d passed, %d failed' % (ok, failed))
    sys.exit(1 if failed else 0)
