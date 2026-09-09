# VisiteScribe v0.2

Pi-side recording stack for Raspberry Pi 4 + Jabra Speak 710 + Waveshare 1.3-inch SH1106 OLED HAT + PiSugar 3 Plus.

## v0.2 features

- three recording modes: single patient, multi-patient round, meeting
- physical patient-boundary / marker events
- real privacy pause (capture process is stopped)
- continuous ALSA recording with 30-second WAV splits in `/run` (RAM only)
- closed WAV chunks are encoded to FLAC in RAM, then AES-256-GCM encrypted before persistent storage
- per-session random encryption key
- persistent manifest + append-only event journal
- crash recovery: completed encrypted chunks survive; an unfinished RAM chunk can be lost
- upload queue independent of recorder
- server URL preconfigured, uploads disabled until mTLS and server wrapping key are provisioned
- resumable/idempotent encrypted chunk upload with SHA-256 verification
- battery and queue/network state on OLED
- v0.1 sessions are left untouched

## Important security note

The device master key used to wrap local session keys is stored on the Pi. This protects the audio files from casual/offline access to the SD filesystem but is not equivalent to a hardware-backed TPM. Before real clinical use, provision the server public wrapping key and mTLS identity, complete the DPIA/security review, and test lost-device procedures.

## Admin commands

```bash
sudo visitescribe-admin status
sudo visitescribe-admin selftest
sudo visitescribe-admin queue
sudo visitescribe-admin config
sudo visitescribe-admin enable-upload
sudo visitescribe-admin disable-upload
```

## Logs

```bash
sudo journalctl -u visitescribe-recorder -n 30 --no-pager
sudo journalctl -u visitescribe-uploader -n 30 --no-pager
```

## Current safe default

`upload_enabled` is false and `delete_encrypted_audio_after_ingest` is false. The device therefore records and queues completely offline until server credentials are intentionally provisioned.
