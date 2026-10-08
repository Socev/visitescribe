// VisiteScribe CoreS3-Lite v0.6.7
//
// Performance layer over v0.6.6:
// - keeps one HTTPClient alive for the complete chunk upload sequence so
//   HTTP/1.1 keep-alive can preserve the TCP/TLS connection between chunks;
// - replaces the 12-16 KiB SD read loop with a 512 KiB PSRAM scratch buffer;
// - splits preparation timing into SD read, downsample/mix, SHA and AES time;
// - leaves recording format, manifests, crypto identity, resume semantics and
//   the original 48 kHz stereo WAV masters unchanged.

// push/pop so an outer layer (v0.8) can rename this file's setup()/loop() too.
#pragma push_macro("setup")
#pragma push_macro("loop")
#undef setup
#undef loop
#define setup setup_v066_base
#define loop loop_v066_base
#include "main_v066.cpp"
#undef setup
#undef loop
#pragma pop_macro("loop")
#pragma pop_macro("setup")

// v0.8 fleet hooks around the upload queue (unset = unchanged behaviour).
// vsPreUploadHook runs with Wi-Fi up, before any recording is uploaded; false
// skips the uploads (e.g. the device still waits to be linked).
// vsPostUploadHook runs after the queue, e.g. to install a firmware update.
static bool (*vsPreUploadHook)() = nullptr;
static void (*vsPostUploadHook)() = nullptr;

#include <esp32-hal-psram.h>
#include "sync_result_timer.h"
#include "power_probe.h"

static constexpr size_t VS067_SCRATCH_BYTES = 512U * 1024U;
static uint8_t* vs067Scratch = nullptr;
static bool vs067EnsureScratch();

// Forward declaration: LIST advertises the compact virtual speech WAV before
// the helper implementation later in this overlay.
static bool vsUsbSpeechInfo(const String& path,
                            uint32_t& virtualBytes,
                            WAVHeader* outHeader = nullptr,
                            uint32_t* payloadStart = nullptr,
                            uint32_t* groupBytesOut = nullptr);

static bool vsUsbSyncActive = false;
static String vsUsbRxLine;

// Native ESP32-S3 HW CDC can enumerate at the PC after the recorder has already
// been running on battery, while its RX path remains stale until the MCU is
// reset.  Recover only the USB CDC peripheral on a physical hot-plug; never
// touch SD, audio capture or the recorder session.
static bool vsUsbPlugStateKnown = false;
static bool vsUsbWasPlugged = false;
static bool vsUsbRecoveryPending = false;
static uint32_t vsUsbLastRecoveryMs = 0;

static bool vsUsbRecorderBusy() {
  return captureRunning ||
         state == AppState::RECORDING ||
         state == AppState::PAUSED;
}

static void vsUsbHotplugService() {
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
  const bool plugged = Serial.isPlugged();

  if (!vsUsbPlugStateKnown) {
    vsUsbPlugStateKnown = true;
    vsUsbWasPlugged = plugged;
    return;
  }

  if (!plugged) {
    vsUsbWasPlugged = false;
    vsUsbRecoveryPending = false;
    return;
  }

  if (!vsUsbWasPlugged) {
    vsUsbWasPlugged = true;
    vsUsbRecoveryPending = true;
  }

  if (!vsUsbRecoveryPending ||
      vsUsbSyncActive ||
      vsUsbRecorderBusy() ||
      millis() - vsUsbLastRecoveryMs < 1500) {
    return;
  }

  // Reinitialising HW CDC intentionally makes Windows drop and recreate the
  // COM handle.  The PC watcher already tolerates that re-enumeration.
  vsUsbRecoveryPending = false;
  vsUsbLastRecoveryMs = millis();
  Serial.end();
  delay(80);
  Serial.begin(115200);
  delay(250);
#endif
}

static bool vsUsbSafePath(const String& path) {
  return path.startsWith("/visitescribe/") &&
         path.indexOf("..") < 0 &&
         path.length() < 180;
}

static void vsUsbDraw(const char* status, const char* sub = nullptr) {
#if VS_STICK
  drawHeader("");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_BG);
  centeredText(HEADER_H + 11, "USB SYNC", C_NAVY, 2);
  if (sub && sub[0]) centeredText(HEADER_H + 30, sub, C_GREY, 1);
  stickWrapped(90, status, C_BLUE, 2, 3);
  stickWrapped(160, "PC beheert sync - opnames blijven lokaal", C_GREY, 1, 3);
  return;
#endif
  drawHeader("USB SYNC", sub);
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_BG);
  centeredText(105, status, C_BLUE, 2);
  centeredText(145, "PC beheert sync - SD blijft lokaal", C_GREY, 1);
}

static void vsUsbReplyInfo() {
  const String tokenB64 = vsBase64(
      reinterpret_cast<const uint8_t*>(VISITESCRIBE_DEVICE_TOKEN),
      strlen(VISITESCRIBE_DEVICE_TOKEN));
  Serial.printf("VSUSB INFO %s %s %s\n",
                VISITESCRIBE_DEVICE_ID,
                VISITESCRIBE_SERVER_BASE_URL,
                tokenB64.c_str());
}

#ifdef VISITESCRIBE_DIRECT_OPUS
struct VsUsbDirectOpusRow {
  uint32_t sequence = 0;
  String path;
  uint32_t durationMs = 0;
  uint32_t bytes = 0;
};

static std::vector<VsUsbDirectOpusRow> vsUsbDirectOpusRows(const String& prefix) {
  std::vector<VsUsbDirectOpusRow> rows;
  const String metaPath = String("/visitescribe/") + prefix + "_opus.csv";
  File meta = SD.open(metaPath, FILE_READ);
  if (!meta) return rows;

  bool first = true;
  while (meta.available()) {
    String line = meta.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    if (first) {
      first = false;
      if (line.startsWith("sequence,")) continue;
    }

    const int c1 = line.indexOf(',');
    const int c2 = c1 >= 0 ? line.indexOf(',', c1 + 1) : -1;
    const int c3 = c2 >= 0 ? line.indexOf(',', c2 + 1) : -1;
    if (c1 <= 0 || c2 <= c1 || c3 <= c2) continue;

    VsUsbDirectOpusRow row;
    row.sequence = static_cast<uint32_t>(line.substring(0, c1).toInt());
    row.path = line.substring(c1 + 1, c2);
    row.durationMs = static_cast<uint32_t>(
        line.substring(c2 + 1, c3).toInt());
    row.bytes = static_cast<uint32_t>(line.substring(c3 + 1).toInt());

    if (!row.sequence || !row.durationMs || !vsUsbSafePath(row.path)) continue;
    File opus = SD.open(row.path, FILE_READ);
    if (!opus) continue;
    row.bytes = static_cast<uint32_t>(opus.size());
    opus.close();
    if (row.bytes) rows.push_back(row);
  }
  meta.close();

  std::sort(rows.begin(), rows.end(),
            [](const VsUsbDirectOpusRow& a, const VsUsbDirectOpusRow& b) {
              return a.sequence < b.sequence;
            });
  return rows;
}

static String vsUsbDirectMode(const String& eventsPath) {
  File events = SD.open(eventsPath, FILE_READ);
  if (!events) return "single_patient";

  bool meeting = false;
  bool patientBoundary = false;
  while (events.available()) {
    String line = events.readStringUntil('\n');
    if (line.indexOf(",mode_selected_meeting,") >= 0) meeting = true;
    if (line.indexOf(",patient_boundary,") >= 0) patientBoundary = true;
  }
  events.close();

  if (meeting) return "meeting";
  if (patientBoundary) return "multi_patient";
  return "single_patient";
}

