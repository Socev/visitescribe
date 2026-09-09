# VisiteScribe v0.2 - server ingest contract

Base URL: `https://scribe.primumnonnocere.olares.com`

The recorder stores each audio chunk as **AES-256-GCM ciphertext**. A random 256-bit session key is generated per recording. The Pi stores that session key only wrapped by the local device key. Before upload, the uploader unwraps the session key in RAM and re-wraps it with the server RSA public key using **RSA-OAEP-SHA256**. The audio itself is uploaded exactly as stored; it is never decrypted on the Pi for upload.

## Required device provisioning before upload is enabled

- mTLS client certificate: `/etc/visitescribe/client.crt`
- mTLS private key: `/etc/visitescribe/client.key`
- server RSA public wrapping key: `/etc/visitescribe/server-wrap-public.pem`

`visitescribe-admin enable-upload` refuses to enable uploads until these files exist.

## Endpoints

### `POST /v1/sessions`

Idempotent create/register call.

Headers:

- `X-Device-ID: visitescribe-001`
- `Idempotency-Key: <session_uuid>:create`

JSON body is the local `manifest.json`, except:

- `encryption.local_key_wrap` is `null`
- `encryption.server_key_wrap` contains:
  - `algorithm: RSA-OAEP-SHA256`
  - `ciphertext_b64`: server-public-key wrapped session key

Server should accept HTTP 200 or 201 for both first and repeated identical calls.

### `PUT /v1/sessions/{session_id}/chunks/{sequence}`

Body: raw encrypted `.flac.enc` bytes.

Headers:

- `Content-Type: application/octet-stream`
- `X-Device-ID`
- `X-Chunk-SHA256`
- `X-Chunk-Nonce` (base64)
- `X-Chunk-AAD`
- `X-Plaintext-SHA256`
- `Idempotency-Key: <session_uuid>:chunk:<sequence>`

The server verifies the ciphertext SHA-256 before acknowledging the chunk.

To decrypt:

1. RSA-OAEP-SHA256 decrypt `server_key_wrap.ciphertext_b64` to obtain the 32-byte session key.
2. Base64-decode `X-Chunk-Nonce`.
3. AES-256-GCM decrypt the request body using the session key, nonce, and UTF-8 bytes of `X-Chunk-AAD` as AAD.
4. Verify the decrypted FLAC SHA-256 against `X-Plaintext-SHA256`.

### `POST /v1/sessions/{session_id}/events`

Body:

```json
{"events": [{"event":"patient_boundary","offset_ms":73425}]}
```

Header: `Idempotency-Key: <session_uuid>:events`

### `POST /v1/sessions/{session_id}/complete`

Body:

```json
{
  "chunk_count": 12,
  "completed_at": "2026-09-08T22:10:00+02:00",
  "status": "complete"
}
```

Header: `Idempotency-Key: <session_uuid>:complete`

The server should only mark ingest complete after every expected chunk and event batch has been durably committed and verified.

### `GET /v1/sessions/{session_id}/status`

Return JSON such as:

```json
{"state":"INGESTED","ingest_confirmed":true}
```

The Pi regards `INGESTED`, `READY_FOR_PROCESSING`, `TRANSCRIBING`, `PROCESSING`, `REVIEW_REQUIRED`, or `APPROVED` as proof that durable ingest has completed.

## Retry semantics

The Pi uses exponential backoff from 30 seconds up to one hour with jitter. All writes are expected to be idempotent. A server must therefore tolerate repeated session creation, chunk PUTs, event submission, and completion calls.

## Local deletion

The implementation supports automatic deletion of encrypted audio after confirmed ingest, but v0.2 ships with this deliberately set to `false` in `config.json`. Enable only after the server's durable-ingest and backup behavior has been tested.
