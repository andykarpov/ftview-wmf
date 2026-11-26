"""Z80 model of the plugin on TS-Conf: the Z-Controller SPI (#77 / #57) with
SD cards, the DMA SPI -> RAM, and the WC API calls the reader uses
(GIPAGPL, LOAD512, CHTOSEP) on a file of a Fat32 image."""
import re
from collections import deque
from pathlib import Path

import z80

API = 0x6006
RET = 0x7000          # return address of the tested main()
TXT_PAGE = 0x8F       # the text screen of WC


class SDCard:
    """SD card in SPI mode: CMD58, CMD17, CMD18, CMD12. ready=False: not
    initialised (R1 = idle for every command)."""

    def __init__(self, mm, hc=True, ready=True):
        self.mm, self.hc, self.ready = mm, hc, ready
        self.selected = False
        self.q = deque()
        self.cmd = bytearray()
        self.reading = None       # next LBA of CMD18
        self.reads = []           # LBAs read (data blocks sent)
        self.cmds = []
        self.errors = []

    def select(self, on):
        if self.selected and not on:
            self.cmd.clear()
            if self.reading is not None:
                self.errors.append('deselected during CMD18')
        self.selected = on

    def block(self, lba):
        self.reads.append(lba)
        return [0xFF, 0xFE] + list(self.mm[lba * 512:lba * 512 + 512]) + [0x12, 0x34]

    def xfer(self, b):
        if not self.selected:
            return 0xFF
        if not self.q and self.reading is not None:
            self.q.extend(self.block(self.reading))
            self.reading += 1
        out = self.q.popleft() if self.q else 0xFF
        if self.cmd or (b & 0xC0) == 0x40:
            self.cmd.append(b)
            if len(self.cmd) == 6:
                self.execute()
                self.cmd.clear()
        return out

    def lba(self, arg):
        if self.hc:
            return arg
        if arg % 512:
            self.errors.append('byte address %#x not aligned' % arg)
        return arg // 512

    def execute(self):
        c = self.cmd[0] & 0x3F
        arg = int.from_bytes(self.cmd[1:5], 'big')
        self.cmds.append((c, arg))
        if c == 12:
            self.q.clear()
            self.reading = None
            self.q.extend([0xFF, 0x00, 0x00, 0x00, 0xFF])
            return
        self.q.clear()
        if not self.ready:
            self.q.extend([0xFF, 0x01])
            return
        if c == 58:
            self.q.extend([0xFF, 0x00, 0xC0 if self.hc else 0x80, 0xFF, 0x80, 0x00])
        elif c in (17, 18):
            lba = self.lba(arg)
            if (lba + 1) * 512 > len(self.mm):
                self.q.extend([0xFF, 0x40])
            elif c == 17:
                self.q.extend([0xFF, 0x00] + self.block(lba))
            else:
                self.q.extend([0xFF, 0x00])
                self.reading = lba
        else:
            self.q.extend([0xFF, 0x04])