static std::vector<String> vsUsbDirectPendingPrefixes() {
  std::vector<String> prefixes;
  File dir = SD.open("/visitescribe");
  if (!dir) return prefixes;

  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      String base = vsBaseName(f.name());
      if (base.startsWith("s") &&
          base.endsWith("_events.csv") &&
          base.length() >= 6) {
        const String prefix = base.substring(0, 6);
        const String eventsPath = String("/visitescribe/") + base;
        String uuid, syncState;
        const bool hasMeta = vsReadSyncMeta(prefix, uuid, syncState);
        if ((!hasMeta || !vsLocalSyncStateTerminal(syncState)) &&
            vsEventsShowComplete(eventsPath)) {
          const auto opus = vsUsbDirectOpusRows(prefix);
          if (!opus.empty()) prefixes.push_back(prefix);
        }
      }
    }
    f.close();
  }
  dir.close();

  std::sort(prefixes.begin(), prefixes.end(),
            [](const String& a, const String& b) {
              return a.compareTo(b) < 0;
            });
  prefixes.erase(
      std::unique(prefixes.begin(), prefixes.end(),
                  [](const String& a, const String& b) { return a == b; }),
      prefixes.end());
  return prefixes;
}

static void vsUsbReplyDirectOpusList() {
  Serial.println("VSUSB LISTING START");
  Serial.flush();

  const auto prefixes = vsUsbDirectPendingPrefixes();
  Serial.printf("VSUSB LISTING %u\n", (unsigned)prefixes.size());
  Serial.flush();

  uint32_t listed = 0;
  for (const auto& prefix : prefixes) {
    const String eventsPath =
        String("/visitescribe/") + prefix + "_events.csv";
    const auto opus = vsUsbDirectOpusRows(prefix);
    if (opus.empty()) continue;

    String uuid, syncState;
    if (!vsEnsureSessionUuid(prefix, uuid, syncState)) {
      Serial.printf("VSUSB SKIP %s NO_UUID\n", prefix.c_str());
      continue;
    }

    File events = SD.open(eventsPath, FILE_READ);
    const uint32_t eventsSize =
        events ? static_cast<uint32_t>(events.size()) : 0;
    if (events) events.close();

    const String mode = vsUsbDirectMode(eventsPath);
    Serial.printf("VSUSB SESSION %s %s %s %u %u\n",
                  prefix.c_str(), uuid.c_str(), mode.c_str(),
                  (unsigned)opus.size(), (unsigned)eventsSize);
    Serial.printf("VSUSB EVENTS %u %s\n",
                  (unsigned)eventsSize, eventsPath.c_str());

    for (const auto& row : opus) {
      Serial.printf("VSUSB OPUS %lu %lu %lu %s\n",
                    (unsigned long)row.sequence,
                    (unsigned long)row.bytes,
                    (unsigned long)row.durationMs,
                    row.path.c_str());
    }
    Serial.println("VSUSB ENDSESSION");
    ++listed;
  }

  Serial.printf("VSUSB ENDLIST %lu\n", (unsigned long)listed);
}
#endif

static void vsUsbReplyList() {
#ifdef VISITESCRIBE_DIRECT_OPUS
  vsUsbReplyDirectOpusList();
  return;
#endif

  // LIST may need several seconds on a full/slow SD card. Tell the PC
  // immediately that the recorder is alive before starting directory scans.
  Serial.println("VSUSB LISTING START");
  Serial.flush();

  std::vector<VsLocalSession> preparedSessions;
  const auto prefixes = vsPendingPrefixes(&preparedSessions);

  Serial.printf("VSUSB LISTING %u\n", (unsigned)prefixes.size());
  Serial.flush();

  uint32_t listed = 0;
  size_t preparedIndex = 0;
  for (const auto& prefix : prefixes) {
    VsLocalSession local;
    if (!vsLoadLocalSession(prefix, local, &preparedSessions[preparedIndex++])) continue;

    const uint32_t chunks = vsCountChunks(local, false);
    if (chunks == 0) {
      Serial.printf("VSUSB SKIP %s NO_AUDIO\n", prefix.c_str());
      continue;
    }

    File events = SD.open(local.eventsPath, FILE_READ);
    const size_t eventsSize = events ? events.size() : 0;
    if (events) events.close();

    Serial.printf("VSUSB SESSION %s %s %s %u %u\n",
                  local.prefix.c_str(), local.uuid.c_str(), local.mode.c_str(),
                  (unsigned)local.wavs.size(), (unsigned)eventsSize);
    Serial.printf("VSUSB EVENTS %u %s\n",
                  (unsigned)eventsSize, local.eventsPath.c_str());
    for (const auto& path : local.wavs) {
      File wav = SD.open(path, FILE_READ);
      const size_t size = wav ? wav.size() : 0;
      if (wav) wav.close();
      Serial.printf("VSUSB WAV %u %s\n", (unsigned)size, path.c_str());

      uint32_t speechBytes = 0;
      if (vsUsbSpeechInfo(path, speechBytes) && speechBytes > sizeof(WAVHeader)) {
        Serial.printf("VSUSB SPEECH %u %s\n",
                      (unsigned)speechBytes, path.c_str());
      }
    }
    Serial.println("VSUSB ENDSESSION");
    ++listed;
  }
  Serial.printf("VSUSB ENDLIST %lu\n", (unsigned long)listed);
}

static bool vsUsbSpeechInfo(const String& path,
                            uint32_t& virtualBytes,
                            WAVHeader* outHeader,
                            uint32_t* payloadStart,
                            uint32_t* groupBytesOut) {
  virtualBytes = 0;
  if (!vsUsbSafePath(path)) return false;

  File f = SD.open(path, FILE_READ);
  if (!f) return false;

  WAVHeader h;
  if (!vsReadWavHeader(f, h)) {
    f.close();
    return false;
  }

  uint32_t ratio = 0, groupBytes = 0;
  if (!vsSpeechSourceShape(h, ratio, groupBytes)) {
    f.close();
    return false;
  }

  const uint32_t start = static_cast<uint32_t>(f.position());
  const uint32_t physical = static_cast<uint32_t>(f.size());
  f.close();

  uint32_t sourceBytes = h.dataSize;
  const uint32_t physicalPayload = physical > start ? physical - start : 0;
  if (sourceBytes > physicalPayload) sourceBytes = physicalPayload;
  sourceBytes -= sourceBytes % groupBytes;

  const uint32_t outputSamples = sourceBytes / groupBytes;
  const uint32_t outputDataBytes = outputSamples * sizeof(int16_t);

  WAVHeader out = h;
  out.audioFormat = 1;
  out.numChannels = VS_SPEECH_CHANNELS;
  out.sampleRate = VS_SPEECH_RATE;
  out.bitsPerSample = VS_SPEECH_BITS;
  out.blockAlign = VS_SPEECH_CHANNELS * (VS_SPEECH_BITS / 8);
  out.byteRate = VS_SPEECH_RATE * out.blockAlign;
  out.dataSize = outputDataBytes;
  out.fileSize = 36 + outputDataBytes;

  virtualBytes = sizeof(WAVHeader) + outputDataBytes;
  if (outHeader) *outHeader = out;
  if (payloadStart) *payloadStart = start;
  if (groupBytesOut) *groupBytesOut = groupBytes;
  return true;
}

static bool vsUsbWriteAll(const uint8_t* data, size_t len) {
  size_t written = 0;
  while (written < len) {
    const size_t n = Serial.write(data + written, len - written);
    if (n == 0) {
      if (!Serial.isPlugged()) return false;
      delay(1);
      continue;
    }
    written += n;
  }
  return true;
}

