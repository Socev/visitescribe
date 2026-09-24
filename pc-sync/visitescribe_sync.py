from __future__ import annotations

import base64
import csv
import hashlib
import hmac
import io
import json
import queue
import shutil
import struct
import subprocess
import tempfile
import threading
import time
import wave
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

import numpy as np
import requests
import serial
from serial.tools import list_ports
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

import tkinter as tk
from tkinter import ttk
from tkinter.scrolledtext import ScrolledText


USB_BAUD = 115200
USB_PROTOCOL = 1
USB_READ_BYTES = 256 * 1024
SPEECH_RATE = 16000
SYNC_CHUNK_SECONDS = 30
OPUS_BITRATE = 24000
OPUS_FRAME_MS = 20
SPOOL_ROOT = Path(__file__).resolve().parent / "spool"
CONFIRMED_STATES = {
    "INGESTED", "READY_FOR_PROCESSING", "TRANSCRIBING",
    "PROCESSING", "REVIEW_REQUIRED", "APPROVED",
}


@dataclass
class DeviceInfo:
    device_id: str
    base_url: str
    token: str


@dataclass
class RemoteFile:
    path: str
    size: int


@dataclass
class UsbSession:
    prefix: str
    uuid: str
    mode: str
    events: RemoteFile | None = None
    wavs: list[RemoteFile] = field(default_factory=list)


@dataclass
class Chunk:
    sequence: int
    wav_path: Path
    frame_start: int
    frame_count: int
    source_rate: int
    source_channels: int
    ratio: int
    output_samples: int
    start_offset_ms: int
    duration_ms: int
    plaintext_size: int
    ciphertext_size: int


@dataclass
class OpusPreparedChunk:
    sequence: int
    path: Path
    nonce_b64: str
    aad: str
    plaintext_sha256: str
    ciphertext_sha256: str
    plaintext_size: int
    ciphertext_size: int
    start_offset_ms: int
    duration_ms: int


class ExistingSessionNeedsV3(RuntimeError):
    pass


