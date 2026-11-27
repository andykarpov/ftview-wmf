"""ESP32 audio client (src/esp.c) on the Z80 model: the register protocol of
ZiFi32 and its audio player as test/espsim.py of espplay-wmf (late M2S, lost
W_END, FREE), the bytes of the stream against what was staged. Run from the
repository root: make test."""
import re
import struct
import subprocess
import sys
from pathlib import Path

import z80

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'build' / 'test'
WMF = OUT / 'ESPTEST.WMF'
MAP = OUT / 'esptest' / 'out.map'
RET = 0x7000
OP = dict(DETECT=0x41, START=0x42, STAGE=0x43, POLL=0x44, PAUSE=0x45, STOP=0x46, END=0x47)
SHARED = struct.Struct('<BBBBHHBBHII20s')
ST_IDLE, ST_READY, ST_BUSY, ST_M2S = 0, 1, 2, 3
A_IDLE, A_WAIT, A_PLAY, A_DRAIN, A_ENDED = range(5)

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print('FAIL:', what)


class Esp:
    """The ESP on the SPI: WR_REGS / RD_REGS / WR_DATA / W_END and the player."""

    def __init__(self, present=True, audio=True, in_size=512 * 1024, busy_polls=2,
                 late_push=(), late_polls=70000, lost_wend=(), drain_per_state=4096):
        self.present, self.audio, self.in_size = present, audio, in_size
        self.busy_polls, self.late_push, self.late_polls = busy_polls, set(late_push), late_polls
        self.lost_wend, self.drain = set(lost_wend), drain_per_state
        self.regs = bytearray(64)
        self.cs = False
        self.out = bytearray()
        self.rd_ptr, self.rd_skip = None, 0
        self.busy_left = self.arm_in = 0
        self.armed = False
        self.data_rx = None
        self.pushes = self.wends = 0
        self.state, self.fmt, self.ring, self.played, self.end, self.paused = A_IDLE, 0, 0, 0, False, False
        self.streams = []
        self.cmds = []

    def port_in(self):
        if not self.present or not self.cs or self.rd_ptr is None:
            return 0xFF
        if self.rd_skip:
            self.rd_skip -= 1
            return 0xFF
        r = self.rd_ptr
        self.rd_ptr += 1
        if r == 1 and self.arm_in:
            self.arm_in -= 1
            if not self.arm_in:
                self.armed = True
                self.regs[1] = ST_M2S
            return self.regs[1]
        if r == 1 and self.busy_left:
            self.busy_left -= 1
            return ST_BUSY
        return self.regs[r & 63]

    def ctrl(self, v):
        on = v == 0x13
        if self.cs and not on:
            self.frame_end()
        if on and not self.cs:
            self.out = bytearray()
            self.rd_ptr = None
        self.cs = on

    def data(self, v):
        if not self.cs or not self.present:
            return
        self.out.append(v)
        if len(self.out) == 2 and self.out[0] == 2:
            self.rd_ptr, self.rd_skip = self.out[1], 2

    def frame_end(self):
        o = self.out
        if not o:
            return
        if o[0] == 1 and len(o) >= 2:
            for i, v in enumerate(o[2:]):
                self.regs[(o[1] + i) & 63] = v
            if o[1] == 0 and len(o) >= 3:
                self.command(o[2])
        elif o[0] == 3:
            self.data_rx = bytes(o[1:])
        elif o[0] == 7:
            k = self.wends
            self.wends += 1
            if k not in self.lost_wend:
                self.push_done()

    def status(self, s):
        self.regs[1] = s
        self.busy_left = self.busy_polls

    def put_state(self):
        struct.pack_into('<BBBBIIIII', self.regs, 0x10, self.state, self.fmt, 1, 1 if self.paused else 0,
                         self.in_size - self.ring, 22050, self.played, 0, self.ring)

    def command(self, c):
        self.cmds.append(c)
        if not self.audio and 0x90 <= c <= 0x97:
            self.status(0x80)
            return
        if c == 0x90:
            self.fmt, self.state, self.ring, self.played, self.end = self.regs[2], A_WAIT, 0, 0, False
            self.streams.append(bytearray())
        elif c == 0x91:
            n = struct.unpack_from('<I', self.regs, 3)[0]
            if self.state not in (A_WAIT, A_PLAY) or self.end:
                self.put_state()
                self.status(0x82)
                return
            if not 0 < n <= 16384 or n > self.in_size - self.ring:
                self.put_state()
                self.status(0x84)
                return
            self.push_n = n
            k = self.pushes
            self.pushes += 1
            if k in self.late_push:
                self.arm_in = self.late_polls
                return
            self.armed = True
            self.regs[1] = ST_M2S
            self.busy_left = self.busy_polls
            return
        elif c == 0x92:
            self.end = True
        elif c == 0x94:
            self.paused = bool(self.regs[2])
        elif c == 0x96:
            self.state, self.ring = A_IDLE, 0
        elif c == 0x97:
            # the decoder drains some of the ring between two polls
            if self.state in (A_WAIT, A_PLAY) and not self.paused:
                d = min(self.ring, self.drain)
                self.ring -= d
                self.played += d
                if self.ring:
                    self.state = A_PLAY
                if self.end and not self.ring:
                    self.state = A_ENDED
        else:
            self.status(0x80)
            return
        self.put_state()
        self.status(ST_READY)

    def push_done(self):
        if not self.armed or self.data_rx is None:
            if not self.armed:
                self.data_rx = None
            return
        self.armed = False
        d = self.data_rx[:self.push_n]
        assert len(d) == self.push_n, 'PUSH: %d bytes sent, %d announced' % (len(self.data_rx), self.push_n)
        assert len(self.data_rx) == (self.push_n + 3) & ~3, 'PUSH of %d not padded to 4' % self.push_n
        self.streams[-1] += d
        self.ring += len(d)
        self.data_rx = None
        self.put_state()
        self.status(ST_READY)


