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


def ihx2bin(path, fill=0xFF):
    """Intel HEX -> (lowest address, bytes from it, gaps `fill`)."""
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
    return lo, bytes(mem.get(i, fill) for i in range(lo, hi + 1))


def areas(map_text):
    """Area name -> (start, size) from an sdld map."""
    out = {}
    for name, start, size in re.findall(
            r'^(_\w+)\s+([0-9A-F]{8})\s+([0-9A-F]{8})', map_text, re.M):
        out[name] = (int(start, 16), int(size, 16))
    return out


def tools():
    """sdcc and sdasz80 of SDCC 4.3 or newer: $SDCC_HOME/bin, else
    ~/.local/opt/sdcc-4.4.0/bin, else PATH. The FTView code of WC Improved
    needs 4.3+: 4.2 does not fit it into the banks, crashes on the video bank
    and splits UTF-8 comments of __asm."""
    import os
    home = os.environ.get('SDCC_HOME') or str(Path.home() / '.local/opt/sdcc-4.4.0')
    path = os.pathsep.join([str(Path(home) / 'bin'), os.environ.get('PATH', '')])
    sdcc, sdas = shutil.which('sdcc', path=path), shutil.which('sdasz80', path=path)
    if not sdcc or not sdas:
        fail('sdcc / sdasz80 not found (SDCC_HOME=%s)' % home)
    ver = subprocess.run([sdcc, '--version'], capture_output=True, text=True).stdout
    m = re.search(r' (\d+)\.(\d+)\.(\d+) ', ver)
    if not m or (int(m[1]), int(m[2])) < (4, 3):
        fail('%s is %s, SDCC 4.3 or newer is needed (SDCC_HOME)' % (sdcc, m[0].strip() if m else '?'))
    return sdcc, sdas


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


def build(out, target, sources, header, data_loc=DATA_LOC, crt0=None):
    check_vendor()
    sdcc, sdas = tools()
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

    run([sdas, '-o', 'crt0.rel', str(crt0 or ROOT / 'src/crt0.s')], out, log)
    rels = []
    for src in sources:
        rel = Path(src).stem + '.rel'
        run([sdcc] + common + inc + ['-c', str(src), '-o', rel], out, log)
        rels.append(rel)
    cc = [sdcc] + common + ['crt0.rel', '--code-loc', hex(CODE_LOC), '--data-loc', hex(data_loc)]
    run(cc + rels + libs + ['-o', 'out.ihx'], out, log)
    # The libraries of a newer SDCC put code into _HOME; with --code-loc the
    # linker places it after _DATA (past #C000), so it is linked again right
    # after _CODE.
    a = areas((out / 'out.map').read_text())
    home = a['_CODE'][0] + a['_CODE'][1]
    run(cc + ['-Wl-b_HOME=0x%04X' % home] + rels + libs + ['-o', 'out.ihx'], out, log)
    a = areas((out / 'out.map').read_text())
    code_end = max(s + n for name, (s, n) in a.items()
                   if name in ('_HEADER0', '_HOME', '_CODE', '_INITIALIZER', '_GSINIT', '_GSFINAL') and n)
    data_end = max((s + n for name, (s, n) in a.items()
                    if name in ('_DATA', '_INITIALIZED', '_BSEG', '_BSS') and n), default=data_loc)
    # crt0 does not run GSINIT: initialised non-const globals would stay zero.
    if a.get('_INITIALIZER', (0, 0))[1]:
        fail('initialised non-const globals (%d bytes of _INITIALIZER): make them const'
             % a['_INITIALIZER'][1])
    if code_end > data_loc:
        fail('code ends at #%04X, past the data at #%04X' % (code_end, data_loc))
    if data_end > PAGE_END:
        fail('data ends at #%04X, past #%04X' % (data_end, PAGE_END))

    lo, image = ihx2bin(out / 'out.ihx')
    if lo != 0x8000:
        fail('image starts at #%04X, not #8000' % lo)
    blocks = (len(image) + 511) // 512
    if blocks > MAX_BLOCKS:
        fail('image takes %d blocks of 512 bytes, a WC page holds %d' % (blocks, MAX_BLOCKS))
    image += bytes(blocks * 512 - len(image))

    run([sdcc] + common + inc + ['-DN_BLK0=%d' % blocks, str(header),
                                 '-o', 'hdr.ihx'], out, log)
    _, hdr = ihx2bin(out / 'hdr.ihx')
    if len(hdr) > 512:
        fail('header is %d bytes, not 512' % len(hdr))
    hdr += bytes(512 - len(hdr))
    if hdr[16:32] != b'WildCommanderMDL':
        fail('header has no WildCommanderMDL magic')
    target.write_bytes(hdr + image)
    print('%s: %d bytes, code #8000..#%04X, data #%04X..#%04X, %d blocks' % (
        target.name, len(hdr) + len(image), code_end, data_loc, data_end, blocks))


