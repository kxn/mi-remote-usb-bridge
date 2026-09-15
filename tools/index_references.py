"""Build searchable derivatives and inventory; originals are preserved."""
import hashlib
import html.parser
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools' / '_vendor'))
from pypdf import PdfReader

class TextExtractor(html.parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.skip = 0
        self.parts = []
    def handle_starttag(self, tag, attrs):
        if tag in ('script', 'style'): self.skip += 1
        if tag in ('p', 'div', 'h1', 'h2', 'h3', 'li', 'tr', 'br', 'pre'): self.parts.append('\n')
    def handle_endtag(self, tag):
        if tag in ('script', 'style') and self.skip: self.skip -= 1
    def handle_data(self, data):
        if not self.skip: self.parts.append(data)

ref = ROOT / 'references'
previous = {i['source']: i for i in json.loads((ref / 'text-index.json').read_text('utf-8'))} if (ref / 'text-index.json').exists() else {}
paths = list((ref / 'documents').glob('*'))
paths += list((ref / 'sources' / 'wch-ch583' / 'Datasheet').glob('*'))
paths += list((ref / 'sources' / 'wch-ch583' / 'EVT' / 'PUB').glob('*.pdf'))
paths += list((ref / 'sources' / 'wch-ch583' / 'EVT' / 'EXAM' / 'BLE').glob('*.pdf'))
paths += list((ref / 'sources' / 'weact-ble-core' / 'Doc' / 'CH582').rglob('*.PDF'))
paths += list((ref / 'sources' / 'weact-ble-core' / 'HDK').glob('*57x*pdf'))
paths += list((ref / 'sources' / 'usb-cdc-1.2').rglob('*.pdf'))
results = []
for p in sorted(set(paths)):
    if not p.is_file() or p.suffix.lower() not in ('.pdf', '.html'): continue
    relative = p.relative_to(ref)
    out = ref / 'text' / relative.with_suffix('.txt')
    out.parent.mkdir(parents=True, exist_ok=True)
    info = {'source': p.relative_to(ROOT).as_posix(), 'text': out.relative_to(ROOT).as_posix()}
    try:
        if out.exists() and out.stat().st_mtime >= p.stat().st_mtime and previous.get(info['source'], {}).get('status') == 'ok':
            info = previous[info['source']]
            results.append(info)
            continue
        if p.suffix.lower() == '.pdf':
            pdf = PdfReader(p)
            info['pages'] = len(pdf.pages)
            with out.open('w', encoding='utf-8') as f:
                f.write('Source: ' + info['source'] + '\nText extraction only; consult original for diagrams/tables.\n')
                for n, page in enumerate(pdf.pages, 1):
                    f.write(f'\n--- PDF PAGE {n} ---\n' + (page.extract_text() or '') + '\n')
        else:
            parser = TextExtractor()
            parser.feed(p.read_text('utf-8', errors='replace'))
            lines = [' '.join(s.split()) for s in ''.join(parser.parts).splitlines()]
            out.write_text('Source: ' + info['source'] + '\n\n' + '\n'.join(s for s in lines if s) + '\n', encoding='utf-8')
        info['status'] = 'ok'
    except Exception as exc:
        info.update(status='failed', error=str(exc))
        if p.suffix.lower() == '.pdf':
            try:
                import pymupdf
                with pymupdf.open(p) as pdf, out.open('w', encoding='utf-8') as f:
                    info['pages'] = len(pdf)
                    f.write('Source: ' + info['source'] + '\nMuPDF fallback extraction; consult original for diagrams/tables.\n')
                    for n, page in enumerate(pdf, 1):
                        f.write(f'\n--- PDF PAGE {n} ---\n' + page.get_text() + '\n')
                info.update(status='ok', extractor='pymupdf-fallback', pypdf_error=info.pop('error'))
            except Exception as fallback:
                info['error'] += '; fallback: ' + str(fallback)
    results.append(info)
    print(relative, info['status'], flush=True)
    (ref / 'text-index.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
(ref / 'text-index.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
inventory = []
manifest = json.loads((ref / 'manifest.json').read_text('utf-8'))
lines = ['# 下载目录（自动生成）', '', '原件下载状态、固定revision和URL见manifest.json。源码/文本不代表已编译或已验收。', '', '| ID | 本地原件 | 固定版本 | 原URL | 说明 |', '|---|---|---|---|---|']
for entry in sorted(manifest, key=lambda x: x['id']):
    path = entry.get('path', '').removeprefix('references/')
    lines.append(f"| {entry['id']} | [{entry.get('status')}]({path}) | {entry.get('revision', '见文件/元数据')} | [URL]({entry.get('url', '')}) | {entry.get('note', '')} |")
(ref / 'CATALOG.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
for p in sorted(ref.rglob('*')):
    if not p.is_file() or p.name in ('file-inventory.json', 'README.md', 'manifest.json', 'sources.json') or p.suffix == '.part': continue
    with p.open('rb') as f: checksum = hashlib.file_digest(f, 'sha256').hexdigest()
    inventory.append({'path': p.relative_to(ROOT).as_posix(), 'bytes': p.stat().st_size, 'sha256': checksum})
(ref / 'file-inventory.json').write_text(json.dumps(inventory, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print('Indexed', len(results), 'documents;', len(inventory), 'files')
