# VisiteScribe MINI - Waveshare ESP32-S3-Touch-AMOLED-1.64

OurMind-branded hardware/UI prototype for the Waveshare **ESP32-S3-Touch-AMOLED-1.64**.

The main build is still a **UI + touch + microSD workflow prototype**. Real microphone development lives in the separate `waveshare-amoled-1.64-im73d122-lab` firmware until the IM73D122 hardware has been validated.

## UI philosophy

The v0.5 interface is intentionally designed as a recorder rather than a tiny smartphone:

- the top 64 px are status only;
- every choice uses the rest of the screen in huge full-width touch zones;
- no small record/stop/privacy targets;
- recording uses three ~one-third-screen actions: PRIVACY / VOLGENDE or MARKER / STOP;
- privacy pause uses HERVAT / VOLGENDE or MARKER / STOP;
- after STOP the user gets VUL AAN or KLAAR;
- if untouched after STOP, the device returns to the main menu after 8 seconds;
- the main menu includes MENU -> STATUS and SYNC.

Colours are functional as well as branded: OurMind blue for normal actions, green for start/resume/complete, amber for privacy, red for stop, and dark navy for navigation/back.

## Main menu

The main menu has four large zones:

1. VISITE
2. PATIENTRONDE
3. VERGADERING
4. MENU

`MENU` opens:

- `STATUS` - battery, microSD, microphone integration state, Wi-Fi state and local session count;
- `SYNC` - manually turns Wi-Fi on and tries the configured networks;
- `TERUG`.

Wi-Fi is deliberately **OFF at boot** and remains off during normal recording. It is only enabled when the user explicitly enters SYNC, and is turned off again when leaving that screen. This reduces power consumption and avoids background network activity during recording.

## Battery status

The V2 board schematic routes `VBAT` through an onboard **200k/100k divider** to `BAT_ADC` on GPIO4. The firmware averages ADC readings, multiplies by three and shows an approximate LiPo percentage in the top-right corner.

The initial percentage curve is only an estimate. Once the real 1500/1800 mAh batteries are connected, calibrate it against measured run time and battery voltage.

If no plausible battery voltage is detected, the header shows `--%`.

## Local Wi-Fi configuration

**Never put Wi-Fi passwords in this public repository.**

The firmware looks for:

```text
include/wifi_secrets.h
```

That file is ignored by git. A safe template is committed as:

```text
include/wifi_secrets.example.h
```

Copy it locally:

```powershell
Copy-Item include\wifi_secrets.example.h include\wifi_secrets.h
```

Then edit `include/wifi_secrets.h` and fill in the two preferred SSIDs/passwords:

```cpp
#pragma once
#define VISITESCRIBE_WIFI_SSID_1 "YOUR_WIFI_SSID_1"
#define VISITESCRIBE_WIFI_PASSWORD_1 "YOUR_WIFI_PASSWORD"
#define VISITESCRIBE_WIFI_SSID_2 "YOUR_WIFI_SSID_2"
#define VISITESCRIBE_WIFI_PASSWORD_2 "YOUR_WIFI_PASSWORD"
```

The current SYNC screen performs the manual Wi-Fi connection/fallback test. Actual API upload is deliberately not faked in this UI-only build; that will be connected when the real audio/session format is merged from the microphone firmware.

## What works in this demo

- 280 x 456 AMOLED display
- FT3168 touch
- microSD detection and event logging
- modes: VISITE / PATIENTRONDE / VERGADERING
- very large touch targets throughout
- simulated recording timer
- privacy pause/resume
- marker / next-patient action
- stop + resume-same-session (`VUL AAN`)
- automatic return to main menu after completion
- battery voltage/percentage estimate
- STATUS screen
- on-request Wi-Fi with two-network fallback
- BOOT button long-press as physical emergency stop
- per-demo CSV event log under `/visitescribe/`

## Board revision

The project defaults to **Waveshare V2**. V2 uses LCD CS GPIO46; older V1 examples use GPIO9.

PlatformIO environments:

- `waveshare-v2` - default
- `waveshare-v1` - fallback for older hardware

## Requirements

- VS Code
- PioArduino / PlatformIO environment
- USB-C data cable
- microSD/TF card, FAT32 recommended

All Arduino dependencies are pulled automatically by PlatformIO. Arduino_GFX is pinned in `platformio.ini`.

## Flash

Open exactly this directory in VS Code:

```text
firmware/waveshare-amoled-1.64
```

Then:

```powershell
pio run -e waveshare-v2 -t upload
```

If upload cannot connect, force download mode: hold **BOOT**, press and release **RESET**, release **BOOT**, then retry.

## microSD output

On boot:

```text
/visitescribe/boot.log
```

Each demo session creates for example:

```text
/visitescribe/demo_0012ab34.csv
```

with rows such as:

```csv
elapsed_ms,event,patient,markers
0,session_started,1,0
8204,patient_boundary,2,1
12311,privacy_pause_started,2,1
15724,privacy_pause_ended,2,1
22449,session_stopped,2,1
22449,session_resumed,2,1
```

No patient identifiers are written.

## Next integration step

After the IM73D122 dual-PDM lab firmware has been validated on real microphones, merge its audio task into this large-touch recorder UI. At that point STATUS can report live `2/2` microphone health and SYNC can upload encrypted audio/session data using the existing VisiteScribe API contract.


## Production firmware

A new `waveshare-v2-production` environment combines the proven AMOLED/touch
workflow with the dual IM73D122 PDM recorder and the same automatic VSUSB v1
PC-sync protocol used by the CoreS3-Lite.

It records an authoritative local master as:

- 48 kHz
- 16-bit PCM
- stereo
- WAV on microSD

The production session layout matches the CoreS3-Lite:

```text
/visitescribe/s00001_events.csv
/visitescribe/s00001_sync.txt
/visitescribe/s00001_visit.wav
/visitescribe/s00002_round_p001_s01.wav
/visitescribe/s00002_round_p002_s01.wav
/visitescribe/s00003_meeting.wav
```

The existing Windows PC sync app therefore works without a Waveshare-specific
client. USB sync is automatic after plugging in the recorder. The PC app creates
API v4 Ogg/Opus sync copies; the WAV masters remain on the SD card.

Privacy pause disables the PDM receiver itself rather than merely suppressing
file writes.

### API credentials for PC sync

The recorder passes its API device ID/token to the PC app over USB. For the
current one-user prototype it is acceptable to reuse the CoreS3-Lite device
credentials. Copy the existing local secrets file:

```powershell
Copy-Item ..\m5stack-cores3-lite\include\server_secrets.h include\server_secrets.h
```

Or copy `include/server_secrets.example.h` to `server_secrets.h` and provision
a separate API device.

### Build and flash production

From `firmware/waveshare-amoled-1.64`:

```powershell
pio run -e waveshare-v2-production -t upload
```

The original `waveshare-v2` UI prototype and the separate IM73D122 lab project
remain available for regression testing.
