from __future__ import annotations

import base64
import csv
import hashlib
import hmac
import io
import json
import queue
import struct
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


class VisiteScribeUsb:
    def __init__(self, ser: serial.Serial):
        self.ser = ser

    @staticmethod
    def _open_port(port: str) -> serial.Serial:
        ser = serial.Serial(
            port,
            USB_BAUD,
            timeout=0.35,
            write_timeout=10,
            inter_byte_timeout=1,
        )
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:
            pass
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
                time.sleep(0.2)
                ser.reset_input_buffer()
                dev = cls(ser)
                for _ in range(3):
                    dev._write_line("VSUSB HELLO")
                    line = dev._wait_protocol_line("VSUSB READY", timeout=0.8)
                    if line:
                        parts = line.split()
                        if len(parts) >= 3 and int(parts[2]) == USB_PROTOCOL:
                            log(f"VisiteScribe gevonden op {p.device}")
                            return dev
                    time.sleep(0.2)
                log(f"{p.device}: COM-poort bereikbaar, maar geen VisiteScribe-handshake.")
            except serial.SerialException as exc:
                log(
                    f"{p.device}: kan COM-poort niet openen ({exc}). "
                    "Sluit PlatformIO Serial Monitor of andere programma's die deze poort gebruiken."
                )
            except Exception as exc:
                log(f"{p.device}: detectiefout: {type(exc).__name__}: {exc}")
            finally:
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

    def enter(self) -> bool:
        self._write_line("VSUSB ENTER")
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            line = self._readline(1)
            if line == "VSUSB OK ENTER":
                return True
            if line == "VSUSB BUSY RECORDING":
                return False
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

    def status(self, uuid: str, fail: bool = True) -> dict | None:
        r = self.http.get(
            self.info.base_url.rstrip("/") + f"/v1/sessions/{uuid}/status",
            headers=api_headers(self.info),
            timeout=30,
        )
        if not r.ok:
            if fail:
                r.raise_for_status()
            return None
        return r.json()

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

        wrapped = wrap_session_key(session_key, self.server_public_pem)
        manifest = build_manifest(
            session, self.info, chunks, self.server_key_id, wrapped
        )
        fill_manifest_crypto(manifest, session_key, session.uuid)

        base = self.info.base_url.rstrip("/")
        create = self.http.post(
            base + "/v1/sessions",
            headers=api_headers(
                self.info,
                {
                    "Content-Type": "application/json",
                    "Idempotency-Key": f"{session.uuid}:create",
                },
            ),
            data=json.dumps(manifest, separators=(",", ":")).encode("utf-8"),
            timeout=60,
        )
        if create.status_code not in (200, 201, 409):
            create.raise_for_status()

        remote = self.status(session.uuid, fail=True) or {}
        if confirmed(remote):
            self.log(f"{session.prefix}: server had sessie al compleet")
            return

        expected = remote.get("expected_chunks")
        if expected is not None and int(expected) != len(chunks):
            raise RuntimeError(
                f"server verwacht {expected} chunks, lokaal zijn het er {len(chunks)}"
            )
        missing = remote.get("missing_chunks")
        if isinstance(missing, list):
            needed = {int(x) for x in missing}
        else:
            needed = {c.sequence for c in chunks}

        upload_chunks = [c for c in chunks if c.sequence in needed]
        total_bytes = sum(c.ciphertext_size for c in upload_chunks)
        sent_bytes = 0
        started = time.monotonic()

        for c in upload_chunks:
            self.progress(
                sent_bytes / total_bytes if total_bytes else 1.0,
                f"{session.prefix}: chunk {c.sequence}/{len(chunks)} voorbereiden",
            )
            ciphertext, meta = chunk_crypto(session_key, session.uuid, c)
            headers = api_headers(
                self.info,
                {
                    "Content-Type": "application/octet-stream",
                    "X-Chunk-SHA256": meta["ciphertext_sha256"],
                    "X-Chunk-Nonce": meta["nonce_b64"],
                    "X-Chunk-AAD": meta["aad"],
                    "X-Plaintext-SHA256": meta["plaintext_sha256"],
                    "Idempotency-Key": f"{session.uuid}:chunk:{c.sequence}",
                },
            )
            r = self.http.put(
                base + f"/v1/sessions/{session.uuid}/chunks/{c.sequence}",
                headers=headers,
                data=ciphertext,
                timeout=120,
            )
            r.raise_for_status()
            sent_bytes += len(ciphertext)
            self.progress(
                sent_bytes / total_bytes if total_bytes else 1.0,
                f"{session.prefix}: {sent_bytes / 1024 / 1024:.1f} MB geupload",
            )

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
            json={"chunk_count": len(chunks), "status": "complete"},
            timeout=60,
        )
        r.raise_for_status()

        remote = self.status(session.uuid, fail=True) or {}
        if not confirmed(remote):
            raise RuntimeError(
                f"server bevestigt ingest niet: missing={remote.get('missing_chunks')}"
            )

        elapsed = time.monotonic() - started
        self.log(
            f"{session.prefix}: server bevestigd, {len(chunks)} chunks in {elapsed:.1f}s"
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

    def _run(self) -> None:
        device: VisiteScribeUsb | None = None
        try:
            while not self.stop.is_set() and device is None:
                device = VisiteScribeUsb.discover(self.log)
                if device is None:
                    self.post("status", "Zoeken naar VisiteScribe...", "Geen M5 gevonden")
                    time.sleep(1.0)
            if not device or self.stop.is_set():
                return

            self.post("status", "VisiteScribe gevonden", device.port)

            while not self.stop.is_set():
                if device.enter():
                    break
                self.post("status", "Recorder is bezig", "Wachten tot de opname is gestopt...")
                time.sleep(2.0)

            if self.stop.is_set():
                return

            info = device.info()
            self.log(f"Device: {info.device_id}")
            self.log(f"Server: {info.base_url}")

            sessions = device.list_sessions()
            if not sessions:
                self.post("status", "Alles is gesynchroniseerd", "Geen openstaande sessies.")
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
            self.post(
                "status",
                "Alles gesynchroniseerd",
                "De originele WAV-bestanden blijven op de SD-kaart staan.",
            )
        except Exception as exc:
            self.log(f"FOUT: {type(exc).__name__}: {exc}")
            self.post("status", "Sync fout", str(exc))
        finally:
            if device:
                try:
                    device.exit()
                finally:
                    device.close()

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
