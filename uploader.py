#!/usr/bin/env python3
import base64
import hashlib
import json
import os
import random
import signal
import ssl
import time
from datetime import datetime
from pathlib import Path

import requests
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

CONFIG_PATH = Path('/etc/visitescribe/config.json')
DEVICE_KEY_PATH = Path('/etc/visitescribe/device.key')
DATA_DIR = Path('/var/lib/visitescribe/sessions')
STATUS_PATH = Path('/var/lib/visitescribe/uploader-status.json')

running = True


def sig_handler(*_):
    global running
    running = False


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
    with tmp.open('w', encoding='utf-8') as f:
        os.chmod(tmp, mode)
        f.write(json.dumps(obj, indent=2, ensure_ascii=False) + '\n')
        f.flush(); os.fsync(f.fileno())
    os.replace(tmp, path)


def write_status(state, queue_count, **extra):
    atomic_json(STATUS_PATH, {
        'at': now_iso(),
        'state': state,
        'queue_count': queue_count,
        **extra,
    }, mode=0o644)


def queued_sessions():
    result = []
    if not DATA_DIR.exists():
        return result
    for d in sorted(DATA_DIR.iterdir()):
        if not d.is_dir():
            continue
        manifest = read_json(d / 'manifest.json')
        if not manifest or manifest.get('schema_version') != 2:
            continue
        if manifest.get('status') not in ('complete', 'interrupted'):
            continue
        upload = read_json(d / 'upload.json', {}) or {}
        if upload.get('state') in ('ingested', 'purged'):
            continue
        result.append((d, manifest, upload))
    return result


def unwrap_session_key(manifest, device_key):
    wrap = manifest['encryption']['local_key_wrap']
    nonce = base64.b64decode(wrap['nonce_b64'])
    ciphertext = base64.b64decode(wrap['ciphertext_b64'])
    aad = wrap['aad'].encode('utf-8')
    return AESGCM(device_key).decrypt(nonce, ciphertext, aad)


def wrap_for_server(session_key, public_key_path):
    public_key = serialization.load_pem_public_key(Path(public_key_path).read_bytes())
    wrapped = public_key.encrypt(
        session_key,
        padding.OAEP(mgf=padding.MGF1(algorithm=hashes.SHA256()), algorithm=hashes.SHA256(), label=None),
    )
    return base64.b64encode(wrapped).decode('ascii')