class VisiteScribeUsb:
    def __init__(self, ser: serial.Serial):
        self.ser = ser

    @staticmethod
    def _open_port(port: str) -> serial.Serial:
        # Configure DTR/RTS before opening the ESP32-S3 USB CDC port.  Opening
        # first and clearing the lines afterwards can create a short control-
        # line pulse; on native USB devices that may cause a disconnect /
        # re-enumeration exactly while discovery is sending HELLO.
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = USB_BAUD
        ser.timeout = 0.35
        ser.write_timeout = 10
        ser.inter_byte_timeout = 1
        ser.dtr = False
        ser.rts = False
        ser.open()
        return ser

    @classmethod
    def discover(cls, log: Callable[[str], None]) -> "VisiteScribeUsb | None":
        ports = list(list_ports.comports())
        if not ports:
            log("Geen COM-poorten gevonden.")
            return None

        for p in ports:
            ser = None
            try:
                ser = cls._open_port(p.device)

                # Native USB CDC can need a little time after the host opens the
                # handle.  Do not hammer the port immediately.
                time.sleep(0.75)
                try:
                    ser.reset_input_buffer()
                except serial.SerialException:
                    # Device may have re-enumerated once.  Let the next scan
                    # reopen the fresh COM handle instead of treating this as a
                    # permanent failure.
                    raise

                dev = cls(ser)
                for attempt in range(5):
                    try:
                        dev._write_line("VSUSB HELLO")
                    except serial.SerialException:
                        raise
                    line = dev._wait_protocol_line("VSUSB READY", timeout=1.2)
                    if line:
                        parts = line.split()
                        if len(parts) >= 3 and int(parts[2]) == USB_PROTOCOL:
                            log(
                                f"VisiteScribe gevonden op {p.device} "
                                f"({p.description or 'USB serial'})"
                            )
                            return dev
                    time.sleep(0.35)

                log(
                    f"{p.device}: poort opent wel maar geen VSUSB READY. "
                    f"Beschrijving={p.description!r}, VID={p.vid}, PID={p.pid}"
                )
            except (serial.SerialException, OSError) as exc:
                log(
                    f"{p.device}: USB-poort tijdelijk niet beschikbaar ({exc}). "
                    "Als de Serial Monitor dicht is, wacht de app op re-enumeratie en probeert opnieuw."
                )
            except Exception as exc:
                log(f"{p.device}: detectiefout: {type(exc).__name__}: {exc}")

            if ser:
                try:
                    ser.close()
                except Exception:
                    pass
        return None

    @property
    def port(self) -> str:
        return self.ser.port

    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:
            pass

    def _write_line(self, line: str) -> None:
        self.ser.write((line + "\n").encode("ascii"))
        self.ser.flush()

    def _readline(self, timeout: float = 5.0) -> str:
        deadline = time.monotonic() + timeout
        buf = bytearray()
        while time.monotonic() < deadline:
            b = self.ser.read(1)
            if not b:
                continue
            if b == b"\n":
                return buf.decode("utf-8", errors="replace").rstrip("\r")
            buf += b
        raise TimeoutError("timeout op USB-regel")

    def _wait_protocol_line(self, prefix: str, timeout: float = 5.0) -> str | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self._readline(max(0.1, deadline - time.monotonic()))
            except TimeoutError:
                return None
            if line.startswith(prefix):
                return line
        return None

    def _command(self, command: str, expected: str, timeout: float = 5.0) -> str:
        self._write_line(command)
        line = self._wait_protocol_line(expected, timeout)
        if not line:
            raise RuntimeError(f"geen antwoord op {command}")
        return line

    def enter(self) -> DeviceInfo | None:
        self._write_line("VSUSB ENTER")
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            line = self._readline(1)
            if line.startswith("VSUSB OK ENTER"):
                parts = line.split(" ", 5)
                if len(parts) == 6:
                    token = base64.b64decode(parts[5]).decode("utf-8") if parts[5] else ""
                    return DeviceInfo(parts[3], parts[4], token)
                # Backward compatibility with the first USB firmware build:
                # ENTER succeeded but identity still has to be queried.
                return self.info()
            if line == "VSUSB BUSY RECORDING":
                return None
            if line.startswith("VSUSB ERROR"):
                raise RuntimeError(line)
        raise RuntimeError("M5 antwoordt niet op ENTER")

    def exit(self) -> None:
        try:
            self._command("VSUSB EXIT", "VSUSB OK EXIT", 2)
        except Exception:
            pass

    def info(self) -> DeviceInfo:
        line = self._command("VSUSB INFO", "VSUSB INFO", 3)
        parts = line.split(" ", 4)
        if len(parts) != 5:
            raise RuntimeError(f"ongeldig INFO-antwoord: {line}")
        token = base64.b64decode(parts[4]).decode("utf-8") if parts[4] else ""
        return DeviceInfo(parts[2], parts[3], token)

    def list_sessions(self) -> list[UsbSession]:
        self._write_line("VSUSB LIST")
        sessions: list[UsbSession] = []
        current: UsbSession | None = None
        while True:
            line = self._readline(10)
            if not line.startswith("VSUSB "):
                continue
            if line.startswith("VSUSB SKIP "):
                continue
            if line.startswith("VSUSB SESSION "):
                p = line.split()
                if len(p) < 7:
                    raise RuntimeError(f"ongeldige SESSION: {line}")
                current = UsbSession(prefix=p[2], uuid=p[3], mode=p[4])
                continue
            if line.startswith("VSUSB EVENTS "):
                if current is None:
                    raise RuntimeError("EVENTS buiten SESSION")
                p = line.split(" ", 3)
                current.events = RemoteFile(path=p[3], size=int(p[2]))
                continue
            if line.startswith("VSUSB WAV "):
                if current is None:
                    raise RuntimeError("WAV buiten SESSION")
                p = line.split(" ", 3)
                current.wavs.append(RemoteFile(path=p[3], size=int(p[2])))
                continue
            if line == "VSUSB ENDSESSION":
                if current:
                    sessions.append(current)
                    current = None
                continue
            if line.startswith("VSUSB ENDLIST "):
                return sessions
            if line.startswith("VSUSB ERROR"):
                raise RuntimeError(line)

    def session_key(self, uuid: str) -> bytes:
        line = self._command(f"VSUSB KEY {uuid}", "VSUSB KEY", 3)
        p = line.split()
        if len(p) != 4 or p[2] != uuid:
            raise RuntimeError(f"ongeldig KEY-antwoord: {line}")
        key = base64.b64decode(p[3])
        if len(key) != 32:
            raise RuntimeError("sessiesleutel is niet 32 bytes")
        return key

    def read_file(
        self,
        remote: RemoteFile,
        local: Path,
        progress: Callable[[int, int], None] | None = None,
    ) -> None:
        local.parent.mkdir(parents=True, exist_ok=True)
        done = 0
        with local.open("wb") as out:
            while done < remote.size:
                request = min(USB_READ_BYTES, remote.size - done)
                self._write_line(f"VSUSB READ {remote.path} {done} {request}")
                header = self._wait_protocol_line("VSUSB DATA ", 5)
                if not header:
                    raise RuntimeError(f"geen DATA voor {remote.path}")
                n = int(header.split()[2])
                data = self._read_exact(n, 30)
                out.write(data)
                done += len(data)

                trailer = self._wait_protocol_line("VSUSB ENDDATA ", 5)
                if not trailer:
                    raise RuntimeError("USB data-trailer ontbreekt")
                sent = int(trailer.split()[2])
                if sent != n or len(data) != n:
                    raise RuntimeError("USB bloklengte klopt niet")
                if n == 0 and done < remote.size:
                    raise RuntimeError("onverwacht leeg USB-blok")
                if progress:
                    progress(done, remote.size)

        if done != remote.size:
            raise RuntimeError(f"bestand incompleet: {done}/{remote.size}")

    def _read_exact(self, n: int, timeout: float) -> bytes:
        deadline = time.monotonic() + timeout
        out = bytearray()
        while len(out) < n and time.monotonic() < deadline:
            chunk = self.ser.read(n - len(out))
            if chunk:
                out += chunk
        if len(out) != n:
            raise TimeoutError(f"USB binary timeout {len(out)}/{n}")
        return bytes(out)

    def mark_ingested(self, prefix: str, uuid: str) -> None:
        line = self._command(
            f"VSUSB MARK {prefix} {uuid}", "VSUSB OK MARK", 3
        )
        if not line.endswith(prefix):
            raise RuntimeError(f"ongeldig MARK-antwoord: {line}")


