# VisiteScribe local LAN benchmark

This benchmark measures the raw local-network path between the CoreS3-Lite and
the laptop. It does **not** use the production API and it never sends patient
recordings.

The M5 sends deterministic dummy bytes to the laptop at:

```text
http://192.168.2.31:8765/upload
```

The Python receiver discards every payload in memory and only reports byte
counts and timing.

## Run on the laptop

From the repository root:

```powershell
cd D:\PROJECTS\visitescribe
py tools\lan-benchmark\server.py
```

Expected:

```text
VisiteScribe LAN benchmark receiver
Listening on 0.0.0.0:8765
No payload is written to disk.
```

If Windows asks whether Python may accept network connections, allow it on the
**Private** network.

Optional local health check in another PowerShell window:

```powershell
Invoke-RestMethod http://127.0.0.1:8765/health
```

## Trigger on the M5

Wake the M5, double-PWR to open MENU, then tap **LAN TEST**.

The benchmark joins one of the normal configured Wi-Fi profiles and sends two
payloads:

1. **19.2 MB** = exactly 10 minutes worth of 16 kHz / mono / PCM16.
2. **1.8 MB** = exactly 10 minutes worth of a 24 kbit/s Opus wire payload.

The second payload is only Opus-*sized* dummy data. It deliberately does not
run an encoder, so the measurement isolates network throughput.

The M5 serial log and the Python server both report throughput. The M5 also
estimates transfer time for one hour of 16 kHz mono PCM and one hour at
24 kbit/s.

## Why test PCM and Opus-sized data separately?

The production recorder keeps a 48 kHz stereo PCM WAV as the local
source-of-truth master. The current fast USB path creates 16 kHz mono PCM on the
fly, which is 32 kB/s. A 24 kbit/s Opus stream is nominally 3 kB/s, another
~10.7x smaller.

We deliberately benchmark transport first. Device-side post-recording Opus is a
separate experiment because live Opus encoding previously interfered with the
master recording path. Encoding only after STOP is much safer and is the next
candidate if the LAN benchmark shows that transport is no longer the limiting
factor.
