"""FAT32 card images for the tests: mkfs.fat + mtools make the volume, the
class Fat32 reads it independently of the plugin (the reference) and can
scatter a file over the volume (fragmentation)."""
import mmap
import os
import random
import struct
import subprocess
from pathlib import Path

ENV = dict(os.environ, MTOOLS_SKIP_CHECK='1', MTOOLS_NO_VFAT='1')


def make_image(path, size_mb, spc, part_lba=0, files=()):
    """A card image of size_mb: FAT32 on the whole card (part_lba 0) or in
    a primary partition (type #0C) from part_lba. files: (name 8.3, data)
    copied into the root directory. Returns an open Fat32."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        path.unlink()
    total = size_mb * 2048
    with open(path, 'wb') as f:
        f.truncate(total * 512)
    args = ['mkfs.fat', '-F', '32', '-s', str(spc), '-S', '512', '-n', 'TEST']
    if part_lba:
        mbr = bytearray(512)
        mbr[0x1BE:0x1CE] = struct.pack('<B3sB3sII', 0, b'\xFE\xFF\xFF', 0x0C,
                                       b'\xFE\xFF\xFF', part_lba, total - part_lba)
        mbr[510:512] = b'\x55\xAA'
        with open(path, 'r+b') as f:
            f.write(mbr)
        args += ['--offset', str(part_lba)]
        blocks = (total - part_lba) // 2
    else:
        blocks = total // 2
    subprocess.run(args + [str(path), str(blocks)], check=True, capture_output=True, env=ENV)
    target = '%s@@%d' % (path, part_lba * 512)
    tmp = path.with_suffix('.tmp')
    for name, data in files:
        tmp.write_bytes(data)
        subprocess.run(['mcopy', '-i', target, str(tmp), '::' + name], check=True,
                       capture_output=True, env=ENV)
    if tmp.exists():
        tmp.unlink()
    return Fat32(path, part_lba)


class Fat32:
    def __init__(self, path, vol):
        self.path, self.vol = Path(path), vol
        self.file = open(self.path, 'r+b')
        self.mm = mmap.mmap(self.file.fileno(), 0)
        b = self.sector(vol)
        (bps, self.spc, rsv, self.nfat) = struct.unpack_from('<HBHB', b, 11)
        assert bps == 512
        self.total, self.fatsz = struct.unpack_from('<II', b, 32)
        self.root = struct.unpack_from('<I', b, 44)[0]
        self.fat_lba = vol + rsv
        self.data_lba = self.fat_lba + self.nfat * self.fatsz
        self.clusters = (vol + self.total - self.data_lba) // self.spc

    def sector(self, lba):
        return bytes(self.mm[lba * 512:lba * 512 + 512])

    def lba(self, cl):
        return self.data_lba + (cl - 2) * self.spc

    def fat(self, cl):
        off = self.fat_lba * 512 + cl * 4
        return struct.unpack_from('<I', self.mm, off)[0] & 0x0FFFFFFF

    def set_fat(self, cl, value):
        for n in range(self.nfat):
            off = (self.fat_lba + n * self.fatsz) * 512 + cl * 4
            struct.pack_into('<I', self.mm, off, value)

    def chain(self, cl):
        out = []
        while 2 <= cl < self.clusters + 2:
            out.append(cl)
            cl = self.fat(cl)
        return out

    def find(self, name):
        """(offset of the entry, first cluster, size) of a root entry 8.3."""
        base, ext = (name.split('.') + [''])[:2]
        want = (base.ljust(8) + ext.ljust(3)).upper().encode()
        for cl in self.chain(self.root):
            for i in range(self.spc * 16):
                off = self.lba(cl) * 512 + i * 32
                e = self.mm[off:off + 32]
                if e[0] == 0:
                    break
                if e[:11] == want and not e[11] & 0x18:
                    hi, lo = struct.unpack_from('<H', e, 20)[0], struct.unpack_from('<H', e, 26)[0]
                    return off, (hi << 16) | lo, struct.unpack_from('<I', e, 28)[0]
        raise KeyError(name)

    def data(self, name):
        _, cl, size = self.find(name)
        out = b''.join(bytes(self.mm[self.lba(c) * 512:(self.lba(c) + self.spc) * 512])
                       for c in self.chain(cl))
        return out[:size]

    def file_lbas(self, name):
        _, cl, size = self.find(name)
        lbas = [self.lba(c) + i for c in self.chain(cl) for i in range(self.spc)]
        return lbas[:(size + 511) // 512]

    def scatter(self, name, runs, seed=1):
        """Move the clusters of a file into `runs` runs of free clusters in a
        random order (each run a gap away from the others)."""
        rnd = random.Random(seed)
        off, cl, size = self.find(name)
        old = self.chain(cl)
        used = set()
        for c in range(2, self.clusters + 2):
            if self.fat(c):
                used.add(c)
        free = [c for c in range(2, self.clusters + 2) if c not in used]
        n = len(old)
        cuts = sorted(rnd.sample(range(1, n), runs - 1)) if runs > 1 else []
        sizes = [b - a for a, b in zip([0] + cuts, cuts + [n])]
        # Runs from the free area in a shuffled order, a gap of 3 between them.
        slots, pos = [], 0
        for s in sizes:
            slots.append(free[pos:pos + s])
            assert len(slots[-1]) == s and slots[-1][-1] - slots[-1][0] == s - 1
            pos += s + 3
        rnd.shuffle(slots)
        new = [c for run in slots for c in run]
        data = [bytes(self.mm[self.lba(c) * 512:(self.lba(c) + self.spc) * 512]) for c in old]
        for c in old:
            self.set_fat(c, 0)
        for c, d in zip(new, data):
            self.mm[self.lba(c) * 512:(self.lba(c) + self.spc) * 512] = d
        for a, b in zip(new, new[1:]):
            self.set_fat(a, b)
        self.set_fat(new[-1], 0x0FFFFFFF)
        struct.pack_into('<H', self.mm, off + 20, new[0] >> 16)
        struct.pack_into('<H', self.mm, off + 26, new[0] & 0xFFFF)
        return new
