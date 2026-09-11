# VisiteScribe MINI - Waveshare ESP32-S3-Touch-AMOLED-1.64

First hardware/UI prototype for the Waveshare **ESP32-S3-Touch-AMOLED-1.64**.

This is deliberately a **UI + touch + microSD bring-up build**. The board itself has no onboard microphone, so v0.1 does not record audio yet. It simulates a recording while exercising the intended VisiteScribe workflow and writes demo events to microSD.

## What works in this demo

- 280 x 456 AMOLED display
- FT3168 touch
- microSD detection and event logging
- modes: VISITE / PATIENTRONDE / VERGADERING
- simulated recording timer
- privacy pause/resume
- marker / next-patient action
- stop action
- BOOT button long-press as physical emergency stop
- per-demo CSV event log under `/visitescribe/` on the card

## Board revision

The project defaults to **Waveshare V2**. Waveshare has both V1 and V2 hardware. V2 uses LCD CS GPIO46; older V1 examples use GPIO9.

PlatformIO contains two environments:

- `waveshare-v2` - default
- `waveshare-v1` - fallback if you have older hardware

If the firmware uploads but the screen remains completely black, verify the revision printed on the PCB and try the other environment before changing anything else.

## Requirements

- VS Code
- PlatformIO IDE extension
- USB-C data cable
- microSD/TF card, FAT32 recommended

All Arduino dependencies are pulled automatically by PlatformIO. The project pins Arduino_GFX to v1.6.7.

## Flash

1. Clone `https://github.com/Socev/visitescribe.git` or update an existing clone.
2. In VS Code choose **File -> Open Folder** and open exactly:
   `firmware/waveshare-amoled-1.64`
3. Wait until PlatformIO has installed the ESP32 toolchain and Arduino_GFX.
4. Insert the microSD card in the board.
5. Connect the board via USB-C.
6. Use the PlatformIO **Upload** action. The default environment is `waveshare-v2`.
7. After upload the board should reboot and show the VisiteScribe MINI home screen.

If upload cannot connect, force download mode: hold **BOOT**, press and release **RESET**, then release **BOOT**, and upload again.

## Serial monitor

PlatformIO serial monitor speed: `115200`.

Expected startup lines include:

```text
VisiteScribe MINI - Waveshare AMOLED UI demo v0.1
Board revision target: V2
microSD: OK
No microphone is present on this board; recording is simulated.
```

Touch coordinates are also printed for debugging.

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
```

No patient identifiers are written.

## Next step

Once the display/touch/SD build is proven on the physical unit, add an external digital MEMS microphone over I2S/PDM. Then replace the simulated recorder with real PCM chunk capture, encryption and the same VisiteScribe API contract used by the Raspberry Pi implementation.
