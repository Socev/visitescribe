#!/usr/bin/env python3
import base64
import hashlib
import json
import os
import shutil
import signal
import socket
import subprocess
import time
import uuid
import wave
from datetime import datetime
from pathlib import Path

import RPi.GPIO as GPIO
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from luma.core.interface.serial import spi
from luma.core.render import canvas
from luma.oled.device import sh1106

CONFIG_PATH = Path('/etc/visitescribe/config.json')
DEVICE_KEY_PATH = Path('/etc/visitescribe/device.key')
DATA_DIR = Path('/var/lib/visitescribe/sessions')
UPLOADER_STATUS = Path('/var/lib/visitescribe/uploader-status.json')
RUNTIME_ROOT = Path('/run/visitescribe')
MODES = [
    ('VISITE', 'single_patient'),
    ('PATIENTRONDE', 'multi_patient'),
    ('VERGADERING', 'meeting'),
]
BUTTONS = {
    'KEY1': 21, 'KEY2': 20, 'KEY3': 16,
    'UP': 6, 'DOWN': 19, 'LEFT': 5, 'RIGHT': 26, 'PRESS': 13,
}


def now_iso():
    return datetime.now().astimezone().isoformat(timespec='seconds')


def read_json(path, default=None):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except Exception:
        return default


def atomic_json(path, obj, mode=0o600):
    path = Path(path)
    tmp = path.with_name(path.name + '.tmp')
    data = json.dumps(obj, indent=2, ensure_ascii=False) + '\n'
    with tmp.open('w', encoding='utf-8') as f:
        os.chmod(tmp, mode)
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def fsync_dir(path):
    fd = os.open(str(path), os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


class RecorderApp:
    def __init__(self):
        self.config = read_json(CONFIG_PATH, {}) or {}
        self.audio_cfg = self.config.get('audio', {})
        self.storage_cfg = self.config.get('storage', {})
        self.audio_device = self.audio_cfg.get('device', 'plughw:CARD=J710,DEV=0')
        self.chunk_seconds = int(self.audio_cfg.get('chunk_seconds', 30))
        self.rate = int(self.audio_cfg.get('rate_hz', 48000))
        self.channels = int(self.audio_cfg.get('channels', 1))
        self.min_free_bytes = int(self.storage_cfg.get('minimum_free_bytes', 1024 * 1024 * 1024))

        DATA_DIR.mkdir(parents=True, exist_ok=True)
        RUNTIME_ROOT.mkdir(parents=True, exist_ok=True)

        self.device_key = DEVICE_KEY_PATH.read_bytes()
        if len(self.device_key) != 32:
            raise RuntimeError('device.key moet exact 32 bytes zijn')

        GPIO.setwarnings(False)
        GPIO.setmode(GPIO.BCM)
        for pin in BUTTONS.values():
            GPIO.setup(pin, GPIO.IN, pull_up_down=GPIO.PUD_UP)

        serial = spi(port=0, device=0, gpio_DC=24, gpio_RST=25, bus_speed_hz=8_000_000)
        self.oled = sh1106(serial, width=128, height=64, rotate=2)

        self.mode_index = 0
        self.state = 'idle'
        self.session_dir = None
        self.runtime_dir = None
        self.session_id = None
        self.session_key = None
        self.started_mono = None
        self.stop_offset_ms = None
        self.capture = None
        self.capture_span = 0
        self.span_starts = {}
        self.processed_wavs = set()
        self.chunk_seq = 0
        self.marker_count = 0
        self.last_buttons = {name: False for name in BUTTONS}
        self.key3_since = None
        self.message = ''
        self.message_until = 0.0
        self.battery_cache = '--%'
        self.battery_next = 0.0
        self.mic_cache = False
        self.mic_next = 0.0
        self.queue_cache = 0
        self.queue_next = 0.0
        self.net_cache = 'QUEUE'
        self.running = True

        signal.signal(signal.SIGTERM, self.handle_signal)
        signal.signal(signal.SIGINT, self.handle_signal)
        self.recover_interrupted_sessions()

    def handle_signal(self, *_):
        self.running = False

    def elapsed_ms(self):
        if self.started_mono is None:
            return 0
        return int((time.monotonic() - self.started_mono) * 1000)

    def recover_interrupted_sessions(self):
        for session_dir in sorted(DATA_DIR.iterdir() if DATA_DIR.exists() else []):
            manifest_path = session_dir / 'manifest.json'
            manifest = read_json(manifest_path)
            if not manifest or manifest.get('schema_version') != 2:
                continue
            if manifest.get('status') != 'recording':
                continue
            manifest['status'] = 'interrupted'
            manifest['interrupted_at'] = now_iso()
            manifest['recovery_note'] = 'Service start detected an unfinished session; last RAM-only chunk may be missing.'
            atomic_json(manifest_path, manifest)
            try:
                with (session_dir / 'events.jsonl').open('a', encoding='utf-8') as f:
                    row = {'event': 'recovered_after_unclean_shutdown', 'at': now_iso(), 'offset_ms': manifest.get('duration_ms')}
                    f.write(json.dumps(row, ensure_ascii=False) + '\n')
                    f.flush(); os.fsync(f.fileno())
                self.ensure_upload_state(session_dir)
            except Exception:
                pass

    def ensure_upload_state(self, session_dir):
        path = Path(session_dir) / 'upload.json'
        if path.exists():
            return
        atomic_json(path, {
            'schema_version': 1,
            'state': 'queued',
            'attempts': 0,
            'next_retry_at': 0,
            'last_error': None,
            'created_at': now_iso(),
        })

    def log_event(self, event, offset_ms=None, **extra):
        if not self.session_dir:
            return
        row = {
            'event': event,
            'at': now_iso(),
            'offset_ms': self.elapsed_ms() if offset_ms is None else int(offset_ms),
            **extra,
        }
        path = self.session_dir / 'events.jsonl'
        with path.open('a', encoding='utf-8') as f:
            f.write(json.dumps(row, ensure_ascii=False) + '\n')
            f.flush(); os.fsync(f.fileno())

    def flash(self, text, seconds=2.0):
        self.message = text
        self.message_until = time.monotonic() + seconds

    def mic_ok(self):
        try:
            result = subprocess.run(['arecord', '-l'], capture_output=True, text=True, timeout=3, check=False)
            return 'Jabra Speak 710' in result.stdout or 'J710' in result.stdout
        except Exception:
            return False

    def cached_mic_ok(self):
        now = time.monotonic()
        if now >= self.mic_next:
            self.mic_next = now + 5.0
            self.mic_cache = self.mic_ok()
        return self.mic_cache

    def battery(self):
        now = time.monotonic()
        if now < self.battery_next:
            return self.battery_cache
        self.battery_next = now + 5.0
        try:
            with socket.create_connection(('127.0.0.1', 8423), timeout=0.4) as sock:
                sock.sendall(b'get battery\n')
                data = sock.recv(128).decode('ascii', errors='ignore')
            value = data.split(':', 1)[1].strip()
            self.battery_cache = f'{float(value):.0f}%'
        except Exception:
            self.battery_cache = '--%'
        return self.battery_cache

    def queue_status(self):
        now = time.monotonic()
        if now < self.queue_next:
            return self.queue_cache, self.net_cache
        self.queue_next = now + 3.0
        status = read_json(UPLOADER_STATUS, {}) or {}
        self.queue_cache = int(status.get('queue_count', 0) or 0)
        state = status.get('state', 'disabled')
        self.net_cache = {
            'online': 'ONLINE', 'uploading': 'UPLOAD', 'idle': 'ONLINE',
            'disabled': 'QUEUE', 'waiting_credentials': 'NOAUTH',
            'offline': 'OFFLINE', 'error': 'ERROR',
        }.get(state, 'QUEUE')
        return self.queue_cache, self.net_cache

    def wrap_session_key_local(self, session_key):
        nonce = os.urandom(12)
        aad = f'visitescribe-session-key:{self.session_id}'.encode('utf-8')
        ciphertext = AESGCM(self.device_key).encrypt(nonce, session_key, aad)
        return {
            'algorithm': 'AES-256-GCM',
            'nonce_b64': base64.b64encode(nonce).decode('ascii'),
            'ciphertext_b64': base64.b64encode(ciphertext).decode('ascii'),
            'aad': aad.decode('utf-8'),
        }

    def write_manifest(self, mutator=None):
        path = self.session_dir / 'manifest.json'
        manifest = read_json(path, {}) or {}
        if mutator:
            mutator(manifest)
        atomic_json(path, manifest)

    def start_session(self):
        if not self.mic_ok():
            self.state = 'idle'; self.flash('GEEN JABRA', 4); return
        if shutil.disk_usage(DATA_DIR).free < self.min_free_bytes:
            self.state = 'idle'; self.flash('OPSLAG VOL', 4); return

        label, mode = MODES[self.mode_index]
        self.session_id = str(uuid.uuid4())
        stamp = datetime.now().astimezone().strftime('%Y%m%dT%H%M%S')
        self.session_dir = DATA_DIR / f'{stamp}_{self.session_id}'
        self.session_dir.mkdir(mode=0o700)
        (self.session_dir / 'audio').mkdir(mode=0o700)
        self.runtime_dir = RUNTIME_ROOT / self.session_id
        self.runtime_dir.mkdir(mode=0o700)

        self.session_key = os.urandom(32)
        self.started_mono = time.monotonic()
        self.stop_offset_ms = None
        self.capture_span = 0
        self.span_starts = {}
        self.processed_wavs = set()
        self.chunk_seq = 0
        self.marker_count = 0

        manifest = {
            'schema_version': 2,
            'session_id': self.session_id,
            'device_id': self.config.get('device_id', 'visitescribe-001'),
            'mode': mode,
            'mode_label': label,
            'created_at': now_iso(),
            'status': 'recording',
            'audio': {
                'device': 'Jabra Speak 710',
                'alsa_device': self.audio_device,
                'rate_hz': self.rate,
                'channels': self.channels,
                'sample_format': 'S16_LE',
                'stored_format': 'FLAC encrypted',
                'chunk_seconds': self.chunk_seconds,
            },
            'encryption': {
                'chunk_algorithm': 'AES-256-GCM',
                'key_scope': 'per-session',
                'local_key_wrap': self.wrap_session_key_local(self.session_key),
                'server_key_wrap': None,
            },
            'chunks': [],
            'markers': 0,
        }
        atomic_json(self.session_dir / 'manifest.json', manifest)
        self.log_event('session_started', offset_ms=0, mode=mode)
        try:
            self.start_capture_span()
            self.state = 'recording'
        except Exception as exc:
            self.log_event('recording_error', message=str(exc))
            self.write_manifest(lambda m: m.update({'status': 'error', 'error': str(exc)}))
            self.cleanup_session_runtime()
            self.reset_session()
            self.state = 'idle'; self.flash('OPNAME FOUT', 4)

    def start_capture_span(self):
        self.capture_span += 1
        span = self.capture_span
        self.span_starts[span] = self.elapsed_ms()
        pattern = self.runtime_dir / f'span-{span:04d}-%v.wav'
        errlog = (self.session_dir / 'arecord.log').open('ab')
        cmd = [
            'arecord', '-q', '-D', self.audio_device,
            '-f', 'S16_LE', '-r', str(self.rate), '-c', str(self.channels), '-t', 'wav',
            '--max-file-time', str(self.chunk_seconds), '--use-strftime', str(pattern),
        ]
        self.capture = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=errlog, start_new_session=True)
        errlog.close()
        time.sleep(0.2)
        if self.capture.poll() is not None:
            self.capture = None
            raise RuntimeError('Microfoon kon niet starten')

    def stop_capture(self):
        if self.capture:
            proc = self.capture
            self.capture = None
            try:
                proc.send_signal(signal.SIGTERM)
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait(timeout=2)
        self.harvest_ready_wavs(force=True)

    def wav_info(self, path):
        with wave.open(str(path), 'rb') as w:
            frames = w.getnframes()
            rate = w.getframerate()
            channels = w.getnchannels()
            sampwidth = w.getsampwidth()
        duration_ms = int(frames * 1000 / rate) if rate else 0
        return duration_ms, rate, channels, sampwidth

    def process_wav(self, wav_path):
        wav_path = Path(wav_path)
        if str(wav_path) in self.processed_wavs or not wav_path.exists():
            return
        name = wav_path.stem
        try:
            _, span_s, file_s = name.split('-', 2)
            span = int(span_s)
            file_no = int(file_s)
        except Exception:
            return

        tmp_flac = None
        success = False
        try:
            duration_ms, rate, channels, sampwidth = self.wav_info(wav_path)
            if duration_ms < 100:
                self.processed_wavs.add(str(wav_path))
                success = True
                return

            tmp_flac = self.runtime_dir / f'{name}.flac'
            subprocess.run(['flac', '-f', '--silent', '-o', str(tmp_flac), str(wav_path)], check=True, timeout=20)
            plaintext = tmp_flac.read_bytes()

            self.chunk_seq += 1
            seq = self.chunk_seq
            nonce = os.urandom(12)
            aad = f'visitescribe-v2:{self.session_id}:{seq}'.encode('utf-8')
            ciphertext = AESGCM(self.session_key).encrypt(nonce, plaintext, aad)

            out = self.session_dir / 'audio' / f'chunk-{seq:06d}.flac.enc'
            tmp_out = out.with_suffix(out.suffix + '.part')
            with tmp_out.open('wb') as f:
                f.write(ciphertext); f.flush(); os.fsync(f.fileno())
            os.replace(tmp_out, out)
            fsync_dir(out.parent)

            start_offset_ms = int(self.span_starts.get(span, 0) + (file_no - 1) * self.chunk_seconds * 1000)
            chunk_meta = {
                'sequence': seq,
                'file': f'audio/{out.name}',
                'span': span,
                'span_file_number': file_no,
                'start_offset_ms': start_offset_ms,
                'duration_ms': duration_ms,
                'nonce_b64': base64.b64encode(nonce).decode('ascii'),
                'aad': aad.decode('utf-8'),
                'plaintext_sha256': hashlib.sha256(plaintext).hexdigest(),
                'ciphertext_sha256': hashlib.sha256(ciphertext).hexdigest(),
                'plaintext_bytes': len(plaintext),
                'ciphertext_bytes': len(ciphertext),
                'rate_hz': rate,
                'channels': channels,
                'sample_width_bytes': sampwidth,
            }

            def add_chunk(m):
                chunks = m.setdefault('chunks', [])
                chunks.append(chunk_meta)
                m['chunk_count'] = len(chunks)
                m['encrypted_audio_bytes'] = sum(int(x.get('ciphertext_bytes', 0)) for x in chunks)
                m['duration_ms'] = max(int(m.get('duration_ms', 0) or 0), start_offset_ms + duration_ms)
            self.write_manifest(add_chunk)
            self.processed_wavs.add(str(wav_path))
            success = True
        except Exception as exc:
            self.log_event('chunk_processing_error', source=wav_path.name, message=str(exc))
            self.flash('CHUNK FOUT', 3)
        finally:
            if success:
                try: wav_path.unlink(missing_ok=True)
                except Exception: pass
            if tmp_flac is not None:
                try: tmp_flac.unlink(missing_ok=True)
                except Exception: pass

    @staticmethod
    def wav_sort_key(path):
        try:
            _, span_s, file_s = Path(path).stem.split('-', 2)
            return int(span_s), int(file_s)
        except Exception:
            return (10**9, 10**9)

    def harvest_ready_wavs(self, force=False):
        if not self.runtime_dir or not self.runtime_dir.exists() or not self.session_key:
            return
        wavs = sorted(self.runtime_dir.glob('span-*.wav'), key=self.wav_sort_key)
        # While arecord is alive, the newest WAV is the file currently being
        # written. Never touch it. All older WAVs have been closed by arecord.
        capture_alive = self.capture is not None and self.capture.poll() is None
        candidates = wavs if force or not capture_alive else wavs[:-1]
        for wav_path in candidates:
            if str(wav_path) not in self.processed_wavs:
                self.process_wav(wav_path)

    def toggle_pause(self):
        offset = self.elapsed_ms()
        if self.state == 'recording':
            self.stop_capture()
            self.log_event('privacy_pause_started', offset_ms=offset)
            self.state = 'paused'
        elif self.state == 'paused':
            try:
                self.log_event('privacy_pause_ended', offset_ms=offset)
                self.start_capture_span()
                self.state = 'recording'
            except Exception as exc:
                self.log_event('recording_error', offset_ms=offset, message=str(exc))
                self.flash('HERVAT FOUT', 4)

    def add_marker(self):
        if self.state not in ('recording', 'paused'):
            return
        self.marker_count += 1
        mode = MODES[self.mode_index][1]
        kind = 'patient_boundary' if mode == 'multi_patient' else 'marker'
        self.log_event(kind, number=self.marker_count)
        self.write_manifest(lambda m: m.update({'markers': self.marker_count}))
        self.flash('VOLGENDE PATIENT' if kind == 'patient_boundary' else 'MARKERING')

    def stop_session(self):
        if not self.session_dir:
            return
        stop_offset = self.elapsed_ms()
        self.stop_offset_ms = stop_offset
        self.stop_capture()
        self.log_event('session_stopped', offset_ms=stop_offset, markers=self.marker_count)

        def finalize(m):
            m.update({
                'status': 'complete',
                'completed_at': now_iso(),
                'duration_ms': stop_offset,
                'markers': self.marker_count,
                'chunk_count': len(m.get('chunks', [])),
            })
        self.write_manifest(finalize)
        self.ensure_upload_state(self.session_dir)
        self.cleanup_session_runtime()
        self.state = 'idle'
        self.flash('OPGESLAGEN / QUEUE', 3)
        self.reset_session()

    def cleanup_session_runtime(self):
        try:
            if self.runtime_dir and self.runtime_dir.exists():
                shutil.rmtree(self.runtime_dir)
        except Exception:
            pass

    def reset_session(self):
        self.session_dir = None
        self.runtime_dir = None
        self.session_id = None
        self.session_key = None
        self.started_mono = None
        self.capture = None
        self.stop_offset_ms = None

    def pressed(self, name):
        return GPIO.input(BUTTONS[name]) == GPIO.LOW

    def handle_buttons(self):
        current = {name: self.pressed(name) for name in BUTTONS}
        edge = {name: current[name] and not self.last_buttons[name] for name in BUTTONS}

        if self.state == 'idle':
            if edge['UP']:
                self.mode_index = (self.mode_index - 1) % len(MODES)
            if edge['DOWN']:
                self.mode_index = (self.mode_index + 1) % len(MODES)
            if edge['PRESS']:
                self.state = 'confirm'
        elif self.state == 'confirm':
            if edge['PRESS']:
                self.start_session()
            if edge['KEY3'] or edge['LEFT']:
                self.state = 'idle'
        elif self.state in ('recording', 'paused'):
            if edge['KEY1']:
                self.toggle_pause()
            if edge['KEY2']:
                self.add_marker()
            if current['KEY3']:
                if self.key3_since is None:
                    self.key3_since = time.monotonic()
                elif time.monotonic() - self.key3_since >= 1.5:
                    self.key3_since = None
                    self.stop_session()
            else:
                self.key3_since = None
        self.last_buttons = current

    def draw(self):
        battery = self.battery()
        queue_count, net_state = self.queue_status()
        label, mode = MODES[self.mode_index]
        with canvas(self.oled) as draw:
            if self.message_until > time.monotonic():
                draw.text((4, 5), 'VISITESCRIBE v0.2', fill='white')
                draw.text((4, 28), self.message, fill='white')
                draw.text((4, 48), f'Q:{queue_count} {battery}', fill='white')
                return
            if self.state == 'idle':
                draw.text((4, 1), 'VISITESCRIBE', fill='white')
                draw.text((91, 1), battery, fill='white')
                draw.text((4, 16), f'> {label}', fill='white')
                draw.text((4, 31), f'{net_state} Q:{queue_count}', fill='white')
                draw.text((4, 46), 'PRESS start', fill='white')
                draw.text((92, 46), 'MIC+' if self.cached_mic_ok() else 'MIC-', fill='white')
            elif self.state == 'confirm':
                draw.text((4, 1), label, fill='white')
                draw.text((4, 18), 'TOESTEMMING OK?', fill='white')
                draw.text((4, 34), 'PRESS = start', fill='white')
                draw.text((4, 49), 'KEY3 = terug', fill='white')
            elif self.state == 'recording':
                self.harvest_ready_wavs(force=False)
                sec = self.elapsed_ms() // 1000
                elapsed = f'{sec // 3600:02d}:{(sec % 3600) // 60:02d}:{sec % 60:02d}'
                draw.text((4, 1), f'REC {label}', fill='white')
                draw.text((4, 16), elapsed, fill='white')
                status = f'PATIENT {self.marker_count + 1}' if mode == 'multi_patient' else f'MARKERS {self.marker_count}'
                draw.text((4, 31), status, fill='white')
                draw.text((4, 46), f'K1 pauze K2 mark', fill='white')
                draw.text((98, 16), battery, fill='white')
            elif self.state == 'paused':
                draw.text((4, 1), 'PRIVACY PAUZE', fill='white')
                draw.text((4, 20), 'GEEN AUDIO', fill='white')
                draw.text((4, 36), 'K1 = hervat', fill='white')
                draw.text((4, 50), 'K3 lang = stop', fill='white')

    def run(self):
        try:
            while self.running:
                self.handle_buttons()
                if self.capture and self.capture.poll() is not None:
                    self.capture = None
                    self.harvest_ready_wavs(force=True)
                    self.log_event('recording_process_ended')
                    self.state = 'paused'
                    self.flash('MIC FOUT', 4)
                self.draw()
                time.sleep(0.08)
        finally:
            if self.session_dir:
                stop_offset = self.elapsed_ms()
                try: self.stop_capture()
                except Exception: pass
                self.log_event('service_stopped', offset_ms=stop_offset)
                try:
                    self.write_manifest(lambda m: m.update({
                        'status': 'interrupted',
                        'interrupted_at': now_iso(),
                        'duration_ms': stop_offset,
                    }))
                    self.ensure_upload_state(self.session_dir)
                except Exception:
                    pass
                self.cleanup_session_runtime()
            try:
                self.oled.clear()
            finally:
                GPIO.cleanup(list(BUTTONS.values()))


if __name__ == '__main__':
    RecorderApp().run()
