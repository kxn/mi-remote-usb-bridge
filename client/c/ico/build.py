"""Build the native ICO library and offline demo with GCC/Clang (no shell)."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
DEFAULT = ROOT.parents[2] / 'build' / 'ico'

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', default=None)
    ap.add_argument('--out', type=Path, default=DEFAULT)
    args = ap.parse_args()
    cc = args.cc or (str(Path('C:/msys64/mingw64/bin/gcc.exe'))
                     if os.name == 'nt' and Path('C:/msys64/mingw64/bin/gcc.exe').exists()
                     else shutil.which('cc'))
    if not cc:
        ap.error('Specify --cc with a native GCC/Clang compiler')
    subprocess.run([sys.executable, str(ROOT/'fetch_reference.py'), '--out', str(ROOT/'g7221')], check=True)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    sources = ['common/basic_op.c', 'common/common.c', 'common/huff_tab.c',
               'common/tables.c', 'decode/coef2sam.c', 'decode/dct4_s.c', 'decode/decoder.c']
    base = [cc, '-O2', '-fwrapv', '-I', str(ROOT/'pj_shim'), '-I', str(ROOT),
            '-I', str(ROOT/'g7221/common'), '-I', str(ROOT/'g7221/decode')]
    base += [str(ROOT/'g7221'/s) for s in sources]
    env = dict(os.environ)
    env['PATH'] = str(Path(cc).resolve().parent) + os.pathsep + env.get('PATH', '')
    lib = out / ('ico.dll' if os.name == 'nt' else 'libico.dylib' if sys.platform == 'darwin' else 'libico.so')
    subprocess.run(base + ['-shared', '-fPIC', str(ROOT/'ico_library.c'), '-o', str(lib)], env=env, check=True)
    generic = out / ('rbp_decoder.dll' if os.name == 'nt' else 'librbp_decoder.dylib' if sys.platform == 'darwin' else 'librbp_decoder.so')
    subprocess.run(base + ['-shared', '-fPIC', '-DRBP_WITH_ICO', '-DRBP_DECODER_EXPORTS',
        '-I', str(ROOT.parents[2]/'protocol/include'), '-I', str(ROOT.parent),
        str(ROOT/'ico_library.c'), str(ROOT.parent/'rbp_decoder.c'), str(ROOT.parent/'ima_decoder.c'),
        '-o', str(generic)], env=env, check=True)
    demo = out / ('ico_decoder.exe' if os.name == 'nt' else 'ico_decoder')
    subprocess.run(base + [str(ROOT/'ico_decoder.c'), '-o', str(demo)], env=env, check=True)
    print(lib)
    print(generic)
    print(demo)

if __name__ == '__main__':
    main()