class Machine:
    def __init__(self, esp):
        self.cpu = z80.Z80Machine()
        self.sym = {n: int(a, 16) for a, n in
                    re.findall(r'^\s+([0-9A-F]{8})\s+(_\w+)', MAP.read_text(), re.M)}
        self.cpu.set_memory_block(0x8000, bytes(0x8000))
        self.cpu.set_memory_block(0x8000, WMF.read_bytes()[512:])
        self.esp = esp
        self.cpu.set_input_callback(self.input)
        self.cpu.set_output_callback(self.output)
        self.cpu.set_breakpoint(RET)

    def input(self, port):
        return self.esp.port_in() if port & 0xFF == 0x57 else 0xFF

    def output(self, port, v):
        if port & 0xFF == 0x77:
            self.esp.ctrl(v)
        elif port & 0xFF == 0x57:
            self.esp.data(v)

    def run(self, ops, data=b''):
        m = self.cpu
        base = self.sym['_t_ops']
        for i, o in enumerate(ops):
            blk = SHARED.pack(OP[o[0]], 0, o[1] if len(o) > 1 else 0, o[2] if len(o) > 2 else 0,
                              o[3] if len(o) > 3 else 0, o[4] if len(o) > 4 else 0, 0, 0, 0, 0, 0,
                              (o[5] if len(o) > 5 else b'').ljust(20, b'\0'))
            m.set_memory_block(base + i * SHARED.size, blk)
        m.set_memory_block(self.sym['_t_n'], bytes([len(ops)]))
        assert len(ops) <= 24 and len(data) <= 0x1800, 'test too big'
        m.set_memory_block(self.sym['_t_data'], data)
        m.sp = 0x5D00
        m.set_memory_block(m.sp, RET.to_bytes(2, 'little'))
        m.pc = self.sym['_main']
        ticks = 0
        while m.pc != RET:
            m.ticks_to_stop = 1_000_000
            m.run()
            ticks += 1_000_000 - m.ticks_to_stop
            assert ticks < 3_000_000_000, 'execution limit at %#x' % m.pc
        assert not self.esp.cs, 'ESP chip select left on'
        return [SHARED.unpack_from(bytes(m.memory[base + i * SHARED.size:base + (i + 1) * SHARED.size]))
                for i in range(len(ops))], ticks