static void vsUsbReadSpeech(const String& path, uint32_t offset, uint32_t wanted) {
  static constexpr uint32_t MAX_READ = 256U * 1024U;
  if (!vsUsbSafePath(path) || wanted == 0 || wanted > MAX_READ) {
    Serial.println("VSUSB ERROR READSPEECH_ARGS");
    return;
  }
  if (!vs067EnsureScratch()) {
    Serial.println("VSUSB ERROR READSPEECH_BUFFER");
    return;
  }

  uint32_t virtualBytes = 0, payloadStart = 0, groupBytes = 0;
  WAVHeader outHeader;
  if (!vsUsbSpeechInfo(path, virtualBytes, &outHeader, &payloadStart, &groupBytes)) {
    Serial.println("VSUSB ERROR READSPEECH_INFO");
    return;
  }
  if (offset > virtualBytes) {
    Serial.println("VSUSB ERROR READSPEECH_SEEK");
    return;
  }

  uint32_t sendBytes = wanted;
  if (sendBytes > virtualBytes - offset) sendBytes = virtualBytes - offset;

  Serial.printf("VSUSB DATA %lu\n", (unsigned long)sendBytes);
  Serial.flush();

  uint32_t sent = 0;
  uint32_t cursor = offset;
  const uint8_t* headerBytes = reinterpret_cast<const uint8_t*>(&outHeader);

  if (cursor < sizeof(WAVHeader) && sent < sendBytes) {
    size_t n = sizeof(WAVHeader) - cursor;
    if (n > sendBytes - sent) n = sendBytes - sent;
    if (!vsUsbWriteAll(headerBytes + cursor, n)) {
      Serial.printf("\nVSUSB ENDDATA %lu\n", (unsigned long)sent);
      return;
    }
    cursor += n;
    sent += n;
  }

  if (sent < sendBytes) {
    const uint32_t pcmOffset = cursor - sizeof(WAVHeader);
    if ((pcmOffset & 1U) != 0 || ((sendBytes - sent) & 1U) != 0) {
      Serial.printf("\nVSUSB ENDDATA %lu\n", (unsigned long)sent);
      Serial.flush();
      return;
    }

    const uint32_t outputSampleStart = pcmOffset / sizeof(int16_t);
    const uint64_t sourceByteOffset64 =
        static_cast<uint64_t>(payloadStart) +
        static_cast<uint64_t>(outputSampleStart) * groupBytes;
    if (sourceByteOffset64 > UINT32_MAX) {
      Serial.printf("\nVSUSB ENDDATA %lu\n", (unsigned long)sent);
      Serial.flush();
      return;
    }

    File wav = SD.open(path, FILE_READ);
    if (!wav || !wav.seek(static_cast<uint32_t>(sourceByteOffset64))) {
      if (wav) wav.close();
      Serial.printf("\nVSUSB ENDDATA %lu\n", (unsigned long)sent);
      Serial.flush();
      return;
    }

    const uint32_t ratio = groupBytes /
        (outHeader.numChannels == 0 ? sizeof(int16_t)
                                    : sizeof(int16_t) * outHeader.numChannels);
    (void)ratio; // source shape is derived below from the actual source header.

    WAVHeader sourceHeader;
    wav.seek(0);
    if (!vsReadWavHeader(wav, sourceHeader)) {
      wav.close();
      Serial.printf("\nVSUSB ENDDATA %lu\n", (unsigned long)sent);
      Serial.flush();
      return;
    }
    uint32_t sourceRatio = 0, sourceGroupBytes = 0;
    if (!vsSpeechSourceShape(sourceHeader, sourceRatio, sourceGroupBytes) ||
        sourceGroupBytes != groupBytes ||
        !wav.seek(static_cast<uint32_t>(sourceByteOffset64))) {
      wav.close();
      Serial.printf("\nVSUSB ENDDATA %lu\n", (unsigned long)sent);
      Serial.flush();
      return;
    }

    const size_t samplesPerOutput =
        static_cast<size_t>(sourceHeader.numChannels) * sourceRatio;
    const size_t maxSourceSamples = VS067_SCRATCH_BYTES / sizeof(int16_t);
    const size_t maxOutputGroups = maxSourceSamples / samplesPerOutput;

    uint32_t outputBytesRemaining = sendBytes - sent;
    while (outputBytesRemaining > 0) {
      size_t groups = outputBytesRemaining / sizeof(int16_t);
      if (groups > maxOutputGroups) groups = maxOutputGroups;
      if (groups == 0) break;

      const size_t sourceSamples = groups * samplesPerOutput;
      const size_t readBytes = sourceSamples * sizeof(int16_t);

      size_t totalRead = 0;
      while (totalRead < readBytes) {
        const size_t got = wav.read(vs067Scratch + totalRead, readBytes - totalRead);
        if (got == 0) break;
        totalRead += got;
      }
      if (totalRead != readBytes) break;

      int16_t* samples = reinterpret_cast<int16_t*>(vs067Scratch);
      if (sourceHeader.numChannels == 2 && sourceRatio == 3) {
        for (size_t g = 0; g < groups; ++g) {
          const size_t b = g * 6;
          const int32_t sum =
              static_cast<int32_t>(samples[b]) + samples[b + 1] + samples[b + 2] +
              samples[b + 3] + samples[b + 4] + samples[b + 5];
          samples[g] = static_cast<int16_t>(sum / 6);
        }
      } else {
        for (size_t g = 0; g < groups; ++g) {
          int32_t sum = 0;
          const size_t base = g * samplesPerOutput;
          for (size_t s = 0; s < samplesPerOutput; ++s) sum += samples[base + s];
          samples[g] =
              static_cast<int16_t>(sum / static_cast<int32_t>(samplesPerOutput));
        }
      }

      const size_t outBytes = groups * sizeof(int16_t);
      if (!vsUsbWriteAll(reinterpret_cast<uint8_t*>(samples), outBytes)) break;

      sent += outBytes;
      outputBytesRemaining -= outBytes;
    }
    wav.close();
  }

  Serial.flush();
  Serial.printf("\nVSUSB ENDDATA %lu\n", (unsigned long)sent);
  Serial.flush();
}

static void vsUsbReplySessionKey(const String& uuid) {
  uint8_t key[32];
  if (!vsSessionKey(uuid, key)) {
    Serial.println("VSUSB ERROR KEY");
    return;
  }
  const String b64 = vsBase64(key, sizeof(key));
  memset(key, 0, sizeof(key));
  Serial.printf("VSUSB KEY %s %s\n", uuid.c_str(), b64.c_str());
}

static void vsUsbReadFile(const String& path, uint32_t offset, uint32_t wanted) {
  static constexpr uint32_t MAX_READ = 256U * 1024U;
  if (!vsUsbSafePath(path) || wanted == 0 || wanted > MAX_READ) {
    Serial.println("VSUSB ERROR READ_ARGS");
    return;
  }

  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.println("VSUSB ERROR READ_OPEN");
    return;
  }
  const uint32_t fileSize = static_cast<uint32_t>(f.size());
  if (offset > fileSize || !f.seek(offset)) {
    f.close();
    Serial.println("VSUSB ERROR READ_SEEK");
    return;
  }

  uint32_t sendBytes = wanted;
  if (sendBytes > fileSize - offset) sendBytes = fileSize - offset;
  if (!vs067EnsureScratch()) {
    f.close();
    Serial.println("VSUSB ERROR READ_BUFFER");
    return;
  }

  Serial.printf("VSUSB DATA %lu\n", (unsigned long)sendBytes);
  Serial.flush();

  uint32_t sent = 0;
  while (sent < sendBytes) {
    size_t n = sendBytes - sent;
    if (n > 64U * 1024U) n = 64U * 1024U;
    const size_t got = f.read(vs067Scratch, n);
    if (got == 0) break;
    size_t written = 0;
    while (written < got) {
      const size_t nwrite = Serial.write(vs067Scratch + written, got - written);
      if (nwrite == 0) {
        delay(1);
        continue;
      }
      written += nwrite;
    }
    sent += got;
  }
  f.close();
  Serial.flush();
  Serial.printf("\nVSUSB ENDDATA %lu\n", (unsigned long)sent);
  Serial.flush();
}

static void (*vsUsbTestHook)(const String& line) = nullptr;   // crash_trail.h

