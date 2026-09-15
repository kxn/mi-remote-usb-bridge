"""Host-side compressed recording, decoding and WAV integrity manifest."""
import dataclasses
from functools import wraps
import json
import wave
from pathlib import Path
from rbp.audio_wire import AudioStream,AudioError
from rbp.codecs import AudioDecoder

def recording_io(method):
    @wraps(method)
    def wrapped(*args,**kwargs):
        try:return method(*args,**kwargs)
        except OSError as exc:raise AudioError(f'local recording: {exc}') from exc
    return wrapped

class WaveRecorder:
    def __init__(self,target):
        self.target=Path(target);self.active=None;self.file=None;self.raw=None
        self.segments=[];self.completed=[];self.cleanup_errors=[]
    def _record(self,event,obj):
        value=dataclasses.asdict(obj) if dataclasses.is_dataclass(obj) else obj
        self.raw.write(json.dumps(dict(event=event,value=value),default=lambda b:{'hex':b.hex()},separators=(',',':'))+'\n')
    def _segment(self,fmt):
        self.rate=fmt.sample_rate;self.channels=fmt.channels
        self.path=self.target if not self.segments else self.target.with_name(f"{self.target.stem}.segment{len(self.segments)+1:03d}.wav")
        self.file=wave.open(str(self.path),'wb');self.file.setnchannels(fmt.channels)
        self.file.setsampwidth(2);self.file.setframerate(fmt.sample_rate)
        self.segments.append(dict(path=str(self.path.resolve()),rate=fmt.sample_rate,channels=fmt.channels,first_sample_index=self.samples,samples=0))
    @recording_io
    def start(self,fmt):
        if self.active is not None:self.abort('superseded')
        self.target.parent.mkdir(parents=True,exist_ok=True)
        self.decoder=AudioDecoder(fmt);self.check=AudioStream(fmt,[(fmt.codec_id,fmt.codec_revision)],fmt.max_unit_bytes)
        self.active=fmt.stream_id;self.samples=0;self.segments=[];self.epochs=[];self.cleanup_errors=[]
        self.raw_path=self.target.with_suffix('.encoded.jsonl')
        try:
            self.raw=self.raw_path.open('w',encoding='utf-8')
            self.raw.write('{"format":"RBP3-AUDIO-JSONL","version":1}\n')
            self._record('start',fmt);self.epochs.append(dataclasses.asdict(fmt));self._segment(fmt)
        except (OSError,ValueError) as exc:
            self._close_handles();self.active=None
            raise AudioError(f'local recording: {exc}') from exc
    def data(self,chunk):
        try:
            self.check.feed(chunk);self._record('data',chunk);pcm=self.decoder.data(chunk)
            if pcm:
                self.file.writeframesraw(pcm);count=len(pcm)//(2*self.channels)
                self.samples+=count;self.segments[-1]['samples']+=count
        except (ValueError,OSError) as exc:
            try:self.abort(str(exc))
            finally:raise AudioError(str(exc)) from exc
    def format(self,fmt):
        try:
            self.check.format(fmt);self.decoder.format(fmt);self._record('format',fmt)
            self.epochs.append(dataclasses.asdict(fmt))
            if (self.rate,self.channels)!=(fmt.sample_rate,fmt.channels):self.file.close();self._segment(fmt)
        except (ValueError,OSError) as exc:
            try:self.abort(str(exc))
            finally:raise AudioError(str(exc)) from exc
    def end(self,event):
        try:
            self.check.end(event);self.decoder.end(event)
            self._record('end',event);self._finish(event.reason,event.reason in (0,7,8))
        except (ValueError,OSError) as exc:
            self.abort(str(exc))
            raise AudioError(str(exc)) from exc

    def _close_handles(self):
        # Detach first: cleanup is idempotent even if close/flush itself fails.
        handles=(self.file,self.raw);self.file=None;self.raw=None
        for handle in handles:
            if handle is not None:
                try:handle.close()
                except Exception as exc:self.cleanup_errors.append(str(exc))

    def abort(self,reason):
        """Best-effort cleanup; never let a secondary file error mask failure."""
        if self.active is None:return
        try:
            if self.raw:self._record('abort',dict(reason=reason))
        except Exception as exc:self.cleanup_errors.append(str(exc))
        self._finish(reason,False,quiet=True)

    def _finish(self,reason,complete,quiet=False):
        stream_id=self.active;self.active=None;self._close_handles()
        if self.cleanup_errors:complete=False
        manifest=dict(protocol=3,stream_id=stream_id,complete=complete,reason=reason,samples=self.samples,
                      encoded_bytes=self.check.encoded_bytes,complete_units=self.check.unit-1,
                      declared_samples=self.check.samples,segments=self.segments,epochs=self.epochs,
                      cleanup_errors=list(self.cleanup_errors),encoded_path=str(self.raw_path.resolve()))
        self.completed=list(self.segments)
        try:self.target.with_suffix('.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2,default=lambda b:{'hex':b.hex()}),encoding='utf-8')
        except Exception as exc:self.cleanup_errors.append(str(exc))
        if self.cleanup_errors and not quiet:raise AudioError('local recording cleanup: '+'; '.join(self.cleanup_errors))