def api_headers(info: DeviceInfo, extra: dict[str, str] | None = None) -> dict[str, str]:
    h = {"X-Device-ID": info.device_id}
    if info.token:
        h["Authorization"] = f"Bearer {info.token}"
    if extra:
        h.update(extra)
    return h


def confirmed(status: dict) -> bool:
    if status.get("ingest_confirmed") is True:
        return True
    return str(status.get("state", "")).upper() in CONFIRMED_STATES


def fetch_server_key(http: requests.Session, info: DeviceInfo) -> tuple[str, bytes]:
    r = http.get(
        info.base_url.rstrip("/") + "/v1/server/public-key",
        headers=api_headers(info),
        timeout=30,
    )
    r.raise_for_status()
    data = r.json()
    return data["key_id"], data["public_key_pem"].encode("utf-8")


def wrap_session_key(session_key: bytes, public_pem: bytes) -> str:
    public_key = serialization.load_pem_public_key(public_pem)
    wrapped = public_key.encrypt(
        session_key,
        padding.OAEP(
            mgf=padding.MGF1(algorithm=hashes.SHA256()),
            algorithm=hashes.SHA256(),
            label=None,
        ),
    )
    return base64.b64encode(wrapped).decode("ascii")


def describe_chunks(wav_paths: list[Path]) -> list[Chunk]:
    chunks: list[Chunk] = []
    sequence = 0
    offset_ms = 0

    for path in wav_paths:
        with wave.open(str(path), "rb") as w:
            channels = w.getnchannels()
            rate = w.getframerate()
            width = w.getsampwidth()
            frames_total = w.getnframes()

        if width != 2 or channels not in (1, 2):
            raise RuntimeError(f"{path.name}: alleen 16-bit mono/stereo ondersteund")
        if rate < SPEECH_RATE or rate % SPEECH_RATE:
            raise RuntimeError(f"{path.name}: sample rate {rate} niet deelbaar door 16 kHz")
        ratio = rate // SPEECH_RATE
        if not 1 <= ratio <= 8:
            raise RuntimeError(f"{path.name}: downsample ratio {ratio} niet ondersteund")

        frame_start = 0
        max_frames = rate * SYNC_CHUNK_SECONDS
        while frame_start < frames_total:
            frame_count = min(max_frames, frames_total - frame_start)
            frame_count -= frame_count % ratio
            if frame_count <= 0:
                break
            output_samples = frame_count // ratio
            duration_ms = (output_samples * 1000) // SPEECH_RATE
            plaintext_size = 44 + output_samples * 2
            sequence += 1
            chunks.append(
                Chunk(
                    sequence=sequence,
                    wav_path=path,
                    frame_start=frame_start,
                    frame_count=frame_count,
                    source_rate=rate,
                    source_channels=channels,
                    ratio=ratio,
                    output_samples=output_samples,
                    start_offset_ms=offset_ms,
                    duration_ms=duration_ms,
                    plaintext_size=plaintext_size,
                    ciphertext_size=plaintext_size + 16,
                )
            )
            frame_start += frame_count
            offset_ms += duration_ms

    return chunks


def canonical_wav_16k_mono(chunk: Chunk) -> bytes:
    with wave.open(str(chunk.wav_path), "rb") as w:
        w.setpos(chunk.frame_start)
        raw = w.readframes(chunk.frame_count)

    samples = np.frombuffer(raw, dtype="<i2")
    expected = chunk.frame_count * chunk.source_channels
    if samples.size != expected:
        raise RuntimeError(
            f"{chunk.wav_path.name}: verwacht {expected} samples, kreeg {samples.size}"
        )

    grouped = samples.reshape(
        chunk.output_samples, chunk.ratio, chunk.source_channels
    ).astype(np.int32)
    sums = grouped.sum(axis=(1, 2), dtype=np.int32)
    divisor = chunk.ratio * chunk.source_channels

    # C/C++ integer division truncates towards zero; reproduce the firmware
    # exactly instead of NumPy's floor division for negative values.
    mono32 = np.where(
        sums >= 0,
        sums // divisor,
        -((-sums) // divisor),
    )
    pcm = mono32.astype("<i2").tobytes()

    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        36 + len(pcm),
        b"WAVE",
        b"fmt ",
        16,
        1,
        1,
        SPEECH_RATE,
        SPEECH_RATE * 2,
        2,
        16,
        b"data",
        len(pcm),
    )
    return header + pcm


def chunk_crypto(session_key: bytes, uuid: str, chunk: Chunk) -> tuple[bytes, dict[str, str]]:
    plaintext = canonical_wav_16k_mono(chunk)
    if len(plaintext) != chunk.plaintext_size:
        raise RuntimeError("plaintextgrootte wijkt af van manifest")

    nonce_material = f"nonce:v3-speech:{uuid}:{chunk.sequence}".encode("utf-8")
    nonce = hmac.new(session_key, nonce_material, hashlib.sha256).digest()[:12]
    aad_text = f"visitescribe-v3-speech:{uuid}:{chunk.sequence}"
    ciphertext = AESGCM(session_key).encrypt(
        nonce, plaintext, aad_text.encode("utf-8")
    )

    meta = {
        "nonce_b64": base64.b64encode(nonce).decode("ascii"),
        "aad": aad_text,
        "plaintext_sha256": hashlib.sha256(plaintext).hexdigest(),
        "ciphertext_sha256": hashlib.sha256(ciphertext).hexdigest(),
    }
    return ciphertext, meta


