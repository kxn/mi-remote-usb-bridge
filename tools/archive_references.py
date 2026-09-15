"""Archive explicit URLs and pinned GitHub snapshots. No search engine is used.

python tools/archive_references.py          # fetch missing/failed items
python tools/archive_references.py --verify # verify downloaded originals offline
python tools/archive_references.py --restore # re-extract saved archives offline
Sources are data only: downloaded programs are never executed.
"""
import concurrent.futures
import base64
import datetime
import hashlib
import json
import pathlib
import sys
import subprocess
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
REF = ROOT / 'references'
PLAN = REF / 'sources.json'
MANIFEST = REF / 'manifest.json'

def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def request(url):
    return urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'RemoteBridge-ReferenceArchive/1.0'}), timeout=45)

def fetch(item, old):
    entry = dict(item)
    try:
        previous = old.get(item['id'], {})
        if previous.get('status') == 'ok':
            p = ROOT / previous['path']
            if p.is_file() and digest(p) == previous['sha256']:
                return previous
        if 'repo' in item:
            revision = previous.get('revision') or item.get('revision')
            if not revision:
                with request('https://api.github.com/repos/' + item['repo'] + '/commits/' + item.get('ref', 'HEAD')) as r:
                    revision = json.load(r)['sha']
            entry['revision'] = revision
            entry['url'] = 'https://codeload.github.com/' + item['repo'] + '/zip/' + revision
            entry['path'] = 'references/archives/' + item['id'] + '-' + revision[:12] + '.zip'
        target = ROOT / entry['path']
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_name(target.name + '.part')
        transfer = subprocess.run(['curl.exe' if sys.platform == 'win32' else 'curl',
            '--location', '--fail', '--silent', '--show-error', '--connect-timeout', '15',
            '--max-time', '180', '--output', str(temporary), '--write-out', '%{json}', entry['url']],
            capture_output=True, text=True, timeout=190)
        if transfer.returncode:
            raise RuntimeError(transfer.stderr.strip())
        metadata = json.loads(transfer.stdout)
        entry['resolved_url'] = metadata['url_effective']
        entry['content_type'] = metadata.get('content_type') or ''
        if entry.get('format') == 'pdf' and not temporary.read_bytes().startswith(b'%PDF-'):
            raise ValueError('URL did not return PDF')
        if entry.get('expected_sha256') and digest(temporary) != entry['expected_sha256']:
            raise ValueError('publisher checksum mismatch')
        if 'repo' in item or entry.get('format') == 'zip':
            with zipfile.ZipFile(temporary) as z:
                if z.testzip():
                    raise ValueError('ZIP integrity error')
                dest = REF / 'sources' / item['id']
                for member in z.infolist():
                    # Source archives only; no symlinks or traversal are extracted.
                    relative = pathlib.PurePosixPath(member.filename)
                    parts = relative.parts[1:] if 'repo' in item else relative.parts
                    if not parts or member.is_dir():
                        continue
                    p = dest.joinpath(*parts).resolve()
                    if not p.is_relative_to(dest.resolve()):
                        raise ValueError('unsafe archive path')
                    if (member.external_attr >> 16) & 0o170000 == 0o120000:
                        continue
                    p.parent.mkdir(parents=True, exist_ok=True)
                    p.write_bytes(z.read(member))
                entry['extracted_to'] = dest.relative_to(ROOT).as_posix()
        temporary.replace(target)
        if entry.get('format') == 'base64':
            decoded = ROOT / entry['decoded_path']
            decoded.parent.mkdir(parents=True, exist_ok=True)
            decoded.write_bytes(base64.b64decode(target.read_bytes(), validate=True))
        entry.update(status='ok', size=target.stat().st_size, sha256=digest(target), retrieved_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    except Exception as exc:
        entry.update(status='failed', error=str(exc))
    print(entry['id'], entry['status'], entry.get('size', entry.get('error')), flush=True)
    return entry

def main():
    existing = json.loads(MANIFEST.read_text('utf-8')) if MANIFEST.exists() else []
    if '--restore' in sys.argv:
        restored = 0
        for item in existing:
            if item.get('status') != 'ok': continue
            original = ROOT / item['path']
            if not original.is_file() or digest(original) != item['sha256']:
                raise ValueError('Missing or changed original: ' + item['id'])
            if item.get('extracted_to'):
                dest = (ROOT / item['extracted_to']).resolve()
                with zipfile.ZipFile(original) as archive:
                    for member in archive.infolist():
                        parts = pathlib.PurePosixPath(member.filename).parts
                        if 'repo' in item: parts = parts[1:]
                        if not parts or member.is_dir() or ((member.external_attr >> 16) & 0o170000) == 0o120000: continue
                        target = dest.joinpath(*parts).resolve()
                        if not target.is_relative_to(dest) or any(':' in part for part in parts):
                            raise ValueError('Unsafe archive path')
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(archive.read(member))
                        restored += 1
            if item.get('decoded_path'):
                target = ROOT / item['decoded_path']
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(base64.b64decode(original.read_bytes(), validate=True))
                restored += 1
        print('Restored', restored, 'source files from verified local originals; text can be rebuilt with index_references.py')
        return 0
    if '--verify' in sys.argv:
        errors = []
        for item in existing:
            if item['status'] != 'ok':
                errors.append(item['id'] + ': ' + item.get('error', 'not archived'))
            elif not (ROOT / item['path']).is_file() or digest(ROOT / item['path']) != item['sha256']:
                errors.append(item['id'] + ': checksum mismatch/missing')
        inventory_path = REF / 'file-inventory.json'
        inventory = json.loads(inventory_path.read_text('utf-8')) if inventory_path.exists() else []
        for item in inventory:
            path = ROOT / item['path']
            if not path.is_file() or digest(path) != item['sha256']:
                errors.append(item['path'] + ': extracted/derived checksum mismatch')
        print(json.dumps({'items': len(existing), 'inventory_files': len(inventory), 'errors': errors}, ensure_ascii=False, indent=2))
        return bool(errors)
    plan = json.loads(PLAN.read_text('utf-8'))
    old = {i['id']: i for i in existing}
    # Concurrency is only across independent, uniquely named downloads.
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        futures = {pool.submit(fetch, item, old): item for item in plan}
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            old[result['id']] = result
            MANIFEST.write_text(json.dumps(list(old.values()), ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return any(i.get('status') != 'ok' for i in old.values())

if __name__ == '__main__':
    sys.exit(main())
