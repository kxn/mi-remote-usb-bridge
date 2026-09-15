"""Offline WAV timing analysis; no third-party dependencies."""
import array
import json
import math
import statistics
import sys
import wave
from pathlib import Path

path = Path(sys.argv[1])
with wave.open(str(path), 'rb') as w:
    assert w.getnchannels() == 1 and w.getsampwidth() == 2
    rate = w.getframerate()
    samples = array.array('h', w.readframes(w.getnframes()))
if sys.byteorder != 'little':
    samples.byteswap()
window = rate // 100
levels = []
tone_levels = []
cosine = [math.cos(2*math.pi*1000*i/rate) for i in range(window)]
sine = [math.sin(2*math.pi*1000*i/rate) for i in range(window)]
for offset in range(0, len(samples) - window + 1, window):
    block = samples[offset:offset + window]
    mean = sum(block) / window
    levels.append(math.sqrt(sum((x - mean) ** 2 for x in block) / window))
    re = sum((x-mean)*c for x,c in zip(block,cosine))
    im = sum((x-mean)*s for x,s in zip(block,sine))
    tone_levels.append(2*math.hypot(re,im)/window)
# A button click can be much louder than the played tone. Use a robust
# percentile and frequency-selective envelope, not the global RMS maximum.
threshold = sorted(tone_levels)[int((len(tone_levels)-1)*0.95)] * 0.45
active = [i for i, value in enumerate(tone_levels) if value > threshold]
groups = []
for i in active:
    if not groups or i - groups[-1][-1] > 3:
        groups.append([i])
    else:
        groups[-1].append(i)
groups = [g for g in groups if len(g)>=3]
starts = [g[0] * window / rate for g in groups]
intervals = [round(b-a, 4) for a, b in zip(starts, starts[1:])]
def tone_frequency(group):
    peak = max(group, key=lambda i: tone_levels[i])
    block = samples[peak * window:(peak + 1) * window]
    mean = sum(block) / len(block)
    block = [(v-mean) * (0.5-0.5*math.cos(2*math.pi*i/(len(block)-1)))
             for i, v in enumerate(block)]
    def power(f):
        coeff = 2 * math.cos(2 * math.pi * f / rate)
        a = b = 0
        for v in block:
            current = v + coeff*a-b
            b, a = a, current
        return a*a+b*b-coeff*a*b
    return max(range(800, 1301, 5), key=power)
result = dict(path=str(path), rate=rate, samples=len(samples), duration_s=len(samples)/rate,
              threshold_tone_amplitude=threshold, pulse_count=len(groups), pulse_starts_s=starts,
              intervals_s=intervals, pulse_widths_s=[round((g[-1]-g[0]+1)*window/rate,4) for g in groups],
              median_interval_s=statistics.median(intervals) if intervals else None,
              estimated_tone_hz=[tone_frequency(g) for g in groups])
print(json.dumps(result, indent=2))
