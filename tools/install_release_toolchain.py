"""Install the pinned compiler into the ignored build directory."""
import hashlib
import json
from pathlib import Path
import shutil
import urllib.request
import zipfile


def main():
    lock = json.loads(Path('references/release-toolchain.json').read_text())
    cache = Path('build/toolchain-download')
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / 'toolchain.zip'
    if not archive.exists():
        temp = cache / 'download.tmp'
        with urllib.request.urlopen(lock['url'], timeout=120) as response, temp.open('wb') as out:
            shutil.copyfileobj(response, out)
        temp.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != lock['sha256']:
        raise ValueError('Compiler archive SHA-256 mismatch; refusing to execute')
    target = Path('build/release-toolchain').resolve()
    if target.exists():
        raise ValueError('Compiler target exists; use a clean build directory')
    with zipfile.ZipFile(archive) as z:
        for entry in z.infolist():
            relative = Path(entry.filename).relative_to(lock['root'])
            dest = (target / relative).resolve()
            if not dest.is_relative_to(target):
                raise ValueError('Unsafe archive path')
            if entry.is_dir():
                dest.mkdir(parents=True, exist_ok=True)
            else:
                dest.parent.mkdir(parents=True, exist_ok=True)
                with z.open(entry) as src, dest.open('wb') as out:
                    shutil.copyfileobj(src, out)
    print('Installed verified compiler:', target)


if __name__ == '__main__':
    main()
