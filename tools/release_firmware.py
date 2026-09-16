"""Validate release inputs and package only explicitly selected firmware files."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile


def version(value):
    value = value.removeprefix('v')
    if len(value) > 31 or not re.fullmatch(
            r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[a-zA-Z0-9]+(?:[.-][a-zA-Z0-9]+)*)?', value):
        raise ValueError('Use X.Y.Z or X.Y.Z-rc.1, at most 31 characters, optionally prefixed by v')
    if value.endswith('-debug'):
        raise ValueError('Release workflow only builds non-debug firmware')
    return value


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def select_version(requested, base, used):
    used = {x.removeprefix('v') for x in used}
    if requested:
        selected = version(requested)
        if selected in used:
            raise ValueError('Version already exists (tag or release)')
        return selected
    candidate = tuple(map(int, version(base).split('.')))
    stable = [tuple(map(int, x.split('.'))) for x in used
              if re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', x)]
    if stable and max(stable) >= candidate:
        major, minor, patch = max(stable)
        candidate = major, minor, patch + 1
    return version('.'.join(map(str, candidate)))


def package(ver, build, output):
    ver = version(ver)
    if output.exists():
        raise ValueError('Output already exists; use a fresh output directory')
    config = (build / 'build-config.txt').read_text()
    for forbidden in ('-DRBP_DEBUG', '-DRBP_SDK_RX_PROBE', '-DRBP_EXPERIMENTAL_HOST_VOICE_START'):
        if forbidden in config:
            raise ValueError('Experimental/debug build cannot be released')
    if ver.encode() + b'\0' not in (build / 'ch582f.bin').read_bytes():
        raise ValueError('Requested version absent from firmware')
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
    output.mkdir(parents=True)
    stage = output / 'bundle'
    stage.mkdir()
    for ext in ('bin', 'hex', 'elf', 'map'):
        shutil.copyfile(build / f'ch582f.{ext}', stage / f'ch582f.{ext}')
    for name in ('LICENSE', 'THIRD_PARTY_NOTICES.md'):
        shutil.copyfile(name, stage / name)
    for name in ('sdk-lock.json', 'release-toolchain.json'):
        shutil.copyfile(Path('references') / name, stage / name)
    shutil.copyfile(build / 'build-config.txt', stage / 'build-config.txt')
    manifest = dict(version=ver, tag='v'+ver, commit=commit, protocol='RBP/3',
                    debug=False, sdk_rx_probe=False, host_voice_start=False,
                    files={p.name:digest(p) for p in sorted(stage.iterdir())})
    (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    notes = (f'# CH582F firmware {ver}\n\nSource commit: `{commit}`. Protocol: RBP/3.\n\n'
             'Non-debug firmware; experimental host recording and SDK probes disabled.\n'
             'Flash ch582f.bin using WCHISPTool; preserve DataFlash to retain pairing.\n'
             'The ZIP includes ELF/MAP symbols, build settings, hashes and third-party notices.\n\n'
             'Automated regression/build checks do not constitute hardware acceptance.\n'
             f'See the [source documentation](https://github.com/kxn/mi-remote-usb-bridge/tree/{commit}/docs) '
             'for hardware requirements and known limitations.\n')
    (stage / 'README.md').write_text(notes, encoding='utf-8')
    assets = output / 'assets'
    assets.mkdir()
    for ext in ('bin', 'hex'):
        shutil.copyfile(stage / f'ch582f.{ext}', assets / f'ch582f-{ver}.{ext}')
    shutil.copyfile(stage / 'manifest.json', assets / 'manifest.json')
    with zipfile.ZipFile(assets / f'ch582f-{ver}.zip', 'w', zipfile.ZIP_DEFLATED) as z:
        for p in sorted(stage.iterdir()):
            z.write(p, p.name)
    (assets / 'SHA256SUMS.txt').write_text(''.join(
        f'{digest(p)}  {p.name}\n' for p in sorted(assets.iterdir())), encoding='utf-8')
    (output / 'release-notes.md').write_text(notes, encoding='utf-8')


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('command', choices=('validate', 'package'))
    p.add_argument('--version', default=os.environ.get('RELEASE_VERSION', ''))
    p.add_argument('--build', type=Path, default=Path('build/firmware-release'))
    p.add_argument('--output', type=Path, default=Path('build/release'))
    p.add_argument('--used-versions', type=Path)
    args = p.parse_args()
    if args.command == 'validate':
        used = args.used_versions.read_text(encoding='utf-8-sig').splitlines() if args.used_versions else []
        refs = subprocess.check_output(['git', 'ls-remote', '--tags', 'origin'], text=True)
        used += [line.split('refs/tags/', 1)[1] for line in refs.splitlines() if not line.endswith('^{}')]
        base = re.search(r'^VERSION\s*\?=\s*(\S+)', Path('firmware/Makefile').read_text(), re.M)[1]
        ver = select_version(args.version, base, used)
        if os.environ.get('GITHUB_OUTPUT'):
            with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as f:
                f.write(f'version={ver}\ntag=v{ver}\n')
        print(ver)
    else:
        package(args.version, args.build, args.output)
