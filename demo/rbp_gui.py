#!/usr/bin/env python3
"""Reference GUI; USB and heartbeat stay on BridgeWorker, outside Qt UI work."""
import queue
import sys
import time
import logging
from datetime import datetime
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"client/python"))
from PySide6 import QtCore, QtWidgets, QtMultimedia
from rbp.worker import BridgeWorker
from rbp.faults import decode_faults
from rbp.transport import SerialTransport,TcpTransport
from audio import WaveRecorder
from input_actions import InputMapper,WindowsKeys,MacKeys


class Window(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Remote Bridge — Buttons & Voice")
        self.resize(900,650)
        self.worker=None;self.pending=[];self.device=None;self.catalog=[]
        self.connecting=False;self.next_stats=0;self.last_fault_sequence=None
        self.mapper=None;self.mapping=None;self.recorder=None;self.record_dir=None
        self.search_id=None;self.scan_generation=0;self.pair_operation=None;self.prompts=[];self.last_file=None
        root=QtWidgets.QWidget();self.setCentralWidget(root);layout=QtWidgets.QVBoxLayout(root)
        row=QtWidgets.QHBoxLayout();layout.addLayout(row)
        self.port=QtWidgets.QComboBox();self.port.setEditable(True);row.addWidget(self.port,2)
        self.refresh_ports()
        self.button(row,"Refresh",self.refresh_ports)
        self.button(row,"Open",self.open_bridge)
        self.button(row,"Close",self.close_bridge)
        self.status=QtWidgets.QLabel("Open a serial port, or enter tcp://127.0.0.1:45731 for the simulator.")
        self.status.setWordWrap(True);layout.addWidget(self.status)
        row=QtWidgets.QHBoxLayout();layout.addLayout(row)
        self.button(row,"Scan",self.scan)
        self.candidates=QtWidgets.QComboBox();row.addWidget(self.candidates,3)
        self.pair_button=self.button(row,"Pair selected",self.pair);self.pair_button.setEnabled(False)
        self.button(row,"Forget binding",self.forget)
        self.button(row,"Connect remote",lambda:self.submit("connect_peer"))
        self.button(row,"Disconnect remote",lambda:self.submit("disconnect"))
        self.discovery=QtWidgets.QLabel("Not scanned");layout.addWidget(self.discovery)
        self.reconnect=QtWidgets.QCheckBox("Automatic remote reconnect");layout.addWidget(self.reconnect)
        self.reconnect.clicked.connect(self.set_reconnect)
        self.keys=QtWidgets.QTableWidget(0,3);self.keys.setHorizontalHeaderLabels(["Key ID","Button","State"])
        self.keys.horizontalHeader().setSectionResizeMode(QtWidgets.QHeaderView.Stretch)
        self.keys.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers);layout.addWidget(self.keys,3)
        row=QtWidgets.QHBoxLayout();layout.addLayout(row)
        self.button(row,"Load mapping…",self.load_mapping)
        self.inject=QtWidgets.QCheckBox("Enable mapped OS actions");row.addWidget(self.inject)
        self.inject.toggled.connect(self.enable_injection)
        row.addStretch()
        self.record=QtWidgets.QCheckBox("Record next voice streams…");row.addWidget(self.record)
        self.record.toggled.connect(self.enable_recording)
        self.button(row,"Stop current voice",self.stop_voice)
        self.button(row,"Play last WAV",self.play)
        self.log=QtWidgets.QPlainTextEdit();self.log.setReadOnly(True);self.log.setMaximumBlockCount(500)
        tabs=QtWidgets.QTabWidget();tabs.addTab(self.log,"Log")
        diagnostics=QtWidgets.QWidget();diag_layout=QtWidgets.QVBoxLayout(diagnostics)
        self.diag_status=QtWidgets.QLabel("Open the bridge to read release diagnostics.")
        self.diag_status.setWordWrap(True);diag_layout.addWidget(self.diag_status)
        self.fault_table=QtWidgets.QTableWidget(0,7)
        self.fault_table.setHorizontalHeaderLabels(["Sequence","Board ms","Source","Stage","Raw code","Context","Count"])
        self.fault_table.horizontalHeader().setSectionResizeMode(QtWidgets.QHeaderView.ResizeToContents)
        self.fault_table.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        diag_layout.addWidget(self.fault_table)
        tabs.addTab(diagnostics,"Diagnostics (release)");layout.addWidget(tabs,2)
        self.player=QtMultimedia.QMediaPlayer(self);self.output=QtMultimedia.QAudioOutput(self)
        self.player.setAudioOutput(self.output)
        self.timer=QtCore.QTimer(self);self.timer.timeout.connect(self.poll);self.timer.start(25)

    @staticmethod
    def button(row,text,callback):
        button=QtWidgets.QPushButton(text);button.clicked.connect(callback);row.addWidget(button);return button

    def note(self,text):
        self.log.appendPlainText(f"{datetime.now():%H:%M:%S} {text}")
        logging.getLogger("rbp.gui").info("%s",text)

    def refresh_ports(self):
        from serial.tools import list_ports
        old=self.port.currentText();self.port.clear()
        self.port.addItems([p.device for p in list_ports.comports()])
        if old:self.port.setEditText(old)

    def submit(self,method,*args,done=None):
        if not self.worker:self.note("Open the bridge first.");return
        try:self.pending.append((self.worker.submit(method,*args),done))
        except Exception as exc:self.note(exc)

    def open_bridge(self):
        self.close_bridge()
        address=self.port.currentText().strip()
        try:
            if address.startswith("tcp://"):
                host,port=address[6:].rsplit(":",1);transport=TcpTransport(host,int(port))
            else:transport=SerialTransport(address)
            self.worker=BridgeWorker(transport)
            self.connecting=True;self.status.setText("Connecting — HELLO…")
            self.submit("hello",done=self.hello_ready)
        except Exception as exc:self.note(exc)

    def hello_ready(self,info):
        self.connecting=False;self.next_stats=0;self.last_fault_sequence=None
        self.note(f"Connected; firmware {info[3]}")
        self.submit("get_device",done=self.on_device)
        self.submit("get_peer",done=self.on_peer)

    def release_input(self):
        if self.mapper:
            try:self.mapper.release()
            except Exception as exc:self.note(exc)

    def close_bridge(self):
        self.release_input()
        if self.recorder:self.recorder.abort("bridge_closed")
        for dialog in list(self.prompts):dialog.close()
        self.prompts=[]
        if self.worker:
            try:self.worker.close()
            except Exception as exc:self.note(exc)
        self.worker=None;self.pending=[];self.device=None;self.catalog=[]
        self.connecting=False;self.next_stats=0;self.last_fault_sequence=None
        self.clear_discovery("Bridge closed")
        self.keys.setRowCount(0)
        self.record.blockSignals(True);self.record.setChecked(False);self.record.blockSignals(False)
        self.status.setText("Bridge closed")
        self.diag_status.setText("Bridge closed. Previous records remain in the log.")
        self.fault_table.setRowCount(0)

    def on_stats(self,stats):
        faults=decode_faults(stats)
        sequence=stats.get(6,0)
        self.diag_status.setText(
            f"Updated {datetime.now():%H:%M:%S} · Protocol errors {stats.get(1,0)} · "
            f"Input resets {stats.get(2,0)} · Voice overruns {stats.get(3,0)} · "
            f"Voice errors {stats.get(4,0)} · Reset reason {stats.get(5,0)}\n"
            f"Last {len(faults)}/4 records · Evicted {stats.get(8,0)} · "
            "Records are diagnostic events, not necessarily operation failures.")
        if sequence==self.last_fault_sequence:return
        self.last_fault_sequence=sequence
        self.fault_table.setRowCount(len(faults))
        for row,fault in enumerate(faults):
            stage=f"0x{fault['stage']:04X} {fault['stage_name']}".strip()
            values=(fault['sequence'],fault['board_ms'],fault['source'],stage,
                    fault['code_hex'],f"0x{fault['context']:08X}",fault['count'])
            for col,value in enumerate(values):
                self.fault_table.setItem(row,col,QtWidgets.QTableWidgetItem(str(value)))
        if stats.get(8,0):self.note(f"Diagnostic history evicted records: {stats[8]}")
        for fault in faults:self.note(f"Diagnostic: {fault}")

    def on_peer(self,peer):
        self.reconnect.blockSignals(True);self.reconnect.setChecked(peer["auto_reconnect"]);self.reconnect.blockSignals(False)

    def on_device(self,device):
        previous=self.device;self.device=device
        if device.message and (previous is None or previous.message!=device.message):
            self.note(f"Device: {device.message}")
        states=["Unbound","Disconnected","Connecting","Pairing","Initializing","Ready","Unsupported","Error"]
        self.status.setText(f"{states[device.state]} · {device.name} · Battery {device.battery if device.battery<=100 else '?'}% · Voice state {device.voice_state} · {device.message}")
        changed=previous is None or (previous.connection_id,previous.catalog_revision)!=(device.connection_id,device.catalog_revision)
        if changed:
            self.release_input()
            if device.state==5:self.submit("key_catalog",done=self.on_catalog)
            else:self.catalog=[];self.keys.setRowCount(0)

    def on_catalog(self,catalog):
        self.catalog=catalog;self.keys.setRowCount(len(catalog))
        for i,key in enumerate(catalog):
            for col,value in enumerate((str(key.key_id),key.name,"Released")):
                self.keys.setItem(i,col,QtWidgets.QTableWidgetItem(value))
        self.submit("keys_snapshot",done=self.on_keys)
        self.submit("events_enable",True)

    def on_keys(self,state):
        for i,key in enumerate(self.catalog):
            self.keys.item(i,2).setText("Pressed" if state.pressed_bits&(1<<key.slot) else "Released")
        if self.mapper and self.inject.isChecked() and self.device:
            if state.kind==2:self.release_input()
            else:self.mapper.update(self.device.model_id,self.catalog,state.pressed_bits)

    def clear_discovery(self,message):
        self.scan_generation+=1;self.search_id=None
        self.candidates.clear();self.pair_button.setEnabled(False);self.discovery.setText(message)

    def scan(self):
        if not self.worker:self.note("Open the bridge first.");return
        self.clear_discovery("Scanning…")
        generation=self.scan_generation
        self.submit("find_start",3000,done=lambda search:self.scanning(search,generation))

    def scanning(self,search,generation):
        if generation!=self.scan_generation:return
        self.search_id=search;self.note("Scanning…")

    def list_candidates(self,cursor=0):
        search=self.search_id;generation=self.scan_generation
        self.submit("find_list",search,cursor,done=lambda result:self.show_candidates(result,search,generation))

    def show_candidates(self,result,search,generation):
        if generation!=self.scan_generation or search!=self.search_id:return
        cursor,candidates=result
        for candidate in candidates:
            self.candidates.addItem(f"{candidate.name} · signal {candidate.signal}",(search,candidate.candidate_id))
        if cursor!=255:self.list_candidates(cursor)
        else:
            count=self.candidates.count()
            self.discovery.setText(f"Found {count} device(s). Select one, then click Pair selected." if count else "No devices found. Put the remote in pairing mode and scan again.")
            self.pair_button.setEnabled(count>0)

    def pair(self):
        candidate=self.candidates.currentData()
        if not self.pair_button.isEnabled() or candidate is None or candidate[0]!=self.search_id:return
        self.pair_button.setEnabled(False)
        self.discovery.setText("Pairing selected device…")
        self.submit("pair_begin",*candidate,done=self.pair_started)

    def pair_started(self,operation):
        self.pair_operation=operation;self.note(f"Pairing operation {operation}")

    def prompt(self,prompt):
        dialog=QtWidgets.QDialog(self);dialog.setWindowTitle("Confirm pairing")
        dialog.setProperty('operation_id',prompt['operation_id'])
        layout=QtWidgets.QVBoxLayout(dialog)
        method=prompt['method'];number=prompt.get('number',0)
        layout.addWidget(QtWidgets.QLabel(f"Enter {number:06d} on the remote." if method==3 else
                                        f"Confirm pairing: {number:06d}" if method==2 else "Enter the remote's passkey."))
        entry=QtWidgets.QLineEdit();entry.setMaxLength(6)
        if method==1:layout.addWidget(entry)
        buttons=QtWidgets.QDialogButtonBox(QtWidgets.QDialogButtonBox.Cancel)
        if method!=3:buttons.addButton(QtWidgets.QDialogButtonBox.Ok)
        layout.addWidget(buttons);buttons.accepted.connect(dialog.accept);buttons.rejected.connect(dialog.reject)
        timer=QtCore.QTimer(dialog);timer.setSingleShot(True)
        def expired():
            dialog.setProperty('operation_finished',True);dialog.reject()
        timer.timeout.connect(expired);timer.start(prompt['remaining_ms'])
        def finished(result):
            timer.stop()
            if dialog in self.prompts:self.prompts.remove(dialog)
            if not dialog.property('operation_finished'):
                accept=result==QtWidgets.QDialog.Accepted
                passkey=None
                if method==1:
                    value=entry.text();accept=accept and len(value)==6 and value.isascii() and value.isdecimal()
                    if accept:passkey=int(value)
                if method==3:self.submit('pair_cancel',prompt['operation_id'])
                else:self.submit('pair_reply',prompt['operation_id'],prompt['prompt_id'],accept,passkey)
            dialog.deleteLater()
        dialog.finished.connect(finished);self.prompts.append(dialog);dialog.open()

    def forget(self):
        self.submit("get_peer",done=lambda peer:self.submit("forget_peer",peer["peer_id"]) if peer["peer_id"] else self.note("No saved binding"))

    def set_reconnect(self,enabled):
        self.submit("get_peer",done=lambda peer:self.submit("set_reconnect",peer["peer_id"],enabled))

    def load_mapping(self):
        path,_=QtWidgets.QFileDialog.getOpenFileName(self,"Mapping file","","JSON (*.json)")
        if not path:return
        try:self.mapping=InputMapper.load(path);self.inject.setChecked(False);self.note(f"Mapping loaded: {path}")
        except Exception as exc:self.note(exc)

    def enable_injection(self,enabled):
        self.release_input();self.mapper=None
        if not enabled:return
        try:
            if self.mapping is None:raise ValueError("Load a mapping first")
            self.mapper=InputMapper(WindowsKeys() if sys.platform=="win32" else MacKeys(),self.mapping)
        except Exception as exc:self.note(exc);self.inject.setChecked(False)

    def enable_recording(self,enabled):
        if enabled:
            directory=QtWidgets.QFileDialog.getExistingDirectory(self,"Save recordings")
            if not directory:self.record.setChecked(False);return
            self.record_dir=Path(directory)
        else:
            if self.recorder:self.recorder.abort("consumer_disabled")
            self.recorder=None
        self.submit("voice_enable",enabled)

    def stop_voice(self):
        if self.recorder and self.recorder.active:self.submit("voice_stop",self.recorder.active)

    def play(self):
        if self.last_file:
            self.player.setSource(QtCore.QUrl.fromLocalFile(str(self.last_file)));self.player.play()

    def poll(self):
        if self.worker and self.device and not self.connecting and time.monotonic()>=self.next_stats:
            self.next_stats=time.monotonic()+2
            self.submit("get_stats",done=self.on_stats)
        for future,done in list(self.pending):
            if future.done():
                self.pending.remove((future,done))
                try:
                    result=future.result()
                    if done:done(result)
                except Exception as exc:
                    self.note(exc)
                    self.clear_discovery("Request failed. Scan again to select a device.")
                    if self.connecting:
                        self.close_bridge();self.status.setText("Connection failed — click Open to retry.")
                        return
        if not self.worker:return
        for _ in range(128):
            try:name,args=self.worker.next_event(0)
            except queue.Empty:break
            try:
                if name=="device":self.on_device(*args)
                elif name=="keys":self.on_keys(*args)
                elif name=="pair_prompt":self.prompt(*args)
                elif name=="find_done":
                    if args[0]["search_id"]==self.search_id:self.list_candidates()
                elif name=="operation":
                    if args[0]['operation_id']==self.pair_operation:
                        self.discovery.setText("Pairing complete." if args[0]['result']==0 else "Pairing failed. Put the remote in pairing mode and scan again.")
                        self.pair_operation=None
                    self.note(args[0]);self.submit("get_peer",done=self.on_peer)
                    for dialog in list(self.prompts):
                        if dialog.property('operation_id')==args[0]['operation_id']:
                            dialog.setProperty('operation_finished',True);dialog.close()
                elif name=="voice_start" and self.record.isChecked() and self.record_dir:
                    filename=datetime.now().strftime("voice-%Y%m%d-%H%M%S-%f.wav")
                    self.recorder=WaveRecorder(self.record_dir/filename);self.recorder.start(*args)
                elif name=="voice_data" and self.recorder:self.recorder.data(*args)
                elif name=="voice_format" and self.recorder:self.recorder.format(*args)
                elif name=="voice_end" and self.recorder:
                    self.recorder.end(*args);self.last_file=Path(self.recorder.completed[-1]["path"]);self.note(f"{'Saved' if args[0].reason in (0,7,8) else 'Incomplete recording'} {self.last_file} (reason={args[0].reason})")
                    self.recorder=None
                elif name in ("voice_abort","session_lost","error"):
                    if name!="voice_abort":self.release_input()
                    if self.recorder:self.recorder.abort(name)
                    self.recorder=None
                    if name=="session_lost":
                        self.clear_discovery("Session lost")
                        self.device=None;self.catalog=[];self.keys.setRowCount(0)
                        self.status.setText("Session lost — click Close, then Open to reconnect.")
                    self.note(f"{name}: {args}")
            except Exception as exc:
                self.release_input()
                if self.recorder:self.recorder.abort("local_error")
                self.recorder=None
                self.submit("voice_enable",False);self.note(exc)

    def closeEvent(self,event):
        self.close_bridge();event.accept()


def main():
    log_dir=Path(__file__).resolve().parents[1]/"build"/"logs"
    log_dir.mkdir(parents=True,exist_ok=True)
    logging.basicConfig(filename=log_dir/"gui-session.log",level=logging.INFO,
                        format="%(asctime)s %(message)s",encoding="utf-8")
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port",help="Select and open this serial port (or tcp:// address)")
    args=parser.parse_args()
    app=QtWidgets.QApplication([sys.argv[0]]);window=Window();window.show()
    if args.port:
        window.port.setEditText(args.port)
        QtCore.QTimer.singleShot(0,window.open_bridge)
    return app.exec()


if __name__=="__main__":raise SystemExit(main())
