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

## Brian UX refresh (September 2026)

The current CoreS3-Lite direct-Opus build uses the white, pocket-first Brian UI.

Key behavior:

- boot keeps the existing Brian robot/OurMind composition, but plays the
  animation at half speed and holds the completed Brian frame for about three
  seconds;
- functional screens use a white background, near-black text, restrained
  violet accents and a fixed Brian/battery top bar;
- HOME shows **Klaar voor opname**, the Visite default and the number of local
  recordings still waiting to be sent;
- one PWR starts recording; double PWR opens the menu; a complete PWR gesture
  used only to wake a sleeping display is consumed and cannot leak into the
  next action;
- the VISITE/VERGADERING choice remains available for 10 seconds while audio is
  already recording; a resting finger cannot choose a mode until it has first
  been released and touched again;
- normal recording shows `Visite - Patient N` from patient 1 onward, a large
  per-patient timer, or Vergadering plus marker count; touch is not polled;
- double PWR during VISITE starts the next patient and resets the displayed
  patient timer; during VERGADERING it records a marker;
- stopping first shows **Opname bewaren...** and only shows **Opgeslagen** after
  the local Opus finalization succeeds;
- user-stopped recordings shorter than 10 seconds are treated as **Valse
  start** and all files belonging to that session are deleted after the encoder
  has safely stopped;
- audio/storage failures during an active recording stop the capture and show a
  blocking truthful error rather than a success screen;
- MENU is **Verzenden / Apparaatstatus / Terug**; technical codec, board and
  network information lives under Details;
- recording now dims after about 10 seconds and sleeps the LCD after about 30
  seconds; ordinary menus dim after about 15 seconds. Active server sync never
  dims or sleeps;
- server sync presents user-level phases and recording progress instead of
  session IDs/chunk IDs. Touch is not polled while the synchronous upload queue
  is active.

The direct-Opus audio format, API v4 contract, retry/resume behavior and PC/USB
sync protocol are unchanged by this UI work.


## Direct-Opus rollover buffering and diagnostics

A hardware trace on CoreS3-Lite showed a successful 30-second chunk close taking
487 ms, followed by a successful next-chunk open taking 309 ms. The previous
24-frame PCM queue covered only 480 ms. It filled during this normal rotation;
the producer then timed out after 20 ms, latched a failure, and set `audioError`
although the encoder worker remained ready and healthy. The earlier PCM bounds
fix prevented memory corruption but did not solve this queue exhaustion.

The queue now holds 512 frames (10.24 seconds) in a dedicated PSRAM allocation.
Its FreeRTOS control block stays in internal RAM. The queue is allocated once
and reused. One worker still owns the encoder and Ogg files; every 1500 source
frames it finalizes the independent 30-second stream and opens the next one.
There is no PCM/WAV master. Persistent stalls that exhaust this bounded queue
still report a real recording error rather than silently losing audio.

Serial diagnostics include close/open duration, queue depth/high-water mark,
worker and producer flags, and named failure locations. Start diagnostics split
storage setup, encoder/worker startup, and total startup time. HOME retains the
350 ms single/double-click window but gives immediate first-press feedback and
shows `Opname starten...` before synchronous initialization. The chooser labels
use ASCII `Patient` and `Vergadering`, without a Patient subtitle. PWR during
the ten-second chooser only cycles Patient -> Vergadering -> STOP -> Patient;
expiry confirms the highlighted choice: Patient/ Vergadering continue recording,
while STOP ends recording through the normal save/error path. The last actual
recording mode is committed before stopping. Cycling onward from STOP before
expiry continues recording.

Hardware regression procedure:

1. Build `cores3-lite-direct-opus`, flash, and capture serial at 115200 baud.
2. On HOME, double PWR must open Menu without starting a worker/recording.
3. Return HOME, single PWR starts recording. In the chooser, use three separate
   PWR presses to traverse Vergadering, STOP, Patient without stopping.
4. Record for at least 100 seconds. Require chunks 1, 2, and 3 to CLOSE with
   `frames=1500 duration=30000ms ok=1`, subsequent OPEN messages, `dropped=0`,
   and no audioError, panic, or reboot. Display sleep must not stop recording.
5. Wake the display with one PWR gesture; after the wake guard, stop with a new
   single PWR. Require worker DONE with `failed=0`, `dropped=0`, and saved UI.
6. Separately confirm that touching STOP within 10 seconds removes a false
   start; leaving STOP selected until chooser expiry must stop and save normally.
   Cycling STOP -> Patient before expiry must keep recording.