# --- the plugin: code banks -------------------------------------------------
# Page 0 (images, AVI header, seek table, message windows), page 9 (the AVI
# player), page 10 (I/O: the SD reader); buffer pages 1..8. WC loads page 0 into #8000;
# the gate wc_video_bank (#BF80) switches to page 9 by API 79 (MNG8_PL),
# far_call back for the rare operations. Each bank: code #8006..#BD00 (the
# video bank #BB80: its HUD template hud_cmd is there), DATA #BD00..#BF80,
# the gates from #BF80 and the entry frame #BFE0..#BFF1 equal in both.
BANKS = (('image', 0, ['src/main.c'], []),
         ('video', 9, ['src/main.c'], ['-DFTVIEW_VIDEO_BANK']),
         ('io', 10, ['src/io_bank.c', 'src/sdr.c'], []))
FIXED = {'_fs_buf': 0xC000, '_ret_sp': 0xBFE0, '_ret_ix': 0xBFE2, '_ret_iy': 0xBFE4,
         '_filesize': 0xBFE6, '_file_ext': 0xBFEA, '_call_type': 0xBFEB,
         '_gs_loaded': 0xBFEC, '_ft_state': 0xBFED, '_scr_w': 0xBFEE, '_scr_h': 0xBFF0,
         '_wc_video_bank': 0xBF80}
SDR_STATE_ADDR = 0xFE00       # buffer page 1 tail (sdr.h)


def gs_driver(gen, sdas, log):
    """src/gsdrv.s, the General Sound driver run at #5000 by the GS CPU, as a
    C array (gsdrv.h) for gs_zx.c."""
    sdld = shutil.which('sdldz80', path=str(Path(sdas).parent))
    run([sdas, '-plosgffw', 'gsdrv.rel', str(ROOT / 'src/gsdrv.s')], gen, log)
    run([sdld, '-n', '-m', '-w', '-i', 'gsdrv.ihx', 'gsdrv.rel'], gen, log)
    org, code = ihx2bin(gen / 'gsdrv.ihx')
    sym = {n: int(a, 16) for a, n in re.findall(r'^\s+([0-9A-F]{8})\s+(\w+)',
                                                (gen / 'gsdrv.map').read_text(), re.M)}
    # Not into the DAC window #6000.., the IM2 table #5C00..#5D00; the handler
    # exactly at #5050 (the table points there).
    if org != 0x5000 or org + len(code) > 0x5C00:
        fail('gsdrv at #%04X, %d bytes' % (org, len(code)))
    if sym.get('gs_isr') != 0x5050 or sym.get('gs_init_end', 0x10000) > 0x5050:
        fail('gsdrv: gs_isr / gs_init_end misplaced')
    version = int(re.search(r'^VERSION\s*=\s*(\d+)', (ROOT / 'src/gsdrv.s').read_text(), re.M)[1])
    (gen / 'gsdrv.h').write_text(
        '// Made by tools/build.py from src/gsdrv.s: the General Sound driver.\n'
        '#define GS_DRIVER_ORG 0x%04X\n#define GS_DRIVER_VERSION %d\n'
        'static const u8 gs_driver[%d] = {\n%s\n};\n' % (
            org, version, len(code),
            ',\n'.join(', '.join('0x%02X' % b for b in code[i:i + 16]) for i in range(0, len(code), 16))))