static void vsUsbHandleCommand(String line) {
  line.trim();
  if (vsUsbTestHook && line.startsWith("VSTEST ")) { vsUsbTestHook(line); return; }
  if (!line.startsWith("VSUSB ")) return;

  if (line == "VSUSB HELLO") {
    Serial.println("VSUSB READY 1");
    return;
  }

  if (line == "VSUSB ENTER") {
    if (captureRunning || state == AppState::RECORDING || state == AppState::PAUSED) {
      Serial.println("VSUSB BUSY RECORDING");
      return;
    }
    vsUsbSyncActive = true;

    // Do not tear down Wi-Fi here. USB sync does not use the ESP32 network
    // stack, and changing Wi-Fi mode during the USB handover adds an unrelated
    // subsystem transition exactly when the PC expects a stable CDC channel.
    noteActivity();
    vsUsbDraw("PC VERBONDEN", "USB protocol v1");

    const String tokenB64 = vsBase64(
        reinterpret_cast<const uint8_t*>(VISITESCRIBE_DEVICE_TOKEN),
        strlen(VISITESCRIBE_DEVICE_TOKEN));
    Serial.printf("VSUSB OK ENTER %s %s %s\n",
                  VISITESCRIBE_DEVICE_ID,
                  VISITESCRIBE_SERVER_BASE_URL,
                  tokenB64.c_str());
    Serial.flush();
    return;
  }

  if (!vsUsbSyncActive) {
    Serial.println("VSUSB ERROR NOT_ENTERED");
    return;
  }

#ifdef VISITESCRIBE_DIRECT_OPUS
  if (line.startsWith("VSUSB RECOVER ")) {
    const String prefix = line.substring(14);
    const bool ok = vsDoRecoverSession(prefix);
    Serial.printf("VSUSB RECOVER %s %s\n", prefix.c_str(), ok ? "OK" : "FAILED");
    return;
  }
#endif
  if (line == "VSUSB POWERPROBE") {
    Serial.println(vsStartPowerProbe() ? "VSUSB OK POWERPROBE" : "VSUSB ERROR POWERPROBE");
    return;
  }
  if (line == "VSUSB INFO") {
    vsUsbReplyInfo();
    return;
  }
  if (line == "VSUSB LIST") {
    vsUsbReplyList();
    return;
  }
  if (line == "VSUSB EXIT") {
    Serial.println("VSUSB OK EXIT");
    Serial.flush();
    vsUsbSyncActive = false;
    goHome();
    return;
  }

  if (line.startsWith("VSUSB KEY ")) {
    String uuid = line.substring(strlen("VSUSB KEY "));
    uuid.trim();
    if (uuid.length() != 36) {
      Serial.println("VSUSB ERROR KEY_ARGS");
      return;
    }
    vsUsbReplySessionKey(uuid);
    return;
  }

  if (line.startsWith("VSUSB MARK ")) {
    String rest = line.substring(strlen("VSUSB MARK "));
    const int sep = rest.indexOf(' ');
    if (sep <= 0) {
      Serial.println("VSUSB ERROR MARK_ARGS");
      return;
    }
    const String prefix = rest.substring(0, sep);
    String uuid = rest.substring(sep + 1);
    uuid.trim();
    if (prefix.length() != 6 || uuid.length() != 36 ||
        !vsWriteSyncMeta(prefix, uuid, "ingested")) {
      Serial.println("VSUSB ERROR MARK");
      return;
    }
    Serial.printf("VSUSB OK MARK %s\n", prefix.c_str());
    return;
  }

  if (line.startsWith("VSUSB READSPEECH ")) {
    String rest = line.substring(strlen("VSUSB READSPEECH "));
    const int s1 = rest.indexOf(' ');
    const int s2 = s1 >= 0 ? rest.indexOf(' ', s1 + 1) : -1;
    if (s1 <= 0 || s2 <= s1) {
      Serial.println("VSUSB ERROR READSPEECH_ARGS");
      return;
    }
    const String path = rest.substring(0, s1);
    const uint32_t offset = static_cast<uint32_t>(
        strtoul(rest.substring(s1 + 1, s2).c_str(), nullptr, 10));
    const uint32_t length = static_cast<uint32_t>(
        strtoul(rest.substring(s2 + 1).c_str(), nullptr, 10));
    noteActivity();
    vsUsbReadSpeech(path, offset, length);
    noteActivity();
    return;
  }

  if (line.startsWith("VSUSB READ ")) {
    String rest = line.substring(strlen("VSUSB READ "));
    const int s1 = rest.indexOf(' ');
    const int s2 = s1 >= 0 ? rest.indexOf(' ', s1 + 1) : -1;
    if (s1 <= 0 || s2 <= s1) {
      Serial.println("VSUSB ERROR READ_ARGS");
      return;
    }
    const String path = rest.substring(0, s1);
    const uint32_t offset = static_cast<uint32_t>(strtoul(rest.substring(s1 + 1, s2).c_str(), nullptr, 10));
    const uint32_t length = static_cast<uint32_t>(strtoul(rest.substring(s2 + 1).c_str(), nullptr, 10));
    noteActivity();
    vsUsbReadFile(path, offset, length);
    noteActivity();
    return;
  }

  Serial.println("VSUSB ERROR UNKNOWN");
}

static bool vsUsbSyncService() {
  vsUsbHotplugService();

  while (Serial.available()) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') continue;
    if (ch == '\n') {
      if (vsUsbRxLine.length()) {
        const String line = vsUsbRxLine;
        vsUsbRxLine = "";
        vsUsbHandleCommand(line);
      }
    } else if (vsUsbRxLine.length() < 255) {
      vsUsbRxLine += ch;
    } else {
      vsUsbRxLine = "";
    }
  }
  return vsUsbSyncActive;
}

struct Vs067PrepTiming {
  uint32_t readMs = 0;
  uint32_t mixMs = 0;
  uint32_t shaMs = 0;
  uint32_t aesMs = 0;
};

static bool vs067EnsureScratch() {
  if (vs067Scratch) return true;
  vs067Scratch = static_cast<uint8_t*>(ps_malloc(VS067_SCRATCH_BYTES));
  if (!vs067Scratch) {
    Serial.printf("SERVER: v0.6.7 scratch allocation FAILED bytes=%u psram_free=%u\n",
                  (unsigned)VS067_SCRATCH_BYTES,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return false;
  }
  Serial.printf("SERVER: v0.6.7 scratch=%u bytes PSRAM free=%u\n",
                (unsigned)VS067_SCRATCH_BYTES,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  return true;
}

static bool vs067ReadExactLarge(File& f, uint8_t* dst, size_t len, uint32_t& readMs) {
  size_t total = 0;
  while (total < len) {
    const uint32_t started = millis();
    const size_t got = f.read(dst + total, len - total);
    readMs += millis() - started;
    if (got == 0) return false;
    total += got;
  }
  return true;
}

static bool vs067EncryptPreparedChunk(const uint8_t sessionKey[32], const String& uuid,
                                      uint32_t sequence, size_t plaintextLen,
                                      bool legacy, VsChunkMeta& meta,
                                      Vs067PrepTiming& timing) {
  uint8_t nonce[VS_GCM_NONCE_BYTES];
  if (!vsChunkNonceFlavor(sessionKey, uuid, sequence, legacy, nonce)) return false;
  if (!vsFillCryptoMeta(sessionKey, uuid, sequence, legacy, meta)) return false;
  meta.plaintextBytes = plaintextLen;

  uint32_t started = millis();
  meta.plaintextSha256 = vsSha256Hex(vsPlain, plaintextLen);
  timing.shaMs += millis() - started;
  if (!meta.plaintextSha256.length()) return false;

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, sessionKey, 256);
  uint8_t tag[VS_GCM_TAG_BYTES];
  started = millis();
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(
        &gcm, MBEDTLS_GCM_ENCRYPT, plaintextLen,
        nonce, sizeof(nonce),
        reinterpret_cast<const unsigned char*>(meta.aad.c_str()), meta.aad.length(),
        vsPlain, vsCipher, sizeof(tag), tag);
  }
  timing.aesMs += millis() - started;
  mbedtls_gcm_free(&gcm);
  if (rc != 0) return false;

  memcpy(vsCipher + plaintextLen, tag, sizeof(tag));
  meta.ciphertextBytes = plaintextLen + sizeof(tag);
  started = millis();
  meta.ciphertextSha256 = vsSha256Hex(vsCipher, meta.ciphertextBytes);
  timing.shaMs += millis() - started;
  return meta.ciphertextSha256.length() == 64;
}

