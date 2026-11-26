"""SD reader (src/sdr.c) on the Z80 model: FAT32 images made by mkfs.fat /
mtools, the file read back sector by sector against the reference reader
of fatimg.py. Run from the repository root: make test."""
import os
import random
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fatimg import make_image          # noqa: E402
from tsmodel import Machine, SDCard    # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'build' / 'test'
WMF = OUT / 'SDRTEST.WMF'
MAP = OUT / 'sdrtest' / 'out.map'
PAGE = 0x90
CPU_HZ = 14_000_000
ERRORS = ['OK', 'EMPTY', 'SAMPLE', 'CHAIN', 'CARD', 'VOLUME', 'ALIGN', 'FAT', 'FRAG', 'VERIFY', 'IO']

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print('FAIL:', what)


def build():
    subprocess.run([sys.executable, '-I', str(ROOT / 'tools/build.py'), '--build-dir', str(OUT / 'sdrtest'), '--crt0', str(ROOT / 'diag/crt0.s'), '--header', str(ROOT / 'diag/sdrdiag_h.c'),
                    '-o', str(WMF), str(ROOT / 'test/sdr_main.c'), str(ROOT / 'src/sdr.c')],
                   check=True, stdout=subprocess.DEVNULL)


def payload(size, seed):
    rnd = random.Random(seed)
    return rnd.randbytes(size)


def run(fat, name, cards, reqs=()):
    m = Machine(WMF, MAP, fat, name, cards)
    m.poke('_t_size', fat.find(name)[2], 4)
    for i, (fsec, count, offs) in enumerate(reqs):
        base = m.sym['_t_req'] + i * 9
        m.cpu.set_memory_block(base, struct.pack('<IBBHB', fsec, count, PAGE + i, offs, 0xEE))
    m.cpu.set_memory_block(m.sym['_t_req'] + len(reqs) * 9, bytes(9))
    m.run()
    for card in cards.values():
        if card:
            check(not card.errors, '%s: card errors %s' % (name, card.errors))
    return m


def extents(m):
    out = []
    for i in range(m.peek('_sdr', 2, 4)):
        fcl = m.peek('_sdr', 4, 16 + i * 8)
        lba = m.peek('_sdr', 4, 20 + i * 8)
        out.append((fcl, lba))
    return out


def ref_extents(fat, name):
    lbas = fat.file_lbas(name)
    out = []
    for i in range(0, len(lbas), fat.spc):
        if not out or lbas[i] != lbas[i - fat.spc] + fat.spc:
            out.append((i // fat.spc, lbas[i]))
    return out


def reads_ok(m, reqs, what):
    for i, (fsec, count, offs) in enumerate(reqs):
        ok = m.cpu.memory[m.sym['_t_req'] + i * 9 + 8]
        want = m.data[fsec * 512:(fsec + count) * 512]
        expect_ok = (fsec + count) * 512 <= ((len(m.data) + 511) // 512) * 512
        check(ok == int(expect_ok), '%s: read %d ok = %d' % (what, i, ok))
        if expect_ok:
            got = bytes(m.page(PAGE + i)[offs:offs + count * 512])
            # the tail of the last sector is the card's, not the file's
            check(got[:len(want)] == want, '%s: read %d data' % (what, i))


def case(title, fat, name, cards, reqs=(), expect='OK', ext=None):
    m = run(fat, name, cards, reqs)
    r = ERRORS[m.peek('_t_result')]
    check(r == expect, '%s: result %s, expected %s' % (title, r, expect))
    if r == 'OK':
        check(extents(m) == ref_extents(fat, name), '%s: extents %s' % (title, extents(m)[:5]))
        if ext is not None:
            check(len(extents(m)) == ext, '%s: %d extents' % (title, len(extents(m))))
        reads_ok(m, reqs, title)
    print('%-44s %-6s %3d extents  %7.1f ms' % (title, r, m.peek('_sdr', 2, 4), m.ticks * 1000 / CPU_HZ))
    return m


def main():
    build()
    img = OUT / 'img'
    big = payload(300 * 1024 + 123, 1)
    # SD1, FAT32 on the whole card, 1 sector a cluster, SDHC; the file in 5 runs.
    a = make_image(img / 'a.img', 40, 1, 0, [('A.TXT', b'x' * 1000), ('VIDEO.AVI', big),
                                              ('ONE.BIN', payload(512, 2))])
    a.scatter('VIDEO.AVI', 5)
    reqs = [(0, 1, 0), (1, 32, 0x0000), (100, 7, 0x2000), (590, 11, 0x0200),
            (600, 1, 0x3E00), (601, 1, 0), (602, 1, 0)]
    case('SD1, no MBR, spc 1, 5 runs, SDHC', a, 'VIDEO.AVI',
         {1: SDCard(a.mm, hc=True), 2: None}, reqs, ext=5)
    case('one sector file', a, 'ONE.BIN', {1: SDCard(a.mm), 2: None}, [(0, 1, 0), (1, 1, 0)], ext=1)
    # SD2 in an MBR partition at 2048, 8 sectors a cluster, byte addressing;
    # SD1 holds another card that answers but not with this file.
    other = make_image(img / 'o.img', 40, 1, 0, [('X.BIN', payload(4096, 3))])
    b = make_image(img / 'b.img', 300, 8, 2048, [('VIDEO.AVI', big)])
    case('SD2, MBR, spc 8, SDSC, SD1 other card', b, 'VIDEO.AVI',
         {1: SDCard(other.mm), 2: SDCard(b.mm, hc=False)}, [(5, 30, 0), (0, 3, 0x1000)], ext=1)
    # SD1 not initialised by WC (idle): the reader takes SD2.
    case('SD1 idle, file on SD2', b, 'VIDEO.AVI',
         {1: SDCard(other.mm, ready=False), 2: SDCard(b.mm)}, [(7, 9, 0)])
    case('no card with the file', b, 'VIDEO.AVI',
         {1: SDCard(other.mm), 2: None}, expect='CARD')
    # The table holds 30 runs.
    c = make_image(img / 'c.img', 40, 1, 0, [('VIDEO.AVI', big)])
    c.scatter('VIDEO.AVI', 30)
    case('30 runs', c, 'VIDEO.AVI', {1: SDCard(c.mm), 2: None}, [(0, 32, 0), (580, 21, 0)], ext=30)
    c = make_image(img / 'c.img', 40, 1, 0, [('VIDEO.AVI', big)])
    c.scatter('VIDEO.AVI', 31)
    case('31 runs', c, 'VIDEO.AVI', {1: SDCard(c.mm), 2: None}, expect='FRAG')
    # A broken chain: a link to a free cluster number beyond the volume.
    d = make_image(img / 'd.img', 40, 1, 0, [('VIDEO.AVI', big)])
    _, first, _ = d.find('VIDEO.AVI')
    ch = d.chain(first)
    d.set_fat(ch[10], 0x0FFFFFF0)
    case('bad link', d, 'VIDEO.AVI', {1: SDCard(d.mm), 2: None}, expect='FAT')
    # The time of the FAT walk: 8 MB in clusters of 512 bytes.
    e = make_image(img / 'e.img', 40, 1, 0, [('LONG.AVI', payload(8 << 20, 4))])
    case('8 MB, 16384 clusters (walk time)', e, 'LONG.AVI', {1: SDCard(e.mm), 2: None},
         [(16383, 1, 0)], ext=1)
    print('test_sdr: %d errors' % failures)
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