def build_manifest(
    usb_session: UsbSession,
    info: DeviceInfo,
    chunks: list[Chunk],
    server_key_id: str,
    wrapped_key_b64: str,
) -> dict:
    return {
        "schema_version": 2,
        "session_id": usb_session.uuid,
        "device_id": info.device_id,
        "mode": usb_session.mode,
        "status": "complete",
        "audio": {
            "codec": "wav",
            "sample_rate": SPEECH_RATE,
            "channels": 1,
            "sample_format": "S16_LE",
            "chunk_seconds": SYNC_CHUNK_SECONDS,
        },
        "encryption": {
            "algorithm": "AES-256-GCM",
            "local_key_wrap": None,
            "server_key_wrap": {
                "algorithm": "RSA-OAEP-SHA256",
                "key_id": server_key_id,
                "ciphertext_b64": wrapped_key_b64,
            },
        },
        "chunks": [
            {
                "sequence": c.sequence,
                "file": f"audio/chunk-{c.sequence:06d}.wav.enc",
                "nonce_b64": base64.b64encode(
                    hmac.new(
                        b"manifest-only-placeholder",
                        str(c.sequence).encode(),
                        hashlib.sha256,
                    ).digest()[:12]
                ).decode("ascii"),
                "aad": "",
                "plaintext_size": c.plaintext_size,
                "ciphertext_size": c.ciphertext_size,
                "start_offset_ms": c.start_offset_ms,
                "duration_ms": c.duration_ms,
            }
            for c in chunks
        ],
    }


def fill_manifest_crypto(manifest: dict, session_key: bytes, uuid: str) -> None:
    for item in manifest["chunks"]:
        sequence = int(item["sequence"])
        nonce = hmac.new(
            session_key,
            f"nonce:v3-speech:{uuid}:{sequence}".encode("utf-8"),
            hashlib.sha256,
        ).digest()[:12]
        item["nonce_b64"] = base64.b64encode(nonce).decode("ascii")
        item["aad"] = f"visitescribe-v3-speech:{uuid}:{sequence}"


_FFMPEG_EXE: str | None = None