def build():
    subprocess.run([sys.executable, '-I', str(ROOT / 'tools/build.py'), '--build-dir', str(OUT / 'esptest'),
                    '--crt0', str(ROOT / 'diag/crt0.s'), '--header', str(ROOT / 'diag/sdrdiag_h.c'),
                    '--data-loc', '0xBC00', '-o', str(WMF), str(ROOT / 'test/esp_main.c'), str(ROOT / 'src/esp.c')],
                   check=True, stdout=subprocess.DEVNULL)


IMA_WFX = struct.pack('<HHIIHHHH', 0x11, 1, 43000, 21574, 1024, 4, 2, 2041)


def stream_case(name, fmt, wfx, pieces, esp, polls=6, end=True):
    data = bytes((i * 7 + 3) & 0xFF for i in range(sum(pieces)))
    ops = [('START', fmt, 0, 0, 0, wfx)]
    pos = 0
    for n in pieces:
        ops.append(('STAGE', 0, 0, pos, n))
        ops.append(('POLL',))
        pos += n
    ops += [('POLL',)] * polls
    if end:
        ops += [('END',)] + [('POLL',)] * polls
    m = Machine(esp)
    res, ticks = m.run(ops, data)
    got = bytes(esp.streams[-1]) if esp.streams else b''
    hdr = b''
    if fmt == 1:
        ext = min(wfx[16], 2)
        hdr = b'RIFF\xF8\xFF\xFF\x7FWAVEfmt ' + bytes([18 + ext, 0, 0, 0]) + wfx[:16] + bytes([ext, 0]) + \
            wfx[18:18 + ext] + b'data\xD0\xFF\xFF\x7F'
    check(res[0][1] == 1, '%s: START refused' % name)
    check(got == hdr + data, '%s: stream differs (%d of %d bytes)' % (name, len(got), len(hdr) + len(data)))
    check(res[-1][8] == 0, '%s: %d bytes left staged' % (name, res[-1][8]))
    if end:
        check(esp.cmds.count(0x92) == 1, '%s: AUDIO_END %d times' % (name, esp.cmds.count(0x92)))
    print('%-40s %6d bytes  %4d ms' % (name, len(got), ticks // 14000))
    return res


def main():
    build()
    # detect
    for name, esp, want in (('no ESP', Esp(present=False), 0), ('ESP without the player', Esp(audio=False), 0),
                            ('ESP with the player', Esp(), 1)):
        res, _ = Machine(esp).run([('DETECT',)])
        check(res[0][1] == want, 'detect %s: ok %d' % (name, res[0][1]))
        print('%-40s ok %d' % ('detect: ' + name, res[0][1]))
    stream_case('WAV IMA ADPCM, 1 KB pieces', 1, IMA_WFX, [1024] * 4, Esp())
    stream_case('WAV PCM (cbSize 0), odd pieces', 1, struct.pack('<HHIIHHH', 1, 1, 22050, 22050, 1, 8, 0) + b'\0\0',
                [1, 3, 517, 1023, 2], Esp())
    stream_case('MP3, a piece over the ring end', 2, b'', [2600, 1, 2500, 777], Esp())
    stream_case('small FREE: partial pushes', 2, b'', [2000, 2000, 2000], Esp(in_size=1500, drain_per_state=1000), polls=8)
    stream_case('late M2S of the 2nd PUSH', 2, b'', [1000, 1000, 1000], Esp(late_push=(1,), late_polls=300))
    stream_case('lost W_END of the 1st PUSH', 2, b'', [1000, 1000], Esp(lost_wend=(0,)))
    # a full ring refuses a STAGE
    esp = Esp(in_size=16)
    res, _ = Machine(esp).run([('START', 2), ('STAGE', 0, 0, 0, 4096), ('STAGE', 0, 0, 0, 1024),
                               ('STAGE', 0, 0, 0, 1024)], bytes(4096))
    check([r[1] for r in res] == [1, 1, 1, 0], 'full ring: STAGE results %s' % [r[1] for r in res])
    print('%-40s %s' % ('full ring refuses a STAGE', [r[1] for r in res]))
    print('test_esp: %d errors' % failures)
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
