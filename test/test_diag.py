"""The board check plugin diag/sdrdiag.c on the Z80 model: it must find the
file, read it both ways and report EQUAL."""
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fatimg import make_image          # noqa: E402
from tsmodel import Machine, SDCard    # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'build' / 'test'


def main():
    subprocess.run([sys.executable, '-I', str(ROOT / 'tools/build.py'), '--build-dir', str(OUT / 'diag'),
                    '--header', str(ROOT / 'diag/sdrdiag_h.c'), '-o', str(OUT / 'SDRTEST.WMF'),
                    str(ROOT / 'diag/sdrdiag.c'), str(ROOT / 'src/sdr.c')], check=True, stdout=subprocess.DEVNULL)
    data = bytes(range(256)) * 700 + b'tail'
    fat = make_image(OUT / 'img' / 'diag.img', 40, 1, 0, [('CLIP.SDT', data)])
    fat.scatter('CLIP.SDT', 3)
    m = Machine(OUT / 'SDRTEST.WMF', OUT / 'diag' / 'out.map', fat, 'CLIP.SDT', {1: SDCard(fat.mm), 2: None})
    m.run(regs={'a': 0, 'alt_a': 0, 'hl': len(data) & 0xFFFF, 'de': len(data) >> 16})
    a = m.sym['_text']
    text = bytes(m.cpu.memory[a:a + 640]).split(b'\0')[0].decode('ascii', 'replace').replace('\r', '\n')
    print(text)
    ok = 'result OK' in text and 'EQUAL' in text and '3 extents' in text
    print('test_diag: %d errors' % (0 if ok else 1))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
