#!/usr/bin/env python3
"""Build FTVIEW.WMF with the SDCC of the system (4.2 or newer).

ABI: the prebuilt libraries of vendor/ (pentevo/sdk/ft812sdk/lib) and the
__naked functions of src/wc_api.c take their arguments on the stack (ABI 0,
SDCC before 4.1.12). The C code and the standard library of a newer SDCC use
ABI 1 (registers). A global --sdcccall 0 breaks the calls into the standard
library (memcmp, the 32-bit multiply), so the C code stays on ABI 1 and the
SDK prototypes get __sdcccall(0): build/abi has copies of the SDK headers
with it; vendor/ is not changed (its files are checked against
vendor/manifest.json).

Layout of the plugin page: code from #8003 (the jump of crt0 at #8000), data
from DATA_LOC up to #C000. WC loads the page into one 16 KB window, so the
image (#8000 up to the end of the initialised part) must fit in 32 blocks of
512 bytes. The header (src/wc_h.c, 512 bytes) goes first in the file.
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CODE_LOC = 0x8003
DATA_LOC = 0xA800
PAGE_END = 0xC000
MAX_BLOCKS = 32
LIBS = (('ft812', 'ft812.lib'), ('tsconf', 'ts.lib'), ('sdk', 'sdk.lib'))
HEADERS = (('ft812', 'ft812lib.h'), ('tsconf', 'tslib.h'), ('sdk', 'sdklib.h'))


def fail(msg):
    sys.exit('build: ' + msg)


def run(args, cwd, log):
    p = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    log.write(' '.join(map(str, args)) + '\n' + p.stdout + p.stderr)
    if p.returncode:
        fail('%s failed:\n%s%s' % (args[0], p.stdout, p.stderr))


def ihx2bin(path):
    """Intel HEX -> (lowest address, bytes from it, gaps FF)."""
    mem, base = {}, 0
    for line in path.read_text().split():
        if not line.startswith(':'):
            continue
        b = bytes.fromhex(line[1:])
        n, a, t = b[0], (b[1] << 8) | b[2], b[3]
        if t == 0:
            for i in range(n):
                mem[base + a + i] = b[4 + i]
        elif t == 4:
            base = ((b[4] << 8) | b[5]) << 16
    lo, hi = min(mem), max(mem)
    return lo, bytes(mem.get(i, 0xFF) for i in range(lo, hi + 1))


def areas(map_text):
    """Area name -> (start, size) from an sdld map."""
    out = {}
    for name, start, size in re.findall(
            r'^(_\w+)\s+([0-9A-F]{8})\s+([0-9A-F]{8})', map_text, re.M):
        out[name] = (int(start, 16), int(size, 16))
    return out


def check_vendor():
    manifest = json.loads((ROOT / 'vendor/manifest.json').read_text())
    for name, digest in manifest['files'].items():
        if hashlib.sha256((ROOT / 'vendor' / name).read_bytes()).hexdigest() != digest:
            fail('vendor/%s differs from vendor/manifest.json' % name)


def abi_headers(abi):
    abi.mkdir(parents=True, exist_ok=True)
    for part, name in HEADERS:
        text = (ROOT / 'vendor' / part / name).read_text()
        text = re.sub(r'^(?!typedef)([^\n;{}]+\([^\n;{}]*\))\s*;',
                      r'\1 __sdcccall(0);', text, flags=re.M)
        (abi / name).write_text(text)


def build(out, target):
    check_vendor()
    sdcc, sdas = shutil.which('sdcc'), shutil.which('sdasz80')
    if not sdcc or not sdas:
        fail('sdcc and sdasz80 must be in PATH')
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    abi = out / 'abi'
    abi_headers(abi)
    log = open(out / 'build.log', 'w')
    inc = ['-I' + str(abi), '-I' + str(ROOT / 'src')]
    inc += ['-I' + str(ROOT / 'vendor' / p) for p in ('sdk', 'tsconf', 'ft812')]
    common = ['-mz80', '--sdcccall', '1', '--std-sdcc11', '--opt-code-speed', '--no-std-crt0']
    libs = [str(ROOT / 'vendor' / p / n) for p, n in LIBS]

    run([sdas, '-o', 'crt0.rel', str(ROOT / 'src/crt0.s')], out, log)
    cc = [sdcc] + common + inc + ['crt0.rel', '--code-loc', hex(CODE_LOC),
                                  '--data-loc', hex(DATA_LOC)]
    run(cc + [str(ROOT / 'src/main.c')] + libs + ['-o', 'out.ihx'], out, log)
    # The libraries of a newer SDCC put code into _HOME; with --code-loc the
    # linker places it after _DATA (past #C000), so it is linked again right
    # after _CODE.
    a = areas((out / 'out.map').read_text())
    home = a['_CODE'][0] + a['_CODE'][1]
    run(cc + ['-Wl-b_HOME=0x%04X' % home, str(ROOT / 'src/main.c')] + libs
        + ['-o', 'out.ihx'], out, log)
    a = areas((out / 'out.map').read_text())
    code_end = max(s + n for name, (s, n) in a.items()
                   if name in ('_HEADER0', '_HOME', '_CODE', '_INITIALIZER', '_GSINIT', '_GSFINAL') and n)
    data_end = max((s + n for name, (s, n) in a.items()
                    if name in ('_DATA', '_INITIALIZED', '_BSEG', '_BSS') and n), default=DATA_LOC)
    if code_end > DATA_LOC:
        fail('code ends at #%04X, past the data at #%04X' % (code_end, DATA_LOC))
    if data_end > PAGE_END:
        fail('data ends at #%04X, past #%04X' % (data_end, PAGE_END))

    lo, image = ihx2bin(out / 'out.ihx')
    if lo != 0x8000:
        fail('image starts at #%04X, not #8000' % lo)
    blocks = (len(image) + 511) // 512
    if blocks > MAX_BLOCKS:
        fail('image takes %d blocks of 512 bytes, a WC page holds %d' % (blocks, MAX_BLOCKS))
    image += bytes(blocks * 512 - len(image))

    run([sdcc] + common + inc + ['-DN_BLK0=%d' % blocks, str(ROOT / 'src/wc_h.c'),
                                 '-o', 'hdr.ihx'], out, log)
    _, hdr = ihx2bin(out / 'hdr.ihx')
    if len(hdr) > 512:
        fail('header is %d bytes, not 512' % len(hdr))
    hdr += bytes(512 - len(hdr))
    if hdr[16:32] != b'WildCommanderMDL':
        fail('header has no WildCommanderMDL magic')
    target.write_bytes(hdr + image)
    print('%s: %d bytes, code #8000..#%04X, data #%04X..#%04X, %d blocks' % (
        target.name, len(hdr) + len(image), code_end, DATA_LOC, data_end, blocks))


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-o', '--output', default=str(ROOT / 'FTVIEW.WMF'))
    ap.add_argument('--build-dir', default=str(ROOT / 'build'))
    args = ap.parse_args()
    build(Path(args.build_dir), Path(args.output))