static bool vs067PrepareSpeechChunk(File& wav, const WAVHeader& sourceHeader,
                                    uint32_t& remaining, const uint8_t sessionKey[32],
                                    const String& uuid, uint32_t sequence,
                                    VsChunkMeta& meta, Vs067PrepTiming& timing) {
  if (!vs067EnsureScratch()) return false;

  uint32_t ratio = 0, groupBytes = 0;
  if (!vsSpeechSourceShape(sourceHeader, ratio, groupBytes)) return false;
  const uint32_t sourceBytes = vsSpeechSourceBytes(sourceHeader, remaining);
  if (sourceBytes == 0) return false;

  const uint32_t outputSamples = sourceBytes / groupBytes;
  const uint32_t outputDataBytes = outputSamples * sizeof(int16_t);
  const size_t plainLen = sizeof(WAVHeader) + outputDataBytes;
  if (!vsEnsureChunkBuffers(plainLen)) return false;

  WAVHeader outHeader = sourceHeader;
  outHeader.audioFormat = 1;
  outHeader.numChannels = VS_SPEECH_CHANNELS;
  outHeader.sampleRate = VS_SPEECH_RATE;
  outHeader.bitsPerSample = VS_SPEECH_BITS;
  outHeader.blockAlign = VS_SPEECH_CHANNELS * (VS_SPEECH_BITS / 8);
  outHeader.byteRate = VS_SPEECH_RATE * outHeader.blockAlign;
  outHeader.dataSize = outputDataBytes;
  outHeader.fileSize = 36 + outputDataBytes;
  memcpy(vsPlain, &outHeader, sizeof(outHeader));

  int16_t* dst = reinterpret_cast<int16_t*>(vsPlain + sizeof(WAVHeader));
  const size_t samplesPerOutput = static_cast<size_t>(sourceHeader.numChannels) * ratio;
  if (samplesPerOutput == 0) return false;
  const size_t maxSourceSamples = VS067_SCRATCH_BYTES / sizeof(int16_t);
  const size_t maxOutputPerRead = maxSourceSamples / samplesPerOutput;
  if (maxOutputPerRead == 0) return false;

  uint32_t produced = 0;
  while (produced < outputSamples) {
    size_t groups = outputSamples - produced;
    if (groups > maxOutputPerRead) groups = maxOutputPerRead;
    const size_t sourceSamples = groups * samplesPerOutput;
    const size_t readBytes = sourceSamples * sizeof(int16_t);
    if (!vs067ReadExactLarge(wav, vs067Scratch, readBytes, timing.readMs)) return false;

    const int16_t* src = reinterpret_cast<const int16_t*>(vs067Scratch);
    const uint32_t mixStarted = millis();

    // The production recorder is 48 kHz stereo, so the hot path is six source
    // samples (L/R over three source frames) per 16 kHz mono output sample.
    if (sourceHeader.numChannels == 2 && ratio == 3) {
      for (size_t g = 0; g < groups; ++g) {
        const size_t b = g * 6;
        const int32_t sum = static_cast<int32_t>(src[b]) + src[b + 1] + src[b + 2] +
                            src[b + 3] + src[b + 4] + src[b + 5];
        dst[produced + g] = static_cast<int16_t>(sum / 6);
      }
    } else {
      for (size_t g = 0; g < groups; ++g) {
        int32_t sum = 0;
        const size_t base = g * samplesPerOutput;
        for (size_t s = 0; s < samplesPerOutput; ++s) sum += src[base + s];
        dst[produced + g] = static_cast<int16_t>(sum / static_cast<int32_t>(samplesPerOutput));
      }
    }
    timing.mixMs += millis() - mixStarted;
    produced += groups;
  }

  remaining -= sourceBytes;
  if (!vs067EncryptPreparedChunk(sessionKey, uuid, sequence, plainLen, false, meta, timing)) {
    return false;
  }
  meta.durationMs = static_cast<uint32_t>(
      (static_cast<uint64_t>(outputSamples) * 1000ULL) / VS_SPEECH_RATE);
  return true;
}

static bool vs067PrepareLegacyChunk(File& wav, const WAVHeader& h, uint32_t& remaining,
                                    const uint8_t sessionKey[32], const String& uuid,
                                    uint32_t sequence, VsChunkMeta& meta,
                                    Vs067PrepTiming& timing) {
  const uint32_t dataBytes = vsLegacySourceBytes(h, remaining);
  if (dataBytes == 0) return false;
  const size_t plainLen = sizeof(WAVHeader) + dataBytes;
  if (!vsEnsureChunkBuffers(plainLen)) return false;

  WAVHeader chunkHeader = h;
  chunkHeader.fileSize = 36 + dataBytes;
  chunkHeader.dataSize = dataBytes;
  memcpy(vsPlain, &chunkHeader, sizeof(chunkHeader));
  if (!vs067ReadExactLarge(wav, vsPlain + sizeof(WAVHeader), dataBytes, timing.readMs)) {
    return false;
  }
  remaining -= dataBytes;
  if (!vs067EncryptPreparedChunk(sessionKey, uuid, sequence, plainLen, true, meta, timing)) {
    return false;
  }
  meta.durationMs = static_cast<uint32_t>(
      (static_cast<uint64_t>(dataBytes) * 1000ULL) / h.byteRate);
  return true;
}

static bool vs067PutChunk(HTTPClient& http, bool& httpStarted,
                          const VsLocalSession& session, const VsChunkMeta& meta,
                          bool& reusedBefore, bool& keptAliveAfter) {
  const String uri = String("/v1/sessions/") + session.uuid +
                     "/chunks/" + String(meta.sequence);
  reusedBefore = vsTls.connected();

  if (!httpStarted) {
    const String url = String(VISITESCRIBE_SERVER_BASE_URL) + uri;
    if (!vsBeginHttp(http, url)) return vsFail("HTTPS chunk start mislukt");
    httpStarted = true;
  } else {
    if (!http.setURL(uri)) return vsFail("HTTPS chunk URL wisselen mislukt");
    http.setTimeout(vsHttpTimeoutMs());
    http.setReuse(true);
    vsAddCommonHeaders(http);
  }

  http.addHeader("Content-Type", "application/octet-stream", false, true);
  http.addHeader("X-Chunk-SHA256", meta.ciphertextSha256, false, true);
  http.addHeader("X-Chunk-Nonce", meta.nonceB64, false, true);
  http.addHeader("X-Chunk-AAD", meta.aad, false, true);
  http.addHeader("X-Plaintext-SHA256", meta.plaintextSha256, false, true);
  http.addHeader("Idempotency-Key", session.uuid + ":chunk:" + String(meta.sequence), false, true);

  const int code = http.sendRequest("PUT", vsCipher, meta.ciphertextBytes);
  // Always consume the response body. HTTPClient can only safely retain the
  // connection when no bytes from the previous response remain unread.
  String body = code >= 0 ? http.getString() : String();
  http.end();
  keptAliveAfter = vsTls.connected();

  if (code < 200 || code >= 300) {
    return vsFail(code < 0 ? String("HTTPS chunk fout ") + code
                           : vsHttpErrorBody(body, code));
  }
  return true;
}

