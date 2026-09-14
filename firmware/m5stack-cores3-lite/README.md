# VisiteScribe — M5Stack CoreS3-Lite

Working recorder firmware for the **M5Stack CoreS3-Lite**.

## Implemented

- CoreS3-Lite 320×240 touch UI
- VISITE / PATIENTRONDE / VERGADERING modes
- Real onboard **ES7210 dual-microphone** capture
- **48 kHz / 16-bit / stereo WAV** streamed directly to microSD
- No fixed recording-duration limit other than FAT32/WAV file limits
- Privacy pause physically stops microphone capture
- Patient rounds create a **separate WAV file for every patient**
- Marker/event CSV sidecar per session
- Crash recovery for unfinished `.wav.tmp` recordings
- Battery percentage from AXP2101
- Display dim after 12 s; backlight off after 60 s
- Safe wake: first touch/PWR press on a black display only wakes it
- Short press on the physical **PWR button = hardware START/STOP shortcut**
- Wi-Fi remains OFF except in MENU → SYNC
- Optional two-network local Wi-Fi configuration

Actual server upload is intentionally not yet implemented in this firmware. SYNC currently verifies network bring-up only. The recorder itself is fully offline-capable.

## Storage

Use a FAT32 microSD card. Firmware creates:

```text
/visitescribe/
  s00001_visit.wav
  s00001_events.csv
  s00002_round_p001.wav
  s00002_round_p002.wav
  s00002_events.csv
  ...
```

A patient boundary in PATIENTRONDE closes the current WAV before opening the next patient's WAV. This keeps downstream patient audio physically separated.

## Physical PWR button

Short press while running:

- Home → immediately starts a VISITE
- Mode confirmation → START
- Recording / privacy pause → STOP
- Finished screen → VUL AAN / resume same session in a new WAV segment

The CoreS3-Lite's normal long-hold power-off behaviour remains a board-level function.

## Build / flash with PlatformIO

From the repository root:

```powershell
cd firmware\m5stack-cores3-lite
pio run -t upload
```

If `pio` is not in PATH, use your PlatformIO executable directly, for example:

```powershell
C:\Users\DavidSchaap\.platformio\penv\Scripts\platformio.exe run --target upload --environment cores3-lite
```

To see serial output:

```powershell
pio device monitor -b 115200
```

### If upload does not find the board

The CoreS3-Lite supports a hardware download mode. Hold the **RST button for about 3 seconds** until the green indicator LED comes on, then release it and retry upload.

You can explicitly choose a COM port if needed:

```powershell
pio run -e cores3-lite -t upload --upload-port COM7
```

Replace `COM7` with the port shown by:

```powershell
pio device list
```

## Optional Wi-Fi

The recorder works without Wi-Fi.

To configure manual SYNC locally:

```powershell
Copy-Item include\wifi_secrets.example.h include\wifi_secrets.h
notepad include\wifi_secrets.h
```

`wifi_secrets.h` is ignored by the repository. Never commit credentials.

## Audio implementation notes

The firmware uses M5Unified's microphone API in stereo mode and streams completed double-buffered blocks to the SD card. The speaker is disabled while recording because microphone and speaker share the CoreS3 audio subsystem.

Current audio format:

- 48,000 Hz
- 16 bit PCM
- 2 channels
- approx. 192 kB/s
- approx. 691 MB/hour

A standard WAV/FAT32 file has an effective ~4 GB ceiling, corresponding to roughly 5.8 hours of continuous stereo recording in one file. Ordinary visits and meetings are well below this; automatic long-file rollover can be added later if required.

## Current caveat

This tree was written against the official CoreS3-Lite hardware documentation and current M5Unified API. It has not yet been compiled on the author's local CoreS3-Lite toolchain. If the first build reports an error, capture only the **first compiler error plus roughly the last 20 lines**; fix that before chasing later cascading errors.
