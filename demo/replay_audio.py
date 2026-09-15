"""Replay a recorded RBP3 encoded stream through the current host decoder."""
import argparse
import json
from pathlib import Path
import sys

sys.path[:0]=[str(Path(__file__).resolve().parents[1]/'client/python')]
from rbp.audio_wire import AudioFormat,EncodedFragment,AudioEnd
from audio import WaveRecorder


def replay(source,target):
    source=Path(source);target=Path(target)
    if source.resolve()==target.with_suffix('.encoded.jsonl').resolve():
        raise ValueError('output would overwrite encoded input')
    recorder=WaveRecorder(target)
    def value_hook(obj):
        return bytes.fromhex(obj['hex']) if set(obj)=={'hex'} else obj
    try:
        with source.open(encoding='utf-8') as f:
            if json.loads(f.readline())!={'format':'RBP3-AUDIO-JSONL','version':1}:
                raise ValueError('unsupported recording format')
            ended=False;started=False
            for line in f:
                row=json.loads(line,object_hook=value_hook);event=row['event'];v=row['value']
                if ended:raise ValueError('record after END')
                if event=='start':
                    if started:raise ValueError('duplicate START')
                    recorder.start(AudioFormat(**v));started=True
                elif not started:raise ValueError('missing START')
                elif event=='format':recorder.format(AudioFormat(**v))
                elif event=='data':recorder.data(EncodedFragment(**v))
                elif event=='end':recorder.end(AudioEnd(**v));ended=True
                elif event=='abort':recorder.abort(v['reason']);ended=True
                else:raise ValueError('unknown recording event')
            if not started or not ended:raise ValueError('incomplete recording')
        return recorder.completed
    finally:
        recorder.abort('replay_failed')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source');parser.add_argument('output_wav')
    args=parser.parse_args()
    print(json.dumps(replay(args.source,args.output_wav),indent=2))