static bool vs067UploadSpeechChunks(const VsLocalSession& session,
                                    const uint8_t sessionKey[32],
                                    uint32_t expectedChunks,
                                    const std::vector<uint32_t>* missing) {
  vsServerStage = VsServerStage::UPLOAD_CHUNKS;
  vsServerChunkTotal = expectedChunks;
  vsServerChunkCurrent = 0;
  const uint32_t uploadStarted = millis();
  uint64_t uploadedBytes = 0;
  uint32_t uploadedNow = 0;
  uint32_t skipped = 0;
  uint32_t sequence = 0;
  HTTPClient chunkHttp;
  bool httpStarted = false;

  for (const auto& path : session.wavs) {
    File wav = SD.open(path, FILE_READ);
    WAVHeader h;
    if (!wav || !vsReadWavHeader(wav, h)) {
      if (wav) wav.close();
      return vsFail(String("WAV upload lezen mislukt: ") + path);
    }
    uint32_t remaining = h.dataSize;
    while (remaining) {
      if (vsSyncAborted()) { wav.close(); return vsFail("Gestopt: knop ingedrukt"); }
      const uint32_t sourceBytes = vsSpeechSourceBytes(h, remaining);
      if (sourceBytes == 0) { remaining = 0; break; }
      ++sequence;
      vsServerChunkCurrent = sequence;
      if (!vsNeedSequence(sequence, missing)) {
        if (!vsSkipBytes(wav, sourceBytes)) {
          wav.close();
          return vsFail("Resume seek mislukt");
        }
        remaining -= sourceBytes;
        ++skipped;
        continue;
      }

      VsChunkMeta meta;
      Vs067PrepTiming timing;
      const uint32_t prepStarted = millis();
      if (!vs067PrepareSpeechChunk(
              wav, h, remaining, sessionKey, session.uuid, sequence, meta, timing)) {
        wav.close();
        return vsFail("16k mono chunk voorbereiden mislukt");
      }
      const uint32_t prepMs = millis() - prepStarted;

      vsServerMessage = String("Upload ") + sequence;
      vsDrawServerSync(true);
      bool reused = false, keptAlive = false;
      const uint32_t httpCallStarted = millis();
      if (!vs067PutChunk(chunkHttp, httpStarted, session, meta, reused, keptAlive)) {
        wav.close();
        return false;
      }
      const uint32_t httpMs = millis() - httpCallStarted;

      uploadedBytes += meta.ciphertextBytes;
      ++uploadedNow;
      const uint32_t kibPerSec = httpMs
          ? static_cast<uint32_t>((static_cast<uint64_t>(meta.ciphertextBytes) * 1000ULL) /
                                  (1024ULL * httpMs))
          : 0;
      Serial.printf(
          "SERVER: speech chunk %lu/%lu prep=%lums [read=%lu mix=%lu sha=%lu aes=%lu] "
          "http=%lums rate=%lu KiB/s tls_reused=%d keepalive=%d bytes=%u\n",
          (unsigned long)sequence, (unsigned long)expectedChunks,
          (unsigned long)prepMs, (unsigned long)timing.readMs,
          (unsigned long)timing.mixMs, (unsigned long)timing.shaMs,
          (unsigned long)timing.aesMs, (unsigned long)httpMs,
          (unsigned long)kibPerSec, reused ? 1 : 0, keptAlive ? 1 : 0,
          (unsigned)meta.ciphertextBytes);
      delay(1);
    }
    wav.close();
  }

  chunkHttp.end();
  const uint32_t totalMs = millis() - uploadStarted;
  const uint32_t avgKibPerSec = totalMs
      ? static_cast<uint32_t>((uploadedBytes * 1000ULL) / (1024ULL * totalMs))
      : 0;
  Serial.printf(
      "SERVER: speech upload done expected=%lu uploaded_now=%lu skipped=%lu bytes=%llu "
      "total=%lums avg=%lu KiB/s\n",
      (unsigned long)expectedChunks, (unsigned long)uploadedNow,
      (unsigned long)skipped, (unsigned long long)uploadedBytes,
      (unsigned long)totalMs, (unsigned long)avgKibPerSec);
  return sequence == expectedChunks;
}

static bool vs067UploadLegacyChunks(const VsLocalSession& session,
                                    const uint8_t sessionKey[32],
                                    uint32_t expectedChunks,
                                    const std::vector<uint32_t>* missing) {
  vsServerStage = VsServerStage::UPLOAD_CHUNKS;
  vsServerChunkTotal = expectedChunks;
  vsServerChunkCurrent = 0;
  uint32_t sequence = 0;
  uint32_t uploadedNow = 0;
  uint32_t skipped = 0;
  HTTPClient chunkHttp;
  bool httpStarted = false;
  Serial.println("SERVER: v0.6.7 legacy resume; large SD reads + persistent HTTP/TLS");

  for (const auto& path : session.wavs) {
    File wav = SD.open(path, FILE_READ);
    WAVHeader h;
    if (!wav || !vsReadWavHeader(wav, h)) {
      if (wav) wav.close();
      return vsFail(String("Legacy WAV lezen mislukt: ") + path);
    }
    uint32_t remaining = h.dataSize;
    while (remaining) {
      if (vsSyncAborted()) { wav.close(); return vsFail("Gestopt: knop ingedrukt"); }
      const uint32_t sourceBytes = vsLegacySourceBytes(h, remaining);
      if (sourceBytes == 0) { remaining = 0; break; }
      ++sequence;
      vsServerChunkCurrent = sequence;
      if (!vsNeedSequence(sequence, missing)) {
        if (!vsSkipBytes(wav, sourceBytes)) {
          wav.close();
          return vsFail("Legacy resume seek mislukt");
        }
        remaining -= sourceBytes;
        ++skipped;
        continue;
      }

      VsChunkMeta meta;
      Vs067PrepTiming timing;
      const uint32_t prepStarted = millis();
      if (!vs067PrepareLegacyChunk(
              wav, h, remaining, sessionKey, session.uuid, sequence, meta, timing)) {
        wav.close();
        return vsFail("Legacy chunk voorbereiden mislukt");
      }
      const uint32_t prepMs = millis() - prepStarted;
      vsServerMessage = String("Legacy ") + sequence;
      vsDrawServerSync(true);

      bool reused = false, keptAlive = false;
      const uint32_t httpCallStarted = millis();
      if (!vs067PutChunk(chunkHttp, httpStarted, session, meta, reused, keptAlive)) {
        wav.close();
        return false;
      }
      const uint32_t httpMs = millis() - httpCallStarted;
      ++uploadedNow;
      Serial.printf(
          "SERVER: legacy chunk %lu/%lu prep=%lums [read=%lu sha=%lu aes=%lu] "
          "http=%lums tls_reused=%d keepalive=%d\n",
          (unsigned long)sequence, (unsigned long)expectedChunks,
          (unsigned long)prepMs, (unsigned long)timing.readMs,
          (unsigned long)timing.shaMs, (unsigned long)timing.aesMs,
          (unsigned long)httpMs, reused ? 1 : 0, keptAlive ? 1 : 0);
    }
    wav.close();
  }

  chunkHttp.end();
  Serial.printf("SERVER: v0.6.7 legacy resume done uploaded_now=%lu skipped=%lu\n",
                (unsigned long)uploadedNow, (unsigned long)skipped);
  return sequence == expectedChunks;
}

static bool vs067SyncOne(const VsLocalSession& session,
                         const String& serverKeyId, const String& serverPublicPem) {
#ifdef VISITESCRIBE_DIRECT_OPUS
  if (session.directOpus) {
    Serial.printf(
        "SERVER: %s uses recorder-native direct Ogg/Opus (%u chunks)\n",
        session.prefix.c_str(), (unsigned)session.opus.size());
    return vsSyncDirectOpus(session, serverKeyId, serverPublicPem);
  }
#endif

  const uint32_t sessionStarted = millis();
  vsServerSessionPrefix = session.prefix;
  vsServerChunkCurrent = vsServerChunkTotal = 0;
  vsSetStage(VsServerStage::PREPARE, session.prefix);

  uint8_t sessionKey[32];
  if (!vsSessionKey(session.uuid, sessionKey)) return vsFail("Sessiesleutel maken mislukt");
  String wrappedKey;
  if (!vsWrapSessionKey(sessionKey, serverPublicPem, wrappedKey)) {
    memset(sessionKey, 0, sizeof(sessionKey));
    return vsFail("RSA key wrap mislukt");
  }

  String manifestPath = String("/visitescribe/") + session.prefix + "_upload_manifest.json";
  if (SD.exists(manifestPath)) SD.remove(manifestPath);
  uint32_t speechCount = 0;
  if (!vsWriteSpeechManifest(
          session, sessionKey, serverKeyId, wrappedKey, manifestPath, speechCount)) {
    SD.remove(manifestPath);
    memset(sessionKey, 0, sizeof(sessionKey));
    return false;
  }
  vsServerChunkTotal = speechCount;

  VsManifestPost post = vsPostManifest(session, manifestPath);
  SD.remove(manifestPath);
  if (post == VsManifestPost::FAILED) {
    memset(sessionKey, 0, sizeof(sessionKey));
    return false;
  }

  VsRemoteStatus remote;
  const bool haveRemote = vsFetchRemoteStatus(
      session, remote, post == VsManifestPost::CONFLICT);
  if (post == VsManifestPost::CONFLICT && !haveRemote) {
    memset(sessionKey, 0, sizeof(sessionKey));
    return false;
  }

  if (haveRemote && remote.confirmed) {
    memset(sessionKey, 0, sizeof(sessionKey));
    return vsFinishLocalSession(session, sessionStarted);
  }

  bool legacyResume = false;
  uint32_t activeCount = speechCount;
  const std::vector<uint32_t>* missing = nullptr;

  if (haveRemote && remote.valid) {
    if (remote.expectedChunks == speechCount) {
      missing = &remote.missing;
      Serial.printf("SERVER: resume speech session; %u/%lu chunks nog nodig\n",
                    (unsigned)remote.missing.size(), (unsigned long)speechCount);
    } else if (post == VsManifestPost::CONFLICT) {
      const uint32_t legacyCount = vsCountChunks(session, true);
      if (legacyCount > 0 && remote.expectedChunks == legacyCount) {
        legacyResume = true;
        activeCount = legacyCount;
        missing = &remote.missing;
        Serial.printf(
            "SERVER: oude sessie detected expected=%lu; legacy resume missing=%u\n",
            (unsigned long)legacyCount, (unsigned)remote.missing.size());
      } else {
        memset(sessionKey, 0, sizeof(sessionKey));
        return vsFail(String("Bestaande sessie heeft ") + remote.expectedChunks +
                      " chunks; lokaal speech=" + speechCount + " legacy=" + legacyCount);
      }
    } else {
      memset(sessionKey, 0, sizeof(sessionKey));
      return vsFail("Server chunk-aantal wijkt af na create");
    }
  }

  vsServerChunkTotal = activeCount;
  bool ok = legacyResume
      ? vs067UploadLegacyChunks(session, sessionKey, activeCount, missing)
      : vs067UploadSpeechChunks(session, sessionKey, activeCount, missing);

  if (ok) ok = vsPostEvents(session);
  if (ok) ok = vsComplete(session, activeCount);
  if (ok) ok = vsConfirm(session);
  memset(sessionKey, 0, sizeof(sessionKey));
  if (!ok) return false;
  return vsFinishLocalSession(session, sessionStarted);
}

