"""Check documentation links, JSON examples and wire arithmetic; not firmware tests."""
import json
import pathlib
import re
import struct
import sys
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[1]
paths = [ROOT / 'README.md', ROOT / 'THIRD_PARTY_NOTICES.md', ROOT / 'references' / 'README.md']
for folder in ('docs', 'client', 'demo', 'protocol', 'releases'):
    paths.extend(p for p in (ROOT / folder).rglob('*.md') if 'build' not in p.relative_to(ROOT).parts and not any(x.endswith('.egg-info') for x in p.parts))
errors = []
examples = 0
links = 0
for path in paths:
    if not path.exists():
        errors.append(str(path) + ': missing document')
        continue
    content = path.read_text('utf-8')
    for target in re.findall(r'\]\((<[^>]+>|[^)]+)\)', content):
        target = target.strip('<>')
        if re.match(r'^[a-zA-Z][a-zA-Z0-9+.-]*:', target) or target.startswith('#'): continue
        target = urllib.parse.unquote(target.split('#')[0])
        links += 1
        if not (path.parent / target).exists():
            errors.append(path.relative_to(ROOT).as_posix() + ': broken link ' + target)
    for example in re.findall(r'(?:```|~~~)json\s*\n(.*?)\n(?:```|~~~)', content, re.S):
        examples += 1
        try: json.loads(example)
        except ValueError as exc: errors.append(str(path) + ': invalid JSON: ' + str(exc))

header = struct.calcsize('<2sBBBBHIIIHHHHI')
keys = struct.calcsize('<IQQBBH')
audio = struct.calcsize('<IIQHH')
decoded = header + 512 + 4
cobs_bound = decoded + decoded // 254 + 2
assert (header, keys, audio, decoded, cobs_bound) == (32, 24, 20, 548, 552)
assert (512 - audio) // 2 == 246
crc = 0xffffffff
for byte in b'123456789':
    crc ^= byte
    for _ in range(8): crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
assert (crc ^ 0xffffffff) == 0xe3069283

report = dict(scope='Documentation and wire arithmetic', documents=len(paths), local_links=links, errors=errors)
print(json.dumps(report, ensure_ascii=False, indent=2))
sys.exit(bool(errors))