def events_from_file(path):
    events = []
    try:
        with Path(path).open('r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if line:
                    events.append(json.loads(line))
    except FileNotFoundError:
        pass
    return events


def session_requests(config):
    server = config['server']
    cert = None
    if server.get('require_mtls', True):
        cert = (server['client_cert'], server['client_key'])
    verify = server.get('ca_bundle') or True
    s = requests.Session()
    s.headers.update({'User-Agent': 'VisiteScribe/0.2'})
    return s, cert, verify


def update_upload_state(session_dir, upload, **changes):
    upload.update(changes)
    atomic_json(session_dir / 'upload.json', upload)


def retry_state(session_dir, upload, exc, base=30, maximum=3600):
    attempts = int(upload.get('attempts', 0)) + 1
    delay = min(maximum, base * (2 ** min(attempts - 1, 7)))
    delay = int(delay * random.uniform(0.85, 1.15))
    update_upload_state(
        session_dir, upload,
        state='queued', attempts=attempts,
        next_retry_at=int(time.time()) + delay,
        last_error=str(exc)[:500], last_attempt_at=now_iso(),
    )


def upload_one(config, session_dir, manifest, upload, device_key):
    server = config['server']
    base_url = server['base_url'].rstrip('/')
    timeout = (float(server.get('connect_timeout_seconds', 10)), float(server.get('read_timeout_seconds', 60)))
    session, cert, verify = session_requests(config)

    session_key = unwrap_session_key(manifest, device_key)
    wrapped_server_key = wrap_for_server(session_key, server['server_wrap_public_key'])
    manifest_for_server = json.loads(json.dumps(manifest))
    manifest_for_server['encryption']['local_key_wrap'] = None
    manifest_for_server['encryption']['server_key_wrap'] = {
        'algorithm': 'RSA-OAEP-SHA256',
        'ciphertext_b64': wrapped_server_key,
    }

    headers = {
        'X-Device-ID': config.get('device_id', 'visitescribe-001'),
        'Idempotency-Key': f"{manifest['session_id']}:create",
    }
    r = session.post(f'{base_url}/v1/sessions', json=manifest_for_server, headers=headers, cert=cert, verify=verify, timeout=timeout)
    r.raise_for_status()
    update_upload_state(session_dir, upload, state='uploading', last_error=None, last_attempt_at=now_iso())

    already = set(str(x) for x in upload.get('uploaded_chunks', []))
    for chunk in manifest.get('chunks', []):
        seq = int(chunk['sequence'])
        if str(seq) in already:
            continue
        chunk_path = session_dir / chunk['file']
        data = chunk_path.read_bytes()
        actual_sha = hashlib.sha256(data).hexdigest()
        if actual_sha != chunk['ciphertext_sha256']:
            raise RuntimeError(f'Local ciphertext hash mismatch for chunk {seq}')
        ch = {
            'Content-Type': 'application/octet-stream',
            'X-Device-ID': config.get('device_id', 'visitescribe-001'),
            'X-Chunk-SHA256': actual_sha,
            'X-Chunk-Nonce': chunk['nonce_b64'],
            'X-Chunk-AAD': chunk['aad'],
            'X-Plaintext-SHA256': chunk['plaintext_sha256'],
            'Idempotency-Key': f"{manifest['session_id']}:chunk:{seq}",
        }
        r = session.put(
            f"{base_url}/v1/sessions/{manifest['session_id']}/chunks/{seq}",
            data=data, headers=ch, cert=cert, verify=verify, timeout=timeout,
        )
        r.raise_for_status()
        already.add(str(seq))
        upload['uploaded_chunks'] = sorted(already, key=int)
        update_upload_state(session_dir, upload, state='uploading', uploaded_chunks=upload['uploaded_chunks'])

    events = events_from_file(session_dir / 'events.jsonl')
    r = session.post(
        f"{base_url}/v1/sessions/{manifest['session_id']}/events",
        json={'events': events},
        headers={'X-Device-ID': config.get('device_id', 'visitescribe-001'), 'Idempotency-Key': f"{manifest['session_id']}:events"},
        cert=cert, verify=verify, timeout=timeout,
    )
    r.raise_for_status()

    r = session.post(
        f"{base_url}/v1/sessions/{manifest['session_id']}/complete",
        json={'chunk_count': len(manifest.get('chunks', [])), 'completed_at': manifest.get('completed_at'), 'status': manifest.get('status')},
        headers={'X-Device-ID': config.get('device_id', 'visitescribe-001'), 'Idempotency-Key': f"{manifest['session_id']}:complete"},
        cert=cert, verify=verify, timeout=timeout,
    )
    r.raise_for_status()

    r = session.get(
        f"{base_url}/v1/sessions/{manifest['session_id']}/status",
        headers={'X-Device-ID': config.get('device_id', 'visitescribe-001')},
        cert=cert, verify=verify, timeout=timeout,
    )
    r.raise_for_status()
    body = r.json() if r.content else {}
    confirmed = bool(body.get('ingest_confirmed')) or str(body.get('state', '')).upper() in ('INGESTED', 'READY_FOR_PROCESSING', 'TRANSCRIBING', 'PROCESSING', 'REVIEW_REQUIRED', 'APPROVED')
    if not confirmed:
        raise RuntimeError(f"Server did not confirm ingest; state={body.get('state')}")

    update_upload_state(session_dir, upload, state='ingested', ingested_at=now_iso(), server_status=body, last_error=None)

    if config.get('storage', {}).get('delete_encrypted_audio_after_ingest', False):
        audio_dir = session_dir / 'audio'
        for p in audio_dir.glob('*.enc'):
            p.unlink(missing_ok=True)
        atomic_json(session_dir / 'purge.json', {'purged_at': now_iso(), 'reason': 'server_ingest_confirmed'})
        update_upload_state(session_dir, upload, state='purged', purged_at=now_iso())


def credentials_ready(config):
    server = config.get('server', {})
    if not server.get('server_wrap_public_key') or not Path(server['server_wrap_public_key']).exists():
        return False, 'server wrap public key ontbreekt'
    if server.get('require_mtls', True):
        if not server.get('client_cert') or not Path(server['client_cert']).exists():
            return False, 'mTLS client certificate ontbreekt'
        if not server.get('client_key') or not Path(server['client_key']).exists():
            return False, 'mTLS client key ontbreekt'
    return True, None


def main():
    signal.signal(signal.SIGTERM, sig_handler)
    signal.signal(signal.SIGINT, sig_handler)
    device_key = DEVICE_KEY_PATH.read_bytes()
    while running:
        config = read_json(CONFIG_PATH, {}) or {}
        queue = queued_sessions()
        count = len(queue)
        server = config.get('server', {})
        if not server.get('upload_enabled', False):
            write_status('disabled', count, server=server.get('base_url'))
            time.sleep(3); continue
        ok, why = credentials_ready(config)
        if not ok:
            write_status('waiting_credentials', count, error=why, server=server.get('base_url'))
            time.sleep(5); continue
        if not queue:
            write_status('idle', 0, server=server.get('base_url'))
            time.sleep(5); continue

        session_dir, manifest, upload = queue[0]
        if int(upload.get('next_retry_at', 0) or 0) > int(time.time()):
            write_status('offline', count, error=upload.get('last_error'), next_retry_at=upload.get('next_retry_at'))
            time.sleep(3); continue

        write_status('uploading', count, session_id=manifest.get('session_id'), server=server.get('base_url'))
        try:
            upload_one(config, session_dir, manifest, upload, device_key)
            write_status('online', max(0, count - 1), server=server.get('base_url'))
        except requests.exceptions.RequestException as exc:
            retry_state(session_dir, upload, exc)
            write_status('offline', count, error=str(exc)[:200], server=server.get('base_url'))
        except Exception as exc:
            retry_state(session_dir, upload, exc)
            write_status('error', count, error=str(exc)[:200], server=server.get('base_url'))
        time.sleep(2)


if __name__ == '__main__':
    main()
