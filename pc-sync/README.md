# VisiteScribe PC Sync

Windows MVP voor een tweede sync-route:

```
M5 CoreS3-Lite -> USB -> PC -> bestaande VisiteScribe API
```

De 48 kHz stereo WAV-master blijft altijd op de microSD staan. De PC-app leest
alleen complete, nog niet als ingested gemarkeerde sessies. Voor de huidige API
maakt de PC dezelfde 16 kHz mono PCM16 / 30 s chunks als firmware v0.6.7.

## Installatie

Open PowerShell in deze map:

```powershell
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

Start daarna:

```powershell
.\.venv\Scripts\python.exe visitescribe_sync.py
```

## Gebruik

Beide volgordes werken:

1. start de app en sluit daarna de M5 via USB aan; of
2. sluit de M5 eerst aan en start daarna de app.

De app scant beschikbare COM-poorten en gebruikt pas een device nadat het
expliciet antwoordt met `VSUSB READY 1`.

Als de M5 op dat moment opneemt, antwoordt hij `BUSY RECORDING` en wacht de
app tot de opname is gestopt.

Daarna:

- de M5 levert device-id, API-token en per-sessie session key via USB;
- de PC downloadt WAV + events vanaf SD;
- de PC converteert 48 kHz stereo -> 16 kHz mono PCM16;
- de PC versleutelt identiek aan de bestaande firmware;
- de PC uploadt via de bestaande API;
- pas na server-confirmatie stuurt de PC `MARK` terug;
- de originele WAV-bestanden worden nooit verwijderd.

Na de run stuurt de app `VSUSB EXIT` zodat de M5 weer normaal bruikbaar is.

## USB protocol v1

Line commands zijn ASCII. Alleen `READ` bevat een binair payload.

```text
VSUSB HELLO
VSUSB ENTER
VSUSB INFO
VSUSB LIST
VSUSB KEY <uuid>
VSUSB READ <path> <offset> <length>
VSUSB MARK <prefix> <uuid>
VSUSB EXIT
```

Een READ-response is:

```text
VSUSB DATA <n>\n
<n raw bytes>
\nVSUSB ENDDATA <n>\n
```

Tijdens `ENTER` draait de normale firmware-loop niet; daardoor kan debugoutput
de binaire USB-stream niet vervuilen.

## Dependencies

- Python 3.11+ aanbevolen
- pyserial
- requests
- numpy
- cryptography
- Tkinter (standaard aanwezig in de normale Windows Python-installatie)

## Nog niet gedaan

- Geen installer/EXE; voor deze MVP bewust Python.
- Geen nieuw gecomprimeerd wire-format. Zodra het API-contract daarvoor bekend
  is, kan de PC-codeclaag worden vervangen zonder het USB-protocol te wijzigen.