static bool vs067SyncAllPending() {
  if (WiFi.status() != WL_CONNECTED) return vsFail("WiFi niet verbonden");
  if (!sdOk) return vsFail("microSD niet beschikbaar");
  if (!vsEnsureDeviceRootKey()) return vsFail("Device root key niet beschikbaar");

  vsServerSessionsTotal = vsServerSessionsDone = vsServerSessionsSkipped = 0;
  vsSetStage(VsServerStage::CLEANUP);
  std::vector<String> directory;
  const uint32_t directoryStarted = millis();
  if (!vsReadSessionDirectory(directory)) return vsFail("Opnamemap lezen mislukt");
  Serial.printf("SERVER: shared directory pass files=%u elapsed=%lums\n",
                (unsigned)directory.size(), (unsigned long)(millis() - directoryStarted));
  // Deleting confirmed recordings changes the directory; the rest of the sync
  // must not work from a listing that still names the deleted files.
  if (vsRetentionCleanup(&directory) && !vsReadSessionDirectory(directory))
    return vsFail("Opnamemap lezen mislukt");
#ifdef VISITESCRIBE_DIRECT_OPUS
  vsDoRecoverActiveSessions(&directory);
#endif
  std::vector<VsLocalSession> preparedSessions;
  const auto prefixes = vsPendingPrefixes(&preparedSessions, &directory);
  Serial.printf("SERVER: pending queue count=%u", (unsigned)prefixes.size());
  for (const auto& prefix : prefixes) {
    Serial.printf(" %s", prefix.c_str());
  }
  Serial.println();
  vsServerSessionsTotal = prefixes.size();
  vsServerSessionsDone = 0;
  vsServerSessionsSkipped = 0;
  if (prefixes.empty()) {
    vsSetStage(VsServerStage::NOTHING, "Alle opnames zijn gesynchroniseerd");
    return true;
  }

  String serverKeyId, serverPublicPem;
  if (!vsFetchServerKey(serverKeyId, serverPublicPem)) return false;
  size_t preparedIndex = 0;
  for (const auto& prefix : prefixes) {
    if (vsSyncAborted()) return vsFail("Gestopt: knop ingedrukt");
    if (WiFi.status() != WL_CONNECTED) return vsFail("WiFi verbinding verloren");

    VsLocalSession local;
    if (!vsLoadLocalSession(prefix, local, &preparedSessions[preparedIndex++])) {
      return vsFail(String("Lokale sessie fout: ") + prefix);
    }

    // A damaged/empty old WAV recording must never block later
    // consultations forever. Direct-Opus sessions already passed their own
    // contiguous metadata check in vsLoadLocalSession().
#ifdef VISITESCRIBE_DIRECT_OPUS
    const bool needsLegacyChunkCheck = !local.directOpus;
#else
    const bool needsLegacyChunkCheck = true;
#endif
    if (needsLegacyChunkCheck) {
      const uint32_t localSpeechChunks = vsCountChunks(local, false);
      if (localSpeechChunks == 0) {
        ++vsServerSessionsSkipped;
        vsServerSessionPrefix = local.prefix;
        vsServerChunkCurrent = vsServerChunkTotal = 0;

        // Preserve every local file, but quarantine this completed zero-audio
        // session so the same historical number no longer appears on every
        // automatic sync. Removing its _sync.txt manually makes it retryable.
        if (!vsWriteSyncMeta(
                local.prefix, local.uuid, "quarantined_no_audio")) {
          return vsFail(
              String("Kon zero-audio sessie niet quarantaine-markeren: ") +
              local.prefix);
        }

        Serial.printf(
            "SERVER: QUARANTINE session %s: no valid 16k speech chunks; "
            "local files retained, automatic retries disabled\n",
            local.prefix.c_str());
        vsSetStage(
            VsServerStage::PREPARE,
            "QUARANTAINE - geen geldige audio");
        continue;
      }
    }

    if (!vs067SyncOne(local, serverKeyId, serverPublicPem)) return false;
  }

  if (vsServerSessionsSkipped) {
    vsSetStage(
        VsServerStage::DONE,
        String(vsServerSessionsDone) + " upload, " +
        String(vsServerSessionsSkipped) + " overgeslagen");
  } else {
    vsSetStage(VsServerStage::DONE,
               String(vsServerSessionsDone) + " sessie(s) geupload");
  }
  return true;
}

static void vs067ServiceServerSync() {
  static VsSyncResultTimer resultTimer;
  if (resultTimer.update(state == AppState::SYNC && !vsServerSyncRunning &&
                         vsServerResultDone(), millis())) {
    wifiOff();
    syncPhase = SyncPhase::NOT_STARTED;
    vsServerStage = VsServerStage::IDLE;
    vsServerSessionPrefix = "";
    goHome();
    pwrClickPendingV03 = false;
    pwrFirstClickMsV03 = 0;
    Serial.println("SERVER: successful result auto-home after 10s; WiFi off");
    return;
  }
  if (state != AppState::SYNC) {
    if (!vsServerSyncRunning) {
      vsServerStage = VsServerStage::IDLE;
      vsServerSessionPrefix = "";
    }
    return;
  }
  if (syncPhase != SyncPhase::CONNECTED) {
    if (!vsServerSyncRunning &&
        (vsServerStage == VsServerStage::DONE ||
         vsServerStage == VsServerStage::NOTHING ||
         vsServerStage == VsServerStage::ERROR)) {
      vsServerStage = VsServerStage::IDLE;
      vsServerSessionPrefix = "";
      vsServerError = "";
    }
    return;
  }
  if (vsServerStage != VsServerStage::IDLE || vsServerSyncRunning) return;

  vsServerSyncRunning = true;
  noteActivity();
  WiFi.setSleep(false);
  delay(20);
  Serial.println("SERVER: starting v0.6.7 sync; WiFi power-save OFF; persistent HTTP/TLS");
  vsMarkerSet("sync", "server sync");
  if (!vsPreUploadHook || vsPreUploadHook()) vs067SyncAllPending();
  vsReleaseChunkBuffers();
  // Successful sync = delete: every recording the server confirmed goes now,
  // not 72 hours later. Before the post hook, which may reboot into an update.
  const uint32_t purged = vsRetentionCleanup();
  if (purged) Serial.printf("PURGE: %lu confirmed recording(s) deleted after sync\n",
                            (unsigned long)purged);
  vsMarkerClear("sync");
  if (vsPostUploadHook) vsPostUploadHook();
  WiFi.setSleep(true);
  Serial.println("SERVER: v0.6.7 sync ended; WiFi power-save ON");
  vsServerSyncRunning = false;
  noteActivity(); // Give DONE/ERROR a fresh readable interval after a long transfer.
  // The final DONE/ERROR frame may have been drawn while the busy lock was
  // still true. Redraw once unlocked so the user sees the real result actions.
  vsDrawServerSync(true);
}

