# VisiteScribe modern micro-opus benchmark

Standalone benchmark for the CoreS3-Lite / ESP32-S3. It deliberately does not
share the production Arduino firmware build. Flashing this project temporarily
replaces the recorder firmware and immediately runs a serial-only benchmark.

The benchmark uses micro-opus v0.4.1 pinned to commit
`8354085908683c6130e32a832aeec8a7ca115c51` with:

- ESP-IDF / pioarduino modern toolchain
- fixed-point Opus
- Xtensa optimizations
- 16 kHz mono
- 20 ms frames
- `OPUS_APPLICATION_VOIP`
- 24 kbit/s CBR
- complexity 1
- DTX and FEC disabled
- CoreS3-Lite 8 MB **Quad** PSRAM (not Octal/OPI)
- encoder state and 120 kB pseudostack prefer PSRAM

From the VisiteScribe repository root:

```powershell
C:\Users\DavidSchaap\.platformio\penv\Scripts\platformio.exe run -d firmware\opus-modern-benchmark -e esp32-s3 -t upload
C:\Users\DavidSchaap\.platformio\penv\Scripts\platformio.exe device monitor -b 115200
```

On the first build, `fetch_micro_opus.py` clones the exact micro-opus tag plus
its Opus submodule into the ignored local `components/micro-opus` directory.

Expected serial lines include:

```text
VS_OPUS_MODERN: block 1/10 audio=30s bytes=90000 ... realtime=...x ...
VS_OPUS_MODERN: DONE audio=300.0s ... realtime=...x ...
```

The meaningful comparison is against the old Arduino Opus probe, which measured
about 2.05x realtime at the same 16 kHz mono / 24 kbit/s / 20 ms settings.

To restore the normal recorder firmware afterwards:

```powershell
C:\Users\DavidSchaap\.platformio\penv\Scripts\platformio.exe run -d firmware\m5stack-cores3-lite -e cores3-lite -t upload
```

Or restore the earlier old-Opus menu benchmark with `-e cores3-lite-opus-test`.
