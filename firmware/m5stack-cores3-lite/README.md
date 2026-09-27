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


## Pocket-first power-button controls

The demo/production CoreS3-Lite firmware uses the physical PWR button as the
primary control for home visits:

- if the LCD is asleep, the first short PWR press only wakes the display;
- with the display awake and no recording active:
  - single PWR starts recording after the short double-click decision window;
  - double PWR opens MENU;
- after PWR starts a recording, audio capture begins immediately and a
  10-second VISITE / VERGADERING touch chooser is shown;
- if no choice is made within 10 seconds, VISITE is selected automatically;
- after the choice, touch input is ignored for the rest of the recording;
- during a VISITE, double PWR closes the current patient WAV and starts the
  next patient;
- during a VERGADERING, double PWR inserts a marker;
- during any recording, single PWR stops the recording;
- long PWR has no application action; there is no privacy mode in this control
  model.

VISITE uses patient-capable filenames from patient 1. A session with only one
patient is still reported as `single_patient`; patient 2+ changes the session
mode to `multi_patient`.

### Power saving

For pocket use the LCD controller enters real sleep shortly after the recording
type is locked. While the screen is asleep the UI timer is not redrawn and the
direct touch controller is not polled. M5Unified's duplicate touch polling is
also disabled because this firmware owns the FT6336 input path directly.
Battery polling outside STATUS is reduced to once per minute. Wi-Fi remains off
unless sync is explicitly requested.


## Direct Ogg/Opus recorder test

The `cores3-lite-direct-opus` environment is an experimental recorder backend
that deliberately writes **no WAV master**. It keeps the same pocket-first PWR
UI, but the audio path is:

```text
ES7210 48 kHz stereo
  -> stream downmix + 3:1 decimation
  -> 16 kHz mono PCM16, 20 ms
  -> Espressif Opus 24 kbit/s VBR / VOIP
  -> self-contained Ogg/Opus chunks <= 30 s on microSD
```

Each completed file is named:

```text
/visitescribe/s00029_chunk_000001.opus
/visitescribe/s00029_chunk_000002.opus
...
```

and `s00029_opus.csv` records sequence, path, exact source duration and file
size. A patient boundary closes the current logical recording segment and the
next Opus chunk sequence continues. Meeting markers remain events only.

The encoder runs at low priority on core 0; the M5 microphone task is pinned to
core 0 at higher priority so capture wins scheduling conflicts. A 24-frame
(480 ms) PCM queue absorbs short encoder/SD stalls. Any queue overflow is
reported as a dropped frame and makes the recording fail validation rather than
being silently accepted.

Every Ogg chunk has its own OpusHead/OpusTags, CRCs, stream serial and EOS.
The backend uses the normal libopus 312-sample 48 kHz pre-skip and an extra
encoder-drain packet so the final Ogg granule trims to the exact number of real
16 kHz input samples.

### Build / flash

From the local clone:

```powershell
cd C:\Projects\visitescribe
py -m platformio run -d firmware\m5stack-cores3-lite -e cores3-lite-direct-opus -t upload
```

The first build downloads the pinned Espressif `esp_audio_codec` 2.5.0
component.

### Expected serial evidence

During a recording the important lines are:

```text
DIRECT OPUS: worker READY ...
DIRECT OPUS: chunk 1 OPEN ...
DIRECT OPUS: frames=500 ... dropped=0
DIRECT OPUS: chunk 1 CLOSE frames=1500 duration=30000ms ... ok=1
...
DIRECT OPUS: worker DONE failed=0 ... dropped=0
```

A test is not considered successful if `dropped` is non-zero, a chunk closes
with `ok=0`, or the worker reports `failed=1`.

### USB / API path

Direct-Opus firmware advertises completed chunks with `VSUSB OPUS`. The PC
sync app downloads those small Ogg files directly, encrypts the exact bytes
into its normal durable v4 retry spool, and uploads them without ffmpeg or PCM
conversion. The existing v4 API contract already accepts this format.

Keep `cores3-lite-demo` as the proven WAV rollback build until direct Opus has
passed real hardware recording and server deep validation.


## Brian identity and direct-Opus sync

The CoreS3-Lite working name is **Brian** (a play on "Brain"). At boot the
display briefly uses a white splash screen with the supplied monochrome
OurMind logo in the upper-right, an animated cartoon robot doctor in the
centre, and the Brian name before the normal recorder UI appears.

The pocket menu contains only **STATUS / SYNC / TERUG**. The temporary LAN
benchmark remains in the repository as a development tool but is no longer
compiled into the normal/direct-Opus M5 build.

For `cores3-lite-direct-opus`, MENU > SYNC now uploads the recorder-native
Ogg/Opus chunks directly as API v4 audio. It does not look for a WAV master and
does not transcode the audio again. The recorder:

1. inventories completed `sNNNNN_opus.csv` sessions;
2. creates the existing v4 Ogg/Opus manifest;
3. derives the v4 Opus nonce/AAD domain;
4. AES-GCM encrypts each exact local Ogg chunk;
5. resumes only server-reported missing chunks when applicable;
6. posts events, completes the session, verifies durable ingest, and only then
   marks the local session `ingested`.

The SYNC screen stays awake for the full queue. While HTTP/server sync is
running, touch is deliberately locked and the footer says **SYNC LOOPT** rather
than presenting tappable retry/back controls, so an incidental wake/glance tap
cannot tear down Wi-Fi mid-upload.

Completed legacy sessions that contain no valid audio are retained on SD but
are marked `quarantined_no_audio` after detection. That makes them terminal
for automatic queue scans instead of showing the same historical session
numbers on every sync. Removing that session's `_sync.txt` manually makes it
eligible for a deliberate retry/recovery attempt.