If the esptool upload stub cannot verify the flash connection, the installed
esptool ROM path (`--before usb_reset --no-stub`) can program the application
at 0x10000 on this existing partition layout; preserve bootloader, partition
table, NVS and SD recordings. Require successful hash verification.


## Interrupted direct-Opus recordings

An active-session journal is written before capture. Boot and pre-sync recovery
scan journaled sessions while the encoder is stopped. Each approximately one
second, the open Ogg file is flushed to publish its FAT size/allocation. Recovery
copies complete CRC-valid Ogg pages from an interrupted `.opus.tmp`, marks the
last durable page EOS, validates the copy, and retains the original temporary
file. Completed chunks are not modified. CSV metadata is rebuilt with its
original retained as `.before-recovery`; a separate recovery marker makes the
interrupted session eligible for the existing sync path. Patient boundary events
remain unchanged. Existing sync manifests are not rewritten.

For older recordings without a journal, maintenance USB mode additionally accepts
`VSUSB RECOVER sNNNNN`. Recovery does not upload audio. Empty temporary files
cannot yield audio; corrupt chunks or internal sequence gaps block publication.
The latest unflushed audio, PCM still in RAM, and torn pages can be lost on reset.
SD hardware failure can also prevent recovery; this is not a zero-loss guarantee.
Explicitly stopped false starts under ten seconds keep their existing deletion
behavior. PWR in the initial chooser still only cycles the three choices.

Audio failures persist first-failure reason, source line, worker stage, queue
state and dropped-frame counts in a session `_failure.txt` when storage permits.
The larger PSRAM queue adds storage-stall tolerance. It does not establish the
cause of an older field failure for which no diagnostic trace was retained.
USB serial writes have a short timeout during recording to avoid long debug
output stalls when the device is unplugged.

Hardware recovery verification (2026-09-28): a recording interrupted with RESET
was automatically recovered at boot as one 16,793 ms chunk. USB readback decoded
successfully with FFmpeg `-v error -xerror`; the original 53,277-byte temporary
file was retained. Older interrupted sessions were also recovered explicitly;
all completed original Opus files and event logs remained byte-identical to the
USB backup. Server ingestion has not been exercised by these recovery tests.

A subsequent hardware run exceeded 100 seconds: chunks 1, 2 and 3 each closed
with 1500 frames, 30000 ms and ok=1, and chunk 4 opened. No frames were dropped
and no audio error, panic or reboot appeared during those rollovers. The observed
queue high-water mark was 45/512 frames across the first three rotations.


## Confirmed-sync retention and progress

Confirmed (`ingested`) sessions are eligible for removal after 72 full hours.
A separate `_synced_at.txt` records the first confirmation time and matching
session UUID. Repeated confirmations do not move that deadline. All files with
the exact session prefix are removed, including audio, temporary recovery copies,
events, metadata and diagnostics. The sync receipt is deleted last. Interrupted
cleanup retains that receipt, and allocation reserves its session number until
cleanup finishes. Queued, failed and quarantined recordings are never eligible.

Deletion requires an SNTP-synchronized clock in the current boot; the build-date
TLS clock seed cannot authorize deletion. Older confirmed sessions without a
recorded time, corrupt timestamps, and USB confirmations without network time
receive a new full 72-hour grace period at the next valid-clock check. This avoids
inventing an old confirmation date. Clock rollback defers deletion.

Cleanup runs before Wi-Fi sync and hourly while HOME is idle. It never runs during
recording, a live encoder worker or USB transfer. A powered-off/offline device may
therefore retain files longer than 72 hours; after reboot it needs network time.

Throughout preparation, upload, event submission and server confirmation, the
progress line consistently says `Opname N van M`. N advances only after a session
is confirmed (or explicitly skipped). It no longer alternates between the current
session number and the smaller count of already completed sessions.

The in-memory filesystem regression test is `tests/retention_host.cpp`. It uses
the actual retention header and covers the 72-hour boundary, missing/invalid time,
UUID mismatch, non-ingested states, busy guards, short writes, failed deletion,
and reset after each of five deletion steps. It never accesses real SD files.
Run with a C++17 host compiler, for example from a Visual Studio developer prompt:
`cl /EHsc /std:c++17 tests\retention_host.cpp /Fe:retention_host.exe`, then
`retention_host.exe`. Firmware builds also compile retention/progress boundary
assertions. Hardware deletion and the updated display still require validation.


## Single-pass pending inventory

`sync_inventory.h` walks the recording directory once and groups event files and
legacy WAV paths by exact session prefix. Terminal sync states are excluded;
remaining sessions still need a normal stop or recovery marker. Direct-Opus
metadata is validated first. WAV fallback uses paths from the same inventory,
without scanning the directory again for every session.

