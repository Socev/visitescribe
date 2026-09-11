# VisiteScribe IM73D122 dual-microphone lab

Experimental microphone firmware for the Waveshare ESP32-S3-Touch-AMOLED-1.64 V2 and the Infineon KIT-IM73D122V01-FLEX.

This project is deliberately separate from the normal `firmware/waveshare-amoled-1.64` demo firmware. Its first job is to characterize the IM73D122 microphones before they are integrated into the full VisiteScribe recorder.

## Why two microphones first?

The IM73D122 is a PDM microphone designed to support a stereo pair on one shared clock/data bus. Both microphones receive the same PDM clock and share the same DATA wire. The SELECT pin decides which clock edge a microphone uses:

- LEFT microphone: SELECT -> GND
- RIGHT microphone: SELECT -> 3V3

That gives two independently captured microphone channels while using only two ESP32 signal GPIOs.

The first experiments intentionally preserve both channels as 48 kHz / 16-bit stereo PCM WAV. Do not mix the channels on the device yet; keeping them separate lets us compare microphones, placement, distance, noise floor and later server-side processing.

## Wiring for the first experiment

Firmware pin assignment:

| Waveshare V2 | IM73D122 left | IM73D122 right | Function |
| --- | --- | --- | --- |
| 3V3 | V / VDD | V / VDD | microphone power |
| GND | G / GND | G / GND | ground |
| GPIO5 | C / CLOCK | C / CLOCK | shared PDM clock |
| GPIO6 | D / DATA | D / DATA | shared PDM data |
| GND | S / SELECT | - | left channel select |
| 3V3 | - | S / SELECT | right channel select |

Use short wires during the first test. Around 3-8 cm is ideal; Infineon reports that direct-wired flex boards have been tested with wires up to 15 cm.

The flex kit contains five microphone flex boards but only one adapter board. For the cleanest symmetric two-microphone experiment, direct-solder the same wire type to two flex boards. Using one adapter and one direct-wired board is also fine for initial bring-up.

Do not connect microphone VDD to 5 V. IM73D122 supports 1.62-3.60 V; this prototype uses the Waveshare 3.3 V rail.

## Audio format

- 48,000 samples/s
- 16-bit signed PCM
- 2 channels
- PDM-to-PCM conversion in ESP32-S3 I2S0 hardware
- PDM down-sampling mode `I2S_PDM_DSR_8S`
- resulting microphone clock: 48,000 x 64 = 3.072 MHz

3.072 MHz is intentional: this is the clock at which Infineon specifies 73 dB(A) SNR for IM73D122.

ESP-IDF delivers the right PDM slot before the left slot in stereo mode. The firmware swaps each frame before writing WAV, so the saved file is conventional `LEFT, RIGHT` interleaved stereo.

Raw stereo WAV uses about 192 kB/s, roughly 691 MB/hour.

## Firmware behaviour

After boot the AMOLED shows live LEFT and RIGHT dBFS meters even when no recording is active. This makes wiring and channel assignment easy to test.

Touch `START WAV` to start recording. Touch `STOP WAV`, or long-press BOOT, to stop. Recordings are written to:

```text
/visitescribe/mic-lab/im73_XXXXXXXX.wav
```

During recording a `.wav.tmp` file is used. On a clean stop the WAV header is finalized and the file is renamed to `.wav`. This lab build is not yet the production crash-safe/chunked/encrypted VisiteScribe recorder.

## Build and flash

Open this directory as the PlatformIO/PioArduino project, or use a terminal:

```powershell
cd firmware/waveshare-amoled-1.64-im73d122-lab
pio run -t upload
```

Serial monitor:

```powershell
pio device monitor -b 115200
```

## First test when the kit arrives

1. Connect only power, GND, shared CLOCK, shared DATA and the two opposite SELECT levels as shown above.
2. Boot the device. It should show `PDM READY - SD READY`.
3. Speak or snap fingers near the left microphone while shielding/keeping distance from the right. LEFT should move much more strongly.
4. Repeat near the right microphone.
5. Record 30-60 seconds with speech at approximately 0.3 m, 1 m, 2 m and across the consulting room.
6. Copy the WAV files from microSD before changing physical microphone positions.

If the channels appear reversed, do not rewire immediately; first verify which physical flex has SELECT tied to GND versus 3V3. The firmware already compensates for ESP-IDF's right-first PDM stereo buffer order.

## Next experiments

After the two-microphone baseline is stable we can use additional flex microphones for array experiments. ESP32-S3 PDM RX supports multiple PDM data lines, so the remaining kit microphones can later be used to compare 2-, 3- and 4-microphone geometries. That should come after the two-channel baseline so we can measure whether the extra complexity actually improves transcription at distance.

For clinical/real-world VisiteScribe use, this lab firmware still needs the normal product features: privacy pause, patient boundaries, chunking, encryption, recovery, upload queue and server acknowledgement.
