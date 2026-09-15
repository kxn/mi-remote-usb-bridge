"""Check RBP3 music recording continuity and cut SDK-error listening windows."""
import argparse
import array
import json
import math
from pathlib import Path
import wave

p = argparse.ArgumentParser()
p.add_argument('recording', type=Path, help='Recording WAV')
p.add_argument('--log', type=Path, required=True)
a = p.parse_args()
meta = json.loads(a.recording.with_suffix('.json').read_text(encoding='utf-8'))
sid = meta['stream_id']
rotated = [f for f in a.log.parent.glob(a.log.name + '.*') if f.suffix[1:].isdigit()]
files = sorted(rotated, key=lambda f: int(f.suffix[1:]), reverse=True) + [a.log]
rows = []
for file in files:
    lines = file.read_text(encoding='utf-8').splitlines()
    for index, line in enumerate(lines):
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            if file == a.log and index == len(lines)-1:
                continue  # Writer may currently be appending the final row.
            raise
start = next(r for r in rows if r['event'] == 'voice_start' and r['values'][0]['stream_id'] == sid)
end = next(r for r in rows if r['event'] == 'voice_end' and r['values'][0]['stream_id'] == sid)
window = [r for r in rows if start['elapsed'] <= r['elapsed'] <= end['elapsed'] + 1]
debug = [r for r in rows if r['event'] == 'firmware_debug' and start['elapsed']-1 <= r['elapsed'] <= end['elapsed']+1]
records = [v for r in debug for v in r['records']]
errors = [r for r in window if r['event'] == 'firmware_error' and r.get('sdk_code') == 134 and r.get('sdk_status') == 5]
audio = [r for r in window if r['event'] == 'voice_data' and r['stream_id'] == sid]
units = [r['value'] for l in a.recording.with_suffix('.encoded.jsonl').read_text(encoding='utf-8').splitlines() if (r := json.loads(l)).get('event') == 'data']
samples = 0
issues = []
for i, u in enumerate(units, 1):
    if (u['frame_seq'], u['unit_seq'], u['fragment_offset'], u['first_sample_index']) != (i, i, 0, samples):
        issues.append({'unit': i, 'issue': 'sequence_or_sample'})
    if len(bytes.fromhex(u['data']['hex'])) != u['unit_size']:
        issues.append({'unit': i, 'issue': 'size'})
    samples += u['unit_sample_count']
with wave.open(str(a.recording), 'rb') as w:
    params = w.getparams()
    pcm = array.array('h', w.readframes(w.getnframes()))
assert params.nchannels == 1 and params.sampwidth == 2
rate = params.framerate
out = a.recording.parent / (a.recording.stem + '-review')
out.mkdir(exist_ok=True)
clips = []
for n, error in enumerate(errors, 1):
    nearest = min(audio, key=lambda r: abs(r['elapsed']-error['elapsed']))
    # USB event arrival is an approximate association, not an exact source timestamp.
    center = nearest['first_sample_index'] / rate
    lo, hi = max(0, center-10), min(len(pcm)/rate, center+10)
    part = pcm[round(lo*rate):round(hi*rate)]
    path = out / f'error-{n}-before-after-10s.wav'
    with wave.open(str(path), 'wb') as w:
        w.setparams(params)
        w.writeframes(part.tobytes())
    gaps = [(y['elapsed']-x['elapsed'])*1000 for x, y in zip(audio, audio[1:]) if abs(y['elapsed']-error['elapsed']) < .25]
    bins = [math.sqrt(sum(x*x for x in part[i:i+160])/160) for i in range(0, len(part)-159, 160)]
    clips.append({'path': str(path.resolve()), 'sdk_check': error.get('rx_checks'),
                  'error_wall_offset_s': error['elapsed']-start['elapsed'],
                  'estimated_audio_offset_s': center, 'clip_start_s': lo, 'clip_end_s': hi,
                  'nearest_audio_arrival_delta_ms': (nearest['elapsed']-error['elapsed'])*1000,
                  'max_audio_arrival_gap_near_error_ms': max(gaps, default=None),
                  'peak': max(abs(x) for x in part), 'clipped_samples': sum(abs(x)>=32767 for x in part),
                  'zero_10ms_blocks': sum(v == 0 for v in bins),
                  'min_10ms_rms': min(bins), 'max_10ms_rms': max(bins)})
result = {'stream_id': sid, 'complete': meta['complete'], 'end_reason': meta['reason'],
          'audio_seconds': samples/rate, 'units': len(units), 'continuity_issues': issues,
          'sdk_callback_count': len(errors),
          'new_rx_failures': [r for r in records if r.get('name') == 'sdk_rx_check_failed'],
          'debug_dropped_range': [min(r['dropped'] for r in debug), max(r['dropped'] for r in debug)],
          'logger_dropped_max': max(r.get('logger_dropped_total', 0) for r in window),
          'clips': clips,
          'limitations': ['Error/audio association uses host arrival times and is approximate.',
                          'Bridge sequence continuity does not prove source continuity.',
                          'Amplitude statistics do not establish absence of audible artifacts.']}
(out / 'analysis.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(result, indent=2))
