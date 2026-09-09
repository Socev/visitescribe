#!/usr/bin/env python3
import base64
import hashlib
import os
import signal
import subprocess
import time
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from recorder import RecorderApp, MODES, read_json, fsync_dir


class RecorderAppV021(RecorderApp):
    """VisiteScribe v0.2.1 recorder.

    Adds a physical audio-file split for multi-patient boundaries using
    arecord's SIGUSR1 recycle mechanism. The patient boundary event is then
    aligned to the actual end of the closed WAV, not merely to wall-clock
    button time.
    """

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
            subprocess.run(
                ['flac', '-f', '--silent', '-o', str(tmp_flac), str(wav_path)],
                check=True,
                timeout=20,
            )
            plaintext = tmp_flac.read_bytes()

            self.chunk_seq += 1
            seq = self.chunk_seq
            nonce = os.urandom(12)
            aad = f'visitescribe-v2:{self.session_id}:{seq}'.encode('utf-8')
            ciphertext = AESGCM(self.session_key).encrypt(nonce, plaintext, aad)

            out = self.session_dir / 'audio' / f'chunk-{seq:06d}.flac.enc'
            tmp_out = out.with_suffix(out.suffix + '.part')
            with tmp_out.open('wb') as f:
                f.write(ciphertext)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp_out, out)
            fsync_dir(out.parent)

            # v0.2 assumed every WAV before this one was exactly chunk_seconds
            # long. That becomes false when SIGUSR1 creates an immediate file
            # split. Derive the start from the actual durations of all earlier
            # files in the same capture span instead.
            manifest = read_json(self.session_dir / 'manifest.json', {}) or {}
            prior_duration_ms = sum(
                int(c.get('duration_ms', 0) or 0)
                for c in manifest.get('chunks', [])
                if int(c.get('span', -1)) == span
                and int(c.get('span_file_number', -1)) < file_no
            )
            start_offset_ms = int(self.span_starts.get(span, 0) + prior_duration_ms)

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
                m['encrypted_audio_bytes'] = sum(
                    int(x.get('ciphertext_bytes', 0)) for x in chunks
                )
                m['duration_ms'] = max(
                    int(m.get('duration_ms', 0) or 0),
                    start_offset_ms + duration_ms,
                )

            self.write_manifest(add_chunk)
            self.processed_wavs.add(str(wav_path))
            success = True
        except Exception as exc:
            self.log_event(
                'chunk_processing_error', source=wav_path.name, message=str(exc)
            )
            self.flash('CHUNK FOUT', 3)
        finally:
            if success:
                try:
                    wav_path.unlink(missing_ok=True)
                except Exception:
                    pass
            if tmp_flac is not None:
                try:
                    tmp_flac.unlink(missing_ok=True)
                except Exception:
                    pass

    def span_audio_end_ms(self, span):
        manifest = read_json(self.session_dir / 'manifest.json', {}) or {}
        ends = [
            int(c.get('start_offset_ms', 0) or 0)
            + int(c.get('duration_ms', 0) or 0)
            for c in manifest.get('chunks', [])
            if int(c.get('span', -1)) == int(span)
        ]
        return max(ends) if ends else None

    def force_file_boundary(self):
        """Close the current WAV and continue capture in a new WAV.

        arecord handles SIGUSR1 by closing the current output file, opening a
        new one and continuing capture. We wait only until the new file is
        visible, harvest the now-closed file, and return the actual audio end
        position of that file.
        """
        if not self.capture or self.capture.poll() is not None:
            raise RuntimeError('capture process is not running')

        span = self.capture_span
        before = {
            p.name for p in self.runtime_dir.glob(f'span-{span:04d}-*.wav')
        }
        fallback_offset = self.elapsed_ms()
        self.capture.send_signal(signal.SIGUSR1)

        deadline = time.monotonic() + 0.75
        split_seen = False
        while time.monotonic() < deadline:
            current = {
                p.name for p in self.runtime_dir.glob(f'span-{span:04d}-*.wav')
            }
            if current - before:
                split_seen = True
                break
            if self.capture.poll() is not None:
                raise RuntimeError('capture process stopped during boundary split')
            time.sleep(0.01)

        if not split_seen:
            raise RuntimeError('arecord did not create a new file after SIGUSR1')

        # arecord is already recording into the newly opened file. Processing
        # the closed file may take a little longer, but it cannot move audio
        # across the boundary.
        self.harvest_ready_wavs(force=False)
        actual_end = self.span_audio_end_ms(span)
        return actual_end if actual_end is not None else fallback_offset

    def add_marker(self):
        if self.state not in ('recording', 'paused'):
            return

        self.marker_count += 1
        mode = MODES[self.mode_index][1]
        kind = 'patient_boundary' if mode == 'multi_patient' else 'marker'

        if kind == 'patient_boundary' and self.state == 'recording':
            try:
                boundary_offset = self.force_file_boundary()
                self.log_event(
                    kind,
                    offset_ms=boundary_offset,
                    number=self.marker_count,
                    boundary_source='forced_audio_split',
                )
                self.flash('VOLGENDE PATIENT')
            except Exception as exc:
                # Never hide a failed physical split. Keep a logical fallback
                # marker for recoverability, but make the failure explicit in
                # the event journal and on the display.
                fallback_offset = self.elapsed_ms()
                self.log_event(
                    'patient_boundary_split_error',
                    offset_ms=fallback_offset,
                    number=self.marker_count,
                    message=str(exc),
                )
                self.log_event(
                    kind,
                    offset_ms=fallback_offset,
                    number=self.marker_count,
                    boundary_source='clock_fallback_after_split_error',
                )
                self.flash('KNIP FOUT', 3)
        else:
            # Normal markers and boundaries entered while privacy-paused remain
            # logical timeline events and do not need a physical audio split.
            self.log_event(kind, number=self.marker_count)
            self.flash(
                'VOLGENDE PATIENT' if kind == 'patient_boundary' else 'MARKERING'
            )

        self.write_manifest(lambda m: m.update({'markers': self.marker_count}))


if __name__ == '__main__':
    RecorderAppV021().run()