class Machine:
    def __init__(self, wmf, mapfile, fat, name, cards):
        """wmf / mapfile: the test binary and its map; fat, name: the file WC
        started the plugin with; cards: {1: SDCard or None, 2: ...}."""
        self.cpu = z80.Z80Machine()
        self.sym = {n: int(a, 16) for a, n in
                    re.findall(r'^\s+([0-9A-F]{8})\s+(_\w+)', Path(mapfile).read_text(), re.M)}
        image = Path(wmf).read_bytes()[512:]
        self.cpu.set_memory_block(0x8000, image)
        self.cards = cards
        self.conf = 0x03
        self.spi = 0xFF
        self.dma = {}
        self.pages = {}
        self.fat, self.name = fat, name
        self.data = fat.data(name)
        self.lbas = fat.file_lbas(name)
        _, self.first, _ = fat.find(name)
        self.pos = 0
        self.api_calls = []
        self.ticks = 0
        self.cpu.memory[0x6003] = 0x05
        self.cpu.set_input_callback(self.input)
        self.cpu.set_output_callback(self.output)
        self.cpu.set_breakpoint(API)
        self.cpu.set_breakpoint(RET)

    # --- memory ---------------------------------------------------------
    def page(self, n):
        return self.pages.setdefault(n, bytearray(16384))

    def poke(self, name, value, size):
        self.cpu.set_memory_block(self.sym[name], value.to_bytes(size, 'little'))

    def peek(self, name, size=1, offset=0):
        a = self.sym[name] + offset
        return int.from_bytes(bytes(self.cpu.memory[a:a + size]), 'little')

    # --- ports ----------------------------------------------------------
    def exchange(self, v):
        out = 0xFF
        for card in self.cards.values():
            if card and card.selected:
                out &= card.xfer(v)
        return out

    def set_conf(self, v):
        self.conf = v
        if self.cards.get(1):
            self.cards[1].select(not v & 2)
        if self.cards.get(2):
            self.cards[2].select(bool(v & 8))

    def output(self, port, v):
        lo = port & 0xFF
        if lo == 0x77:
            self.set_conf(v)
        elif lo == 0x57:
            self.spi = self.exchange(v)
        elif lo == 0xAF:
            reg = port >> 8
            self.dma[reg] = v
            if reg == 0x27:
                self.run_dma(v)

    def input(self, port):
        lo = port & 0xFF
        if lo == 0x57:
            r, self.spi = self.spi, self.exchange(0xFF)
            return r
        if lo == 0x77:
            return 0
        if port == 0x27AF:
            return 0          # DMA finished
        return 0xFF

    def run_dma(self, ctrl):
        assert ctrl == 0x42, 'DMA mode %#x' % ctrl
        n = (self.dma.get(0x26, 0) + 1) * 2 * ((self.dma.get(0x28, 0) | (self.dma.get(0x2C, 0) & 3) << 8) + 1)
        dst = self.dma[0x1F]
        # the page mapped at #C000 is in the Z80 memory
        page = self.cpu.memory[0xC000:] if dst == self.cpu.memory[0x6003] else self.page(dst)
        a = (self.dma[0x1E] << 8 | self.dma[0x1D]) & 0x3FFF
        assert a + n <= 16384, 'DMA past the page'
        for i in range(n):
            page[a + i], self.spi = self.spi, self.exchange(0xFF)

    # --- WC API ---------------------------------------------------------
    def ret(self):
        m = self.cpu
        m.pc = m.memory[m.sp] | m.memory[m.sp + 1] << 8
        m.sp += 2

    def map_c000(self, new):
        m = self.cpu
        self.pages[m.memory[0x6003]] = bytearray(m.memory[0xC000:0x10000])
        m.set_memory_block(0xC000, bytes(self.page(new)))
        m.memory[0x6003] = new

    def api(self):
        m = self.cpu
        a = m.a
        self.api_calls.append(a)
        if a == 0x32:                       # GIPAGPL
            self.pos = 0
        elif a == 0x30:                     # LOAD512
            n = m.b * 512
            chunk = self.data[self.pos:self.pos + n].ljust(n, b'\0')
            m.set_memory_block(m.hl, chunk)
            self.pos += n
            m.hl = (m.hl + n) & 0xFFFF
        elif a == 0x34:                     # CHTOSEP: whole clusters
            de, end = m.de, m.bc
            for c in self.fat.chain(self.first):
                for i in range(self.fat.spc):
                    m.set_memory_block(de, (self.fat.lba(c) + i).to_bytes(4, 'little'))
                    de += 4
                if de >= end:
                    break
        elif a == 0x00:                     # MNGC_PL: plugin page A' at #C000
            self.map_c000(0x90 + (m.alt_a & 0x3F))
        elif a == 0x17:                     # ESC: not pressed (Z)
            m.f |= 0x40
        elif a in (0x01, 0x02, 0x03):       # PRWOW / RRESB / PRSRW: a real window in IX
            assert m.memory[m.ix] == 1 and m.memory[m.ix + 4], 'window at %#x is empty' % m.ix
            self.map_c000(TXT_PAGE)         # they leave the text page of WC in #C000
            m.memory[0xC000] ^= 0xFF        # and draw into it
        elif a in (0x0E, 0x0F, 0x2E, 0x2F): # clock, screen, keys
            pass
        else:
            raise AssertionError('WC API %#x' % a)
        self.ret()

    def run(self, limit=2_000_000_000, regs=None):
        m = self.cpu
        for name, value in (regs or {}).items():
            setattr(m, name, value)
        m.sp = 0x5D00
        m.set_memory_block(m.sp, RET.to_bytes(2, 'little'))
        m.pc = self.sym['_main']
        while self.ticks < limit:
            m.ticks_to_stop = 70_000
            m.run()
            self.ticks += 70_000 - m.ticks_to_stop
            m.memory[0x6009] = (self.ticks // 280_000) & 0xFF   # frames of WC at 14 MHz
            if m.pc == RET:
                assert self.conf == 0x03, 'chip select left on: #77 = %#x' % self.conf
                return
            if m.pc == API:
                self.api()
        raise AssertionError('execution limit at %#x' % m.pc)