def ffmpeg_exe() -> str:
    global _FFMPEG_EXE
    if _FFMPEG_EXE:
        return _FFMPEG_EXE

    exe = shutil.which("ffmpeg")
    if not exe:
        try:
            import imageio_ffmpeg  # type: ignore
            exe = imageio_ffmpeg.get_ffmpeg_exe()
        except Exception as exc:
            raise RuntimeError(
                "ffmpeg ontbreekt. Installeer requirements.txt opnieuw."
            ) from exc

    probe = subprocess.run(
        [exe, "-hide_banner", "-h", "encoder=libopus"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    if probe.returncode != 0 or "libopus" not in probe.stdout.lower():
        raise RuntimeError("de gevonden ffmpeg bevat geen libopus encoder")

    _FFMPEG_EXE = exe
    return exe


def api_error_code(response: requests.Response) -> str | None:
    try:
        body = response.json()
    except ValueError:
        return None
    error = body.get("error")
    if isinstance(error, dict):
        code = error.get("code")
        return str(code) if code else None
    return None


def v4_nonce_and_aad(session_key: bytes, uuid: str, sequence: int) -> tuple[bytes, str]:
    nonce = hmac.new(
        session_key,
        f"nonce:v4-opus:{uuid}:{sequence}".encode("ascii"),
        hashlib.sha256,
    ).digest()[:12]
    aad = f"visitescribe-v4-opus:{uuid}:{sequence}"
    return nonce, aad


def encode_opus_plaintext(chunk: Chunk) -> bytes:
    pcm_wav = canonical_wav_16k_mono(chunk)
    exe = ffmpeg_exe()

    with tempfile.TemporaryDirectory(prefix="visitescribe-opus-") as td:
        root = Path(td)
        in_wav = root / "chunk.wav"
        out_opus = root / "chunk.opus"
        in_wav.write_bytes(pcm_wav)

        cmd = [
            exe,
            "-hide_banner",
            "-loglevel", "error",
            "-y",
            "-i", str(in_wav),
            "-ac", "1",
            "-ar", str(SPEECH_RATE),
            "-c:a", "libopus",
            "-b:a", "24k",
            "-vbr", "on",
            "-compression_level", "10",
            "-application", "voip",
            "-frame_duration", str(OPUS_FRAME_MS),
            "-map_metadata", "-1",
            "-fflags", "+bitexact",
            "-flags:a", "+bitexact",
            "-f", "ogg",
            str(out_opus),
        ]
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        if proc.returncode != 0 or not out_opus.exists():
            detail = (proc.stderr or proc.stdout or "onbekende ffmpeg fout").strip()
            raise RuntimeError(f"ffmpeg Opus encode mislukt: {detail}")

        data = out_opus.read_bytes()
        if not data.startswith(b"OggS") or b"OpusHead" not in data[:512]:
            raise RuntimeError("ffmpeg leverde geen geldig Ogg/Opus-bestand")
        return data


def _manifest_chunk_to_prepared(root: Path, item: dict) -> OpusPreparedChunk:
    sequence = int(item["sequence"])
    path = root / f"chunk-{sequence:06d}.opus.enc"
    return OpusPreparedChunk(
        sequence=sequence,
        path=path,
        nonce_b64=str(item["nonce_b64"]),
        aad=str(item["aad"]),
        plaintext_sha256=str(item["plaintext_sha256"]),
        ciphertext_sha256=str(item["ciphertext_sha256"]),
        plaintext_size=int(item["plaintext_size"]),
        ciphertext_size=int(item["ciphertext_size"]),
        start_offset_ms=int(item.get("start_offset_ms", 0)),
        duration_ms=int(item.get("duration_ms", 0)),
    )


def load_v4_spool(uuid: str) -> tuple[dict, list[OpusPreparedChunk]] | None:
    root = SPOOL_ROOT / uuid
    manifest_path = root / "manifest.json"
    if not manifest_path.exists():
        return None

    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest.get("session_id") != uuid:
            return None
        audio = manifest.get("audio") or {}
        if audio.get("codec") != "opus" or audio.get("container") != "ogg":
            return None

        prepared = [
            _manifest_chunk_to_prepared(root, item)
            for item in manifest.get("chunks", [])
        ]
        if not prepared:
            return None

        for item in prepared:
            if not item.path.exists() or item.path.stat().st_size != item.ciphertext_size:
                return None
            if hashlib.sha256(item.path.read_bytes()).hexdigest() != item.ciphertext_sha256:
                return None
        return manifest, prepared
    except Exception:
        return None


def build_v4_spool(
    session: UsbSession,
    info: DeviceInfo,
    chunks: list[Chunk],
    session_key: bytes,
    server_key_id: str,
    server_public_pem: bytes,
    log: Callable[[str], None],
    progress: Callable[[float, str], None],
) -> tuple[dict, list[OpusPreparedChunk]]:
    existing = load_v4_spool(session.uuid)
    if existing:
        log(f"{session.prefix}: bestaande v4 spool hergebruikt")
        return existing

    root = SPOOL_ROOT / session.uuid
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True, exist_ok=True)

    wrapped = wrap_session_key(session_key, server_public_pem)
    prepared: list[OpusPreparedChunk] = []
    manifest_chunks: list[dict] = []

    for index, chunk in enumerate(chunks, 1):
        progress(
            (index - 1) / len(chunks),
            f"{session.prefix}: Opus {index}/{len(chunks)} encoderen",
        )
        opus_plain = encode_opus_plaintext(chunk)
        nonce, aad = v4_nonce_and_aad(session_key, session.uuid, chunk.sequence)
        ciphertext = AESGCM(session_key).encrypt(nonce, opus_plain, aad.encode("ascii"))

        out_path = root / f"chunk-{chunk.sequence:06d}.opus.enc"
        tmp_path = out_path.with_suffix(out_path.suffix + ".tmp")
        tmp_path.write_bytes(ciphertext)
        tmp_path.replace(out_path)

        plaintext_sha = hashlib.sha256(opus_plain).hexdigest()
        ciphertext_sha = hashlib.sha256(ciphertext).hexdigest()
        item = {
            "sequence": chunk.sequence,
            "file": f"audio/chunk-{chunk.sequence:06d}.opus.enc",
            "nonce_b64": base64.b64encode(nonce).decode("ascii"),
            "aad": aad,
            "plaintext_sha256": plaintext_sha,
            "ciphertext_sha256": ciphertext_sha,
            "plaintext_size": len(opus_plain),
            "ciphertext_size": len(ciphertext),
            "start_offset_ms": chunk.start_offset_ms,
            "duration_ms": chunk.duration_ms,
        }
        manifest_chunks.append(item)
        prepared.append(_manifest_chunk_to_prepared(root, item))

    manifest = {
        "schema_version": 2,
        "session_id": session.uuid,
        "device_id": info.device_id,
        "mode": session.mode,
        "status": "complete",
        "audio": {
            "codec": "opus",
            "container": "ogg",
            "sample_rate": SPEECH_RATE,
            "channels": 1,
            "sample_format": "opus",
            "chunk_seconds": SYNC_CHUNK_SECONDS,
            "bitrate": OPUS_BITRATE,
            "frame_ms": OPUS_FRAME_MS,
        },
        "encryption": {
            "algorithm": "AES-256-GCM",
            "local_key_wrap": None,
            "server_key_wrap": {
                "algorithm": "RSA-OAEP-SHA256",
                "key_id": server_key_id,
                "ciphertext_b64": wrapped,
            },
        },
        "chunks": manifest_chunks,
    }

    manifest_tmp = root / "manifest.json.tmp"
    manifest_tmp.write_text(
        json.dumps(manifest, ensure_ascii=False, separators=(",", ":")),
        encoding="utf-8",
    )
    manifest_tmp.replace(root / "manifest.json")
    total = sum(item.ciphertext_size for item in prepared)
    log(
        f"{session.prefix}: v4 Opus klaar, {len(prepared)} chunks, "
        f"{total / 1024 / 1024:.2f} MB encrypted wire-data"
    )
    return manifest, prepared


def delete_v4_spool(uuid: str) -> None:
    root = SPOOL_ROOT / uuid
    if root.exists():
        shutil.rmtree(root)


def parse_events(path: Path) -> list[dict]:
    result: list[dict] = []
    with path.open("r", encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            try:
                offset = int(row.get("elapsed_ms", ""))
            except ValueError:
                continue
            event = (row.get("event") or "").strip()
            if not event:
                continue
            item: dict[str, int | str] = {"event": event, "offset_ms": offset}
            try:
                patient = int(row.get("patient", "0") or 0)
            except ValueError:
                patient = 0
            if patient > 0:
                item["patient_index"] = patient
            result.append(item)
    return result


class ApiSync:
    def __init__(
        self,
        info: DeviceInfo,
        log: Callable[[str], None],
        progress: Callable[[float, str], None],
    ):
        self.info = info
        self.log = log
        self.progress = progress
        self.http = requests.Session()
        self.server_key_id, self.server_public_pem = fetch_server_key(
            self.http, self.info
        )

    def probe_status(self, uuid: str) -> tuple[bool, dict | None]:
        r = self.http.get(
            self.info.base_url.rstrip("/") + f"/v1/sessions/{uuid}/status",
            headers=api_headers(self.info),
            timeout=30,
        )
        if r.ok:
            return True, r.json()
        if api_error_code(r) == "UNKNOWN_SESSION":
            return False, None
        r.raise_for_status()
        return False, None

    def status(self, uuid: str) -> dict:
        exists, body = self.probe_status(uuid)
        if not exists or body is None:
            raise RuntimeError("server kent sessie onverwacht niet")
        return body

    def _post_events_complete_confirm(
        self,
        session: UsbSession,
        events_path: Path,
        chunk_count: int,
    ) -> dict:
        base = self.info.base_url.rstrip("/")
        events = parse_events(events_path)
        r = self.http.post(
            base + f"/v1/sessions/{session.uuid}/events",
            headers=api_headers(
                self.info,
                {
                    "Content-Type": "application/json",
                    "Idempotency-Key": f"{session.uuid}:events",
                },
            ),
            json={"events": events},
            timeout=60,
        )
        r.raise_for_status()

        r = self.http.post(
            base + f"/v1/sessions/{session.uuid}/complete",
            headers=api_headers(
                self.info,
                {
                    "Content-Type": "application/json",
                    "Idempotency-Key": f"{session.uuid}:complete",
                },
            ),
            json={"chunk_count": chunk_count, "status": "complete"},
            timeout=60,
        )
        r.raise_for_status()

        remote = self.status(session.uuid)
        if not confirmed(remote):
            raise RuntimeError(
                f"server bevestigt ingest niet: missing={remote.get('missing_chunks')}"
            )
        return remote

    def _upload_v4(
        self,
        session: UsbSession,
        manifest: dict,
        prepared: list[OpusPreparedChunk],
        events_path: Path,
        create_session: bool,
    ) -> None:
        base = self.info.base_url.rstrip("/")

        if create_session:
            create = self.http.post(
                base + "/v1/sessions",
                headers=api_headers(
                    self.info,
                    {
                        "Content-Type": "application/json",
                        "Idempotency-Key": f"{session.uuid}:create",
                    },
                ),
                data=json.dumps(
                    manifest, ensure_ascii=False, separators=(",", ":")
                ).encode("utf-8"),
                timeout=60,
            )
            if create.status_code not in (200, 201):
                code = api_error_code(create)
                try:
                    detail = create.json()
                except ValueError:
                    detail = create.text[:500]
                raise RuntimeError(
                    f"v4 sessie aanmaken mislukt HTTP {create.status_code} "
                    f"{code or ''}: {detail}"
                )

        remote = self.status(session.uuid)
        if confirmed(remote):
            self.log(f"{session.prefix}: server had v4 sessie al compleet")
            delete_v4_spool(session.uuid)
            return

        expected = remote.get("expected_chunks")
        if expected is not None and int(expected) != len(prepared):
            raise RuntimeError(
                f"server verwacht {expected} chunks, v4 spool bevat {len(prepared)}"
            )

        missing = remote.get("missing_chunks")
        needed = (
            {int(x) for x in missing}
            if isinstance(missing, list)
            else {item.sequence for item in prepared}
        )
        upload = [item for item in prepared if item.sequence in needed]
        total_bytes = sum(item.ciphertext_size for item in upload)
        sent_bytes = 0
        started = time.monotonic()

        for item in upload:
            self.progress(
                sent_bytes / total_bytes if total_bytes else 1.0,
                f"{session.prefix}: Opus chunk {item.sequence}/{len(prepared)} upload",
            )
            headers = api_headers(
                self.info,
                {
                    "Content-Type": "application/octet-stream",
                    "X-Chunk-SHA256": item.ciphertext_sha256,
                    "X-Chunk-Nonce": item.nonce_b64,
                    "X-Chunk-AAD": item.aad,
                    "X-Plaintext-SHA256": item.plaintext_sha256,
                    "Idempotency-Key": f"{session.uuid}:chunk:{item.sequence}",
                },
            )
            with item.path.open("rb") as payload:
                r = self.http.put(
                    base + f"/v1/sessions/{session.uuid}/chunks/{item.sequence}",
                    headers=headers,
                    data=payload,
                    timeout=120,
                )

            if not r.ok:
                try:
                    detail = r.json()
                except ValueError:
                    detail = r.text[:500]
                raise RuntimeError(
                    f"Opus chunk {item.sequence} geweigerd HTTP {r.status_code}: {detail}"
                )

            try:
                result = r.json()
            except ValueError as exc:
                raise RuntimeError(
                    f"Opus chunk {item.sequence}: serverresponse is geen JSON"
                ) from exc

            if result.get("codec") != "opus":
                raise RuntimeError(
                    f"Opus chunk {item.sequence}: server meldt codec={result.get('codec')!r}"
                )
            if result.get("flac_deep_verified") is not True:
                raise RuntimeError(
                    f"Opus chunk {item.sequence}: deep verify niet bevestigd: {result}"
                )

            sent_bytes += item.ciphertext_size
            self.progress(
                sent_bytes / total_bytes if total_bytes else 1.0,
                f"{session.prefix}: {sent_bytes / 1024:.0f} KiB Opus geupload",
            )
            self.log(
                f"{session.prefix}: chunk {item.sequence}/{len(prepared)} "
                f"Opus OK ({item.ciphertext_size / 1024:.0f} KiB)"
            )

        self._post_events_complete_confirm(
            session, events_path, len(prepared)
        )
        elapsed = time.monotonic() - started
        wire_bytes = sum(item.ciphertext_size for item in prepared)
        self.log(
            f"{session.prefix}: v4 bevestigd, {len(prepared)} Opus chunks, "
            f"{wire_bytes / 1024 / 1024:.2f} MB, uploadfase {elapsed:.1f}s"
        )
        delete_v4_spool(session.uuid)

    def sync(
        self,
        session: UsbSession,
        local_wavs: list[Path],
        events_path: Path,
        session_key: bytes,
    ) -> None:
        chunks = describe_chunks(local_wavs)
        if not chunks:
            raise RuntimeError("geen geldige audiochunks")

        exists, remote = self.probe_status(session.uuid)
        spool = load_v4_spool(session.uuid)

        if exists:
            if remote and confirmed(remote):
                self.log(f"{session.prefix}: server had sessie al compleet")
                delete_v4_spool(session.uuid)
                return

            if spool:
                manifest, prepared = spool
                self.log(
                    f"{session.prefix}: bestaande onvoltooide v4 sessie hervatten"
                )
                self._upload_v4(
                    session, manifest, prepared, events_path, create_session=False
                )
                return

            # The recorder's Wi-Fi path is deliberately still v3.  Never post a
            # v4 manifest over an existing session UUID: the API binds codec to
            # the session and would (correctly) reject that as a conflict.
            raise ExistingSessionNeedsV3(
                f"{session.prefix}: sessie bestaat al op server maar heeft geen "
                "lokale v4 spool; vermoedelijk v3. Maak deze eenmalig af via "
                "M5 Wi-Fi-sync."
            )

        self.log(
            f"{session.prefix}: nieuwe sessie -> API 1.6 v4 Ogg/Opus "
            "16k mono 24 kbit/s"
        )
        manifest, prepared = build_v4_spool(
            session,
            self.info,
            chunks,
            session_key,
            self.server_key_id,
            self.server_public_pem,
            self.log,
            self.progress,
        )
        self._upload_v4(
            session, manifest, prepared, events_path, create_session=True
        )


class SyncApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("VisiteScribe Sync")
        self.root.geometry("720x470")
        self.q: queue.Queue[tuple] = queue.Queue()
        self.stop = threading.Event()
        self.worker: threading.Thread | None = None

        outer = ttk.Frame(root, padding=12)
        outer.pack(fill="both", expand=True)

        self.status = tk.StringVar(value="Zoeken naar VisiteScribe...")
        ttk.Label(outer, textvariable=self.status, font=("Segoe UI", 14, "bold")).pack(
            anchor="w"
        )

        self.detail = tk.StringVar(value="Sluit de M5 aan via USB.")
        ttk.Label(outer, textvariable=self.detail).pack(anchor="w", pady=(2, 10))

        self.progress = ttk.Progressbar(outer, maximum=100)
        self.progress.pack(fill="x", pady=(0, 10))

        self.logbox = ScrolledText(outer, height=18, state="disabled")
        self.logbox.pack(fill="both", expand=True)

        bottom = ttk.Frame(outer)
        bottom.pack(fill="x", pady=(10, 0))
        ttk.Button(bottom, text="Opnieuw zoeken", command=self.restart).pack(side="left")
        ttk.Button(bottom, text="Sluiten", command=self.close).pack(side="right")

        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.root.after(100, self._drain_queue)
        self.restart()

    def post(self, kind: str, *args) -> None:
        self.q.put((kind, *args))

    def log(self, text: str) -> None:
        self.post("log", text)

    def set_progress(self, fraction: float, text: str) -> None:
        self.post("progress", max(0.0, min(1.0, fraction)), text)

    def restart(self) -> None:
        if self.worker and self.worker.is_alive():
            return
        self.stop.clear()
        self.progress["value"] = 0
        self.status.set("Zoeken naar VisiteScribe...")
        self.detail.set("M5 mag al aangesloten zijn of later worden aangesloten.")
        self.worker = threading.Thread(target=self._run, daemon=True)
        self.worker.start()

    @staticmethod
    def _port_present(port: str) -> bool:
        return any(p.device == port for p in list_ports.comports())

    def _wait_for_disconnect(self, port: str) -> None:
        self.post(
            "status",
            "Alles gesynchroniseerd",
            "U kunt VisiteScribe loskoppelen; de app wacht automatisch op de volgende aansluiting.",
        )
        while not self.stop.is_set() and self._port_present(port):
            time.sleep(0.5)

        if not self.stop.is_set():
            self.post("progress", 0.0, "")
            self.post(
                "status",
                "Wachten op VisiteScribe...",
                "Sluit de M5 aan; synchronisatie start automatisch.",
            )

    def _sync_connected_device(self, device: VisiteScribeUsb) -> None:
        self.post("status", "VisiteScribe gevonden", device.port)

        info: DeviceInfo | None = None
        while not self.stop.is_set():
            info = device.enter()
            if info is not None:
                break
            self.post(
                "status",
                "Recorder is bezig",
                "Wachten tot de opname is gestopt...",
            )
            time.sleep(2.0)

        if self.stop.is_set() or info is None:
            return

        self.log(f"Device: {info.device_id}")
        self.log(f"Server: {info.base_url}")

        sessions = device.list_sessions()
        if not sessions:
            self.log("Geen openstaande sessies.")
            self.post("progress", 1.0, "Geen openstaande sessies")
            return

        self.log(f"{len(sessions)} sessie(s) te synchroniseren")
        api = ApiSync(info, self.log, self.set_progress)

        for index, s in enumerate(sessions, 1):
            if self.stop.is_set():
                return
            self.post(
                "status",
                f"Synchroniseren {index}/{len(sessions)}",
                f"{s.prefix} via USB → PC → server",
            )

            with tempfile.TemporaryDirectory(prefix=f"visitescribe-{s.prefix}-") as td:
                temp = Path(td)
                total_download = (s.events.size if s.events else 0) + sum(
                    w.size for w in s.wavs
                )
                downloaded_before = 0

                local_wavs: list[Path] = []
                for wi, remote_wav in enumerate(s.wavs):
                    local = temp / f"audio-{wi:03d}.wav"

                    def wav_progress(done: int, total: int, base=downloaded_before):
                        fraction = (base + done) / total_download if total_download else 1.0
                        self.set_progress(
                            fraction,
                            f"{s.prefix}: audio van M5 {100*fraction:.0f}%",
                        )

                    device.read_file(remote_wav, local, wav_progress)
                    downloaded_before += remote_wav.size
                    local_wavs.append(local)

                if s.events is None:
                    raise RuntimeError(f"{s.prefix}: eventsbestand ontbreekt")
                events_local = temp / "events.csv"

                def event_progress(done: int, total: int, base=downloaded_before):
                    fraction = (base + done) / total_download if total_download else 1.0
                    self.set_progress(
                        fraction,
                        f"{s.prefix}: bestanden van M5 {100*fraction:.0f}%",
                    )

                device.read_file(s.events, events_local, event_progress)
                key = device.session_key(s.uuid)
                api.sync(s, local_wavs, events_local, key)
                device.mark_ingested(s.prefix, s.uuid)

            self.log(f"{s.prefix}: lokaal op M5 gemarkeerd als ingested")

        self.post("progress", 1.0, "Synchronisatie voltooid")

    def _run(self) -> None:
        # Permanent watcher: once started, never require a manual 'search'
        # action again.  A successful sync returns to device-arrival waiting.
        while not self.stop.is_set():
            device: VisiteScribeUsb | None = None
            port: str | None = None
            try:
                while not self.stop.is_set() and device is None:
                    device = VisiteScribeUsb.discover(self.log)
                    if device is None:
                        self.post(
                            "status",
                            "Wachten op VisiteScribe...",
                            "Sluit de M5 aan; synchronisatie start automatisch.",
                        )
                        time.sleep(1.0)

                if not device or self.stop.is_set():
                    return

                port = device.port
                self._sync_connected_device(device)

            except Exception as exc:
                self.log(f"FOUT: {type(exc).__name__}: {exc}")
                self.post("status", "Sync fout", f"{exc} — nieuwe poging volgt automatisch")
            finally:
                if device:
                    try:
                        device.exit()
                    except Exception:
                        pass
                    device.close()

            if self.stop.is_set():
                return

            # After a normal run, wait for the physical USB disconnect before
            # arming discovery again.  Otherwise the still-connected M5 would
            # immediately be rediscovered in a tight no-op sync loop.
            if port and self._port_present(port):
                self._wait_for_disconnect(port)
            else:
                time.sleep(1.0)

    def _drain_queue(self) -> None:
        try:
            while True:
                item = self.q.get_nowait()
                kind = item[0]
                if kind == "log":
                    self.logbox.configure(state="normal")
                    self.logbox.insert("end", time.strftime("%H:%M:%S ") + item[1] + "\n")
                    self.logbox.see("end")
                    self.logbox.configure(state="disabled")
                elif kind == "status":
                    self.status.set(item[1])
                    self.detail.set(item[2])
                elif kind == "progress":
                    self.progress["value"] = item[1] * 100
                    self.detail.set(item[2])
        except queue.Empty:
            pass
        if not self.stop.is_set():
            self.root.after(100, self._drain_queue)

    def close(self) -> None:
        self.stop.set()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    SyncApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