Wi-Fi sync reuses the validated session/chunk metadata within that invocation.
There is no persistent inventory cache: later recordings, recoveries, confirmed
uploads and retention changes are reflected on the next invocation. The uploader
still opens and checks the actual audio when preparing each upload. Before Wi-Fi sync, retention, active-journal discovery and pending inventory
share one call-scoped filename snapshot. Actual recovery of an interrupted
recording can still inspect its chunks separately; normal maintenance does not
rescan the directory. Metadata/audio are read freshly, so files removed by
retention are not accidentally included from the earlier snapshot.

Serial reports the shared directory pass timing, then `SERVER: inventory
directory_passes=0 files=... pending=... elapsed=...ms` when reusing that snapshot.
Standalone inventory calls report one directory pass. Host test `tests/inventory_host.cpp` includes the retention tests
and exercises the actual inventory header against 60 simulated sessions, including
Opus, legacy WAV, recovered, incomplete, corrupt and already ingested cases. It
asserts one directory pass per inventory and fresh results after files/status
change. Build/run with a C++17 host compiler as for the retention host test.


## Successful sync returns HOME

After a successful server-confirmed upload, or a result that everything was
already synchronized, the result stays visible for ten seconds. The v0.6.7 loop
then switches Wi-Fi off and returns HOME, where normal display dim/sleep resumes.
Active transfers and error results never start this timer. An explicit departure
from the result cancels its timer; the next sync gets a fresh ten seconds. Pending
PWR clicks are consumed on automatic return to avoid starting a recording.
`tests/sync_result_timer_host.cpp` covers the deadline, reset and millis wrap.

USB maintenance diagnostic `VSUSB POWERPROBE` records up to 60 seconds of AXP2101
status, VBUS voltage, battery voltage and charge direction to
`/visitescribe/power_probe.csv`. It runs only when capture/session/server sync is
idle and changes no charger settings. This is for validating Bottom3 dock
visibility; the Bottom3 TP4057 charger is separate from the CoreS3 power chip.
The command is not a dock detection implementation.


Bottom3 hardware check (2026-09-28): with USB disconnected, placement on the
powered magnetic base raised battery voltage from about 4.07 V to 4.12 V; removal
lowered it again. The user confirmed the base charge LED lit. Throughout both
states, AXP2101 registers 0x00/0x01 stayed 24/85, VBUS stayed zero and charge status
stayed -1 (discharge). The base's TP4057 charges independently. Battery voltage can indicate charging,
but load changes can produce similar movement.
Automatic sync now uses an idle battery-voltage heuristic, without hardware changes.
After a stable display-load baseline, a rise of at least 25 mV must persist for
10 seconds (at least 15 mV raw and 20 mV filtered above baseline). A 10-second
"Opladen... / Sync wordt gestart" countdown then allows cancellation by touch
or PWR. A USB power attachment observed while idle uses the same countdown;
booting already connected to USB does not trigger it. Recording, network work,
USB maintenance and POWERPROBE suspend detection. Display load changes reset
the baseline, including any pending voltage-rise confirmation. Cancellation/starting latches detection until a sustained
25 mV fall, or USB removal. Milestones are stored in charge_sync_log.csv.
This is an indication, not proof of charging: full batteries, placement during
recording or baseline collection, and small rises may be missed; unobserved
load changes may cause false positives. Manual SYNC remains available.
Official schematic: https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/531/M5GO3.pdf


Display timeout (2026-09-29): every idle screen, including Wi-Fi failure,
server errors, saving, charging countdown and USB maintenance, dims after 15s
and sleeps after 30s. Recording/paused keeps its existing 10s/30s timing.
Only active server work holds the screen awake; USB file reads wake it and
refresh the deadline. New app screens, Wi-Fi results and server transfer
completion receive a fresh readable interval; ordinary redraws do not.
PWR on a sleeping screen remains wake-only. Voltage confirmation never delays
display sleep and restarts its baseline after a display load transition.


Reset recovery UI (2026-09-29): boot displays storage checking/recovery with
elapsed seconds before returning to HOME. PWR during this blocking operation
only wakes the display and is consumed; the pending gesture is cleared before
HOME. Recovery shares one directory snapshot across interrupted sessions,
retaining full Ogg CRC validation and original interrupted files. A short false
start deletes only known current-session files/chunk numbers, with no directory
walk. The worker must be stopped and synced/recovered sessions are rejected;
the events file is removed last to reserve the session ID on deletion failure.
Boot duration is logged as RECOVERY: boot check finished. SD enumeration and
CRC validation still take time; the UI makes this work visible, not instantaneous.