def build_plugin(out, target):
    check_vendor()
    sdcc, sdas = tools()
    if out.exists():
        shutil.rmtree(out)
    gen = out / 'gen'
    gen.mkdir(parents=True)
    abi_headers(gen)
    log = open(out / 'build.log', 'w')
    digest = hashlib.sha256()
    for f in sorted((ROOT / 'src').iterdir()):
        if f.is_file():
            digest.update(f.name.encode() + f.read_bytes())
    build_id = digest.hexdigest()[:4]
    (gen / 'build_id.h').write_text('#define BUILD_ID "%s"\n' % build_id)
    gs_driver(gen, sdas, log)
    inc = ['-I' + str(gen), '-I' + str(ROOT / 'src')]
    inc += ['-I' + str(ROOT / 'vendor' / p) for p in ('sdk', 'tsconf', 'ft812')]
    common = ['-mz80', '--std-sdcc11', '--sdcccall', '1', '--opt-code-size', '--no-std-crt0']
    libs = [str(ROOT / 'vendor' / p / n) for p, n in LIBS]
    run([sdas, '-o', 'crt0.rel', str(ROOT / 'src/crt0.s')], out, log)
    payloads = []
    for bank, page, sources, defines in BANKS:
        (out / bank).mkdir()
        rels = []
        for src in sources:
            rel = '%s/%s.rel' % (bank, Path(src).stem)
            run([sdcc] + common + inc + defines + ['-DSDR_STATE_ADDR=0x%04X' % SDR_STATE_ADDR,
                                                   '-c', str(ROOT / src), '-o', rel], out, log)
            rels.append(rel)
        link = [sdcc] + common + ['--code-loc', '0x8006', '--data-loc', '0xBD00', 'crt0.rel'] + rels \
            + libs + ['-o', bank + '/ftview.ihx']
        run(link, out, log)
        a = areas((out / bank / 'ftview.map').read_text())
        run(link + ['-Wl-b_HOME=0x%04X' % sum(a['_CODE'])], out, log)
        mp = (out / bank / 'ftview.map').read_text()
        a = areas(mp)
        sym = {n: int(v, 16) for v, n in re.findall(r'^\s+([0-9A-F]{8})\s+(_\w+)\s', mp, re.M)}
        fixed = dict(FIXED, **({'_hud_cmd': 0xBB80} if page == 9 else {}))
        if page == 10:
            del fixed['_fs_buf']
        for name, addr in fixed.items():
            if sym.get(name) != addr:
                fail('%s: %s at %s, not #%04X' % (bank, name, sym.get(name), addr))
        limit = 0xBB80 if page == 9 else 0xBD00
        for name, (start, size) in a.items():
            if not size:
                continue
            if name in ('_CODE', '_HOME'):
                if start < 0x8006 or start + size > limit:
                    fail('%s: %s #%04X..#%04X past #%04X (%d bytes over)'
                         % (bank, name, start, start + size, limit, start + size - limit))
            elif name == '_DATA':
                if start != 0xBD00 or start + size > 0xBF80:
                    fail('%s: DATA #%04X..#%04X past #BF80' % (bank, start, start + size))
            elif name == '_INITIALIZER':
                fail('%s: initialised non-const globals (%d bytes): make them const' % (bank, size))
            elif name.startswith(('_DABS', '_HEADER', '_BANKSWITCH')):
                pass
            else:
                fail('%s: unexpected section %s (%d bytes)' % (bank, name, size))
        # The whole page (the gates sit near its end), gaps and DATA zero.
        lo, image = ihx2bin(out / bank / 'ftview.ihx', fill=0)
        if lo != 0x8000 or len(image) > 0x4000 or any(image[0x3D00:0x3F80]):
            fail('%s: image #%04X, %d bytes, or bytes in DATA' % (bank, lo, len(image)))
        image = image.ljust(0x4000, b'\0')
        payloads.append(image)
        print('%s bank: code #8006..#%04X (limit #%04X), DATA #BD00..#%04X' % (
            bank, max(sum(a['_CODE']), sum(a.get('_HOME', (0, 0)))), limit,
            sum(a.get('_DATA', (0xBD00, 0)))))
    # The gates (#BF80..#BFC7): equal but the address of main_dispatch of
    # wc_video_bank (#BFA5); entry #8003 is a JP far_entry in each bank.
    for other in payloads[1:]:
        if payloads[0][0x3F80:0x3FA5] != other[0x3F80:0x3FA5] or \
                payloads[0][0x3FA7:0x3FC8] != other[0x3FA7:0x3FC8] or other[3] != 0xC3:
            fail('the bank gates differ')
    run([sdcc] + common + inc + ['-DN_BLK0=32', '-DN_BLK9=32', '-DN_BLK10=32', str(ROOT / 'src/wc_h.c'), '-o', 'hdr.ihx'],
        out, log)
    _, hdr = ihx2bin(out / 'hdr.ihx')
    hdr = hdr.ljust(512, b'\0')
    if len(hdr) != 512 or hdr[16:32] != b'WildCommanderMDL' or hdr[34] != 11 or \
            hdr[36:44] != bytes([0, 32, 9, 32, 10, 32, 1, 0]):
        fail('WMF header')
    target.write_bytes(hdr + b''.join(payloads))
    print('%s: %d bytes, build %s' % (target.name, 512 + 0x4000 * len(payloads), build_id))


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-o', '--output', default=str(ROOT / 'FTVIEW.WMF'))
    ap.add_argument('--build-dir', default=str(ROOT / 'build' / 'plugin'))
    ap.add_argument('--crt0')
    ap.add_argument('--data-loc', type=lambda s: int(s, 0), default=DATA_LOC)
    ap.add_argument('--header', default=str(ROOT / 'src/wc_h.c'))
    ap.add_argument('sources', nargs='*')
    args = ap.parse_args()
    if not args.sources:
        build_plugin(Path(args.build_dir), Path(args.output))
        sys.exit(0)
    build(Path(args.build_dir), Path(args.output), [Path(s).resolve() for s in args.sources],
          Path(args.header).resolve(), args.data_loc,
          Path(args.crt0).resolve() if args.crt0 else None)