#include "charge_autosync.h"

#ifdef VISITESCRIBE_DIRECT_OPUS
static uint32_t vsRecoveryUiStarted = 0, vsRecoveryUiPainted = 0;
static const char* vsRecoveryUiPhase = "Opslag controleren";
static void vsRecoveryUiPoll() {
  // AXP latches PWR edges while setup blocks. Consume them here, never replay.
  if (axp2101DirectOk && (M5.Power.Axp2101.getPekPress() & 0x02)) noteActivity();
  serviceDisplayPower();
  const uint32_t now = millis();
  if (now - vsRecoveryUiPainted < 250) return;
  vsRecoveryUiPainted = now;
#if VS_STICK
  M5.Display.fillRect(0, 130, SCREEN_W, 60, C_WHITE);
  stickWrapped(134, vsRecoveryUiPhase, C_GREY, 1, 2);
  char elapsed[40]; snprintf(elapsed, sizeof(elapsed), "%lu seconden", (unsigned long)((now-vsRecoveryUiStarted)/1000));
  centeredText(176, elapsed, C_GREY, 1);
#else
  M5.Display.fillRect(0, 118, SCREEN_W, 64, C_WHITE);
  centeredText(130, vsRecoveryUiPhase, C_GREY, 1);
  char elapsed[40]; snprintf(elapsed, sizeof(elapsed), "%lu seconden", (unsigned long)((now-vsRecoveryUiStarted)/1000));
  centeredText(160, elapsed, C_GREY, 1);
#endif
}
static void vsRecoveryUiProgress(const char* phase) {
  vsRecoveryUiPhase = phase;
  vsRecoveryUiPoll();
}
static void vsBootRecover() {
  noteActivity();
  vsRecoveryUiStarted = millis(); vsRecoveryUiPainted = 0;
#if VS_STICK
  drawHeader("");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H-HEADER_H, C_WHITE);
  centeredText(HEADER_H + 11, "Opstarten", C_NAVY, 1);
  centeredText(66, "Opnames", C_NAVY, 2);
  centeredText(88, "controleren", C_NAVY, 2);
  stickWrapped(104, "Even wachten", C_GREY, 1, 1);
#else
  drawHeader("Opstarten");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H-HEADER_H, C_WHITE);
  centeredText(78, "Opnames controleren", C_NAVY, 2);
  centeredText(103, "Herstellen indien nodig - even wachten", C_GREY, 1);
#endif
  vsSessionScanProgressHook = vsRecoveryUiPoll;
  vsDoRecoveryProgressHook = vsRecoveryUiProgress;
  vsDoRecoverActiveSessions();
  vsSessionScanProgressHook = nullptr;
  vsDoRecoveryProgressHook = nullptr;
  if (axp2101DirectOk) M5.Power.Axp2101.getPekPress();
  pwrClickPendingV03 = false; pwrFirstClickMsV03 = 0;
  pwrWakeGuardUntilV03 = millis() + PWR_DOUBLE_CLICK_MS;
  quickTouchArmedV03 = false;
  Serial.printf("RECOVERY: boot check finished in %lums recovered=%lu\n",
      (unsigned long)(millis()-vsRecoveryUiStarted), (unsigned long)vsDoRecoveredOnBoot);
  noteActivity(); screenDirty = true; render(true);
}
#endif

#ifdef VISITESCRIBE_DIRECT_OPUS
// The full "Opnames controleren" pass walks the whole SD card (about 30 s on a
// slow card). Since 0.10.0 it runs only when a marker says an action was cut
// off, plus once after installing this version (older firmware wrote none).
static void vsBootMaybeRecover() {
  Preferences prefs;
  bool scanned = false;
  if (prefs.begin("vsmarker", true)) {
    scanned = prefs.getBool("scan010", false);
    prefs.end();
  }
  if (vsMarkerExists("sync")) {
    Serial.println("MARKER: previous sync was cut off; it resumes at the next sync");
    vsMarkerClear("sync");
    vsPurgeDue = true;
  }
  if (vsMarkerExists("del")) {
    Serial.println("MARKER: deleting confirmed recordings was cut off; finishing it");
    vsPurgeDue = true;
  }
  const bool recording = vsMarkerExists("rec");
  if (!recording && scanned) {
    Serial.println("RECOVERY: no unfinished recording; boot check skipped");
    return;
  }
  Serial.printf("RECOVERY: full boot check (%s)\n",
                recording ? "recording was cut off" : "first start of this version");
  vsBootRecover();
  // Cleared even if a session could not be recovered: some never can (no
  // audio at all) and must not cost 30 s on every boot. Every sync start
  // retries the recovery anyway.
  if (!vsDoRecoveryClean)
    Serial.println("RECOVERY: not everything recovered; retried at the next sync");
  vsMarkerClear("rec");
  if (!scanned && prefs.begin("vsmarker", false)) {
    prefs.putBool("scan010", true);
    prefs.end();
    vsPurgeDue = true;   // clears what 0.9.x kept for 72 hours
  }
}
#endif

void setup() {
  setup_v066_base();
#ifdef VISITESCRIBE_DIRECT_OPUS
  if (sdOk) vsBootMaybeRecover();
#endif
  if (!vs067EnsureScratch()) {
    Serial.println("SERVER: WARNING v0.6.7 large-read scratch unavailable");
  }
  Serial.println("VisiteScribe CoreS3-Lite v0.6.7; SD=512KiB reads; HTTP/TLS=persistent; timing=split");
}

void loop() {
  vsServicePowerProbe();
  // The PC sync app gets exclusive use of USB Serial after an explicit ENTER
  // handshake. While active, do not run normal UI/Wi-Fi/server code so binary
  // file reads cannot be polluted by debug output.
  if (vsUsbSyncService()) {
    vsSuspendChargeAutoSync();
    // In USB maintenance PWR only wakes the screen; never run recorder actions.
    if (axp2101DirectOk && (M5.Power.Axp2101.getPekPress() & 0x02)) noteActivity();
#if VS_STICK
    vsStickPollButtons();
    if (vsStickTakeA() | vsStickTakeB()) noteActivity();
#endif
    serviceDisplayPower();
    delay(1);
    return;
  }

  // Deliberately do not call loop_v066_base(): that would invoke the v0.6.6
  // sync engine as well. Reuse the proven recorder/UI loop and service only the
  // v0.6.7 sync engine here.
  loop_v05();
  vs067ServiceServerSync();
  vsServiceChargeAutoSync();
  // Never walk/delete SD files while recording, USB transfer or sync owns it.
  // Hourly as a backstop, and soon after a USB confirmation or an interrupted
  // delete asked for a pass (at most once a minute, so a card that refuses a
  // delete is not hammered).
  static uint32_t lastRetentionCheck = 0;
  static uint32_t purgeRetryMs = 60000UL;   // grows 1 -> 4 -> 16 -> 60 min on failure
  const uint32_t sinceCheck = millis() - lastRetentionCheck;
  if (state == AppState::HOME && !vsServerSyncRunning && !vsUsbSyncActive &&
      (sinceCheck >= 3600000UL || (vsPurgeDue && sinceCheck >= purgeRetryMs))) {
    lastRetentionCheck = millis();
    vsRetentionCleanup();
    purgeRetryMs = vsPurgeDue ? min<uint32_t>(purgeRetryMs * 4, 3600000UL) : 60000UL;
  }

  // Server-sync screens are redrawn explicitly on real state/progress changes.
  // Do not repaint the complete screen continuously here; that caused visible
  // flicker during long uploads without adding any information.
}
