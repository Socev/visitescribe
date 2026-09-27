// VisiteScribe CoreS3-Lite local LAN transport benchmark.
//
// This is deliberately isolated from real recordings and the production API.
// It connects to a disposable HTTP sink on the user's laptop and streams
// deterministic dummy bytes so we can measure the actual ESP32-S3 Wi-Fi path.
//
// One menu action performs two uploads:
// 1) 10 minutes worth of 16 kHz / mono / PCM16 = 19,200,000 bytes.
// 2) 10 minutes worth of 24 kbit/s Opus-sized data = 1,800,000 bytes.
// The second payload is size-equivalent dummy data, not an Opus encoder test.

#include <WiFi.h>

#ifndef VISITESCRIBE_LAN_BENCH_HOST
#define VISITESCRIBE_LAN_BENCH_HOST "192.168.2.31"
#endif

#ifndef VISITESCRIBE_LAN_BENCH_PORT
#define VISITESCRIBE_LAN_BENCH_PORT 8765
#endif

static constexpr uint32_t VS_LAN_BENCH_AUDIO_SECONDS = 600;
static constexpr uint32_t VS_LAN_PCM_BYTES =
    16000UL * 1UL * 2UL * VS_LAN_BENCH_AUDIO_SECONDS;
static constexpr uint32_t VS_LAN_OPUS_EQ_BYTES =
    (24000UL / 8UL) * VS_LAN_BENCH_AUDIO_SECONDS;
static constexpr size_t VS_LAN_BUFFER_BYTES = 16U * 1024U;

static String vsLanBenchStatus = "laptop 192.168.2.31";
static uint8_t vsLanBenchBuffer[VS_LAN_BUFFER_BYTES];

struct VsLanResult {
  bool ok = false;
  uint32_t bytes = 0;
  uint32_t payloadMs = 0;
  uint32_t totalMs = 0;
  uint32_t kibPerSec = 0;
  uint32_t kbitPerSec = 0;
  String response;
  String error;
};

static const char* vsLanStatusText() {
  return vsLanBenchStatus.c_str();
}

static void vsLanDraw(const char* title, const String& line1,
                      const String& line2 = String(),
                      uint16_t titleColor = C_NAVY) {
  M5.Display.wakeup();
  M5.Display.setBrightness(BRIGHTNESS_ACTIVE);
  displayPower = DisplayPower::ACTIVE;
  drawHeader(title);
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_BG);
  centeredText(90, line1.c_str(), titleColor, 2);
  if (line2.length()) centeredText(128, line2.c_str(), C_GREY, 1);
  centeredText(185, "dummy-data - geen opname", C_GREY, 1);
}

static void vsLanFillBuffer() {
  // Deterministic non-zero PCM-like pattern. Compression is intentionally not
  // involved; the benchmark measures the transport path only.
  int16_t* pcm = reinterpret_cast<int16_t*>(vsLanBenchBuffer);
  const size_t count = VS_LAN_BUFFER_BYTES / sizeof(int16_t);
  int32_t value = -12000;
  int32_t step = 173;
  for (size_t i = 0; i < count; ++i) {
    pcm[i] = static_cast<int16_t>(value);
    value += step;
    if (value > 12000 || value < -12000) {
      step = -step;
      value += step;
    }
  }
}

static bool vsLanWriteAll(WiFiClient& client,
                          const uint8_t* data,
                          size_t len,
                          uint32_t stallTimeoutMs = 10000) {
  size_t done = 0;
  uint32_t lastProgress = millis();
  while (done < len) {
    const size_t n = client.write(data + done, len - done);
    if (n > 0) {
      done += n;
      lastProgress = millis();
      continue;
    }
    if (!client.connected() || millis() - lastProgress > stallTimeoutMs) {
      return false;
    }
    delay(1);
  }
  return true;
}

static String vsLanReadHttpBody(WiFiClient& client, uint32_t timeoutMs) {
  String body;
  const uint32_t started = millis();

  String status = client.readStringUntil('\n');
  status.trim();
  if (!status.startsWith("HTTP/1.1 200") && !status.startsWith("HTTP/1.0 200")) {
    return String("HTTP status: ") + status;
  }

  while (millis() - started < timeoutMs) {
    String line = client.readStringUntil('\n');
    if (!line.length() && !client.connected()) return String();
    line.trim();
    if (line.length() == 0) break;
  }

  uint32_t lastData = millis();
  while (millis() - started < timeoutMs) {
    while (client.available()) {
      body += static_cast<char>(client.read());
      lastData = millis();
    }
    if (!client.connected()) break;
    if (millis() - lastData > 1200) break;
    delay(1);
  }
  body.trim();
  return body;
}

static VsLanResult vsLanUpload(const char* label,
                               uint32_t totalBytes,
                               uint32_t equivalentAudioSeconds) {
  VsLanResult result;
  result.bytes = totalBytes;

  WiFiClient client;
  client.setTimeout(30);

  const uint32_t requestStarted = millis();
  if (!client.connect(VISITESCRIBE_LAN_BENCH_HOST, VISITESCRIBE_LAN_BENCH_PORT)) {
    result.error = String("connect mislukt ") + VISITESCRIBE_LAN_BENCH_HOST +
                   ":" + VISITESCRIBE_LAN_BENCH_PORT;
    return result;
  }

  String header;
  header.reserve(512);
  header += "POST /upload HTTP/1.1\r\n";
  header += "Host: " VISITESCRIBE_LAN_BENCH_HOST "\r\n";
  header += "User-Agent: VisiteScribe-M5-LAN-Bench/1\r\n";
  header += "Connection: close\r\n";
  header += "Content-Type: application/octet-stream\r\n";
  header += "X-VisiteScribe-Label: ";
  header += label;
  header += "\r\n";
  header += "X-Equivalent-Audio-Seconds: ";
  header += String(equivalentAudioSeconds);
  header += "\r\n";
  header += "Content-Length: ";
  header += String(totalBytes);
  header += "\r\n\r\n";

  if (!vsLanWriteAll(client,
                     reinterpret_cast<const uint8_t*>(header.c_str()),
                     header.length())) {
    result.error = "HTTP header write mislukt";
    client.stop();
    return result;
  }

  const uint32_t payloadStarted = millis();
  uint32_t sent = 0;
  uint32_t nextUi = 0;

  while (sent < totalBytes) {
    size_t chunk = totalBytes - sent;
    if (chunk > VS_LAN_BUFFER_BYTES) chunk = VS_LAN_BUFFER_BYTES;

    if (!vsLanWriteAll(client, vsLanBenchBuffer, chunk)) {
      result.error = String("payload stop bij ") + sent + "/" + totalBytes;
      client.stop();
      return result;
    }
    sent += static_cast<uint32_t>(chunk);

    const uint32_t pct =
        totalBytes ? static_cast<uint32_t>(
                         (static_cast<uint64_t>(sent) * 100ULL) / totalBytes)
                   : 100;
    if (pct >= nextUi) {
      char progress[48];
      snprintf(progress, sizeof(progress), "%lu%%  %.1f / %.1f MB",
               (unsigned long)pct,
               sent / 1000000.0,
               totalBytes / 1000000.0);
      vsLanDraw("LAN TEST", label, progress, C_BLUE);
      nextUi = pct + 10;
    }
  }

  result.payloadMs = millis() - payloadStarted;
  result.response = vsLanReadHttpBody(client, 30000);
  result.totalMs = millis() - requestStarted;
  client.stop();

  if (result.payloadMs == 0) result.payloadMs = 1;
  result.kibPerSec = static_cast<uint32_t>(
      (static_cast<uint64_t>(totalBytes) * 1000ULL) /
      (1024ULL * result.payloadMs));
  result.kbitPerSec = static_cast<uint32_t>(
      (static_cast<uint64_t>(totalBytes) * 8ULL * 1000ULL) /
      (1000ULL * result.payloadMs));
  result.ok = true;
  return result;
}

static void vsLanPrintResult(const char* label, const VsLanResult& r) {
  if (!r.ok) {
    Serial.printf("LAN BENCH: %s FAIL %s\n", label, r.error.c_str());
    return;
  }

  Serial.printf(
      "LAN BENCH: %s bytes=%lu payload=%lums total=%lums "
      "rate=%lu KiB/s %lu kbit/s server=%s\n",
      label,
      (unsigned long)r.bytes,
      (unsigned long)r.payloadMs,
      (unsigned long)r.totalMs,
      (unsigned long)r.kibPerSec,
      (unsigned long)r.kbitPerSec,
      r.response.c_str());
}

static bool vsLanConnectWifi() {
#if VISITESCRIBE_WIFI_CONFIGURED
  state = AppState::SYNC;
  syncPhase = SyncPhase::NOT_STARTED;
  startWifiAttemptV05(0);

  const uint32_t started = millis();
  while (state == AppState::SYNC &&
         syncPhase != SyncPhase::CONNECTED &&
         syncPhase != SyncPhase::FAILED &&
         syncPhase != SyncPhase::NO_CREDENTIALS &&
         millis() - started < 45000) {
    serviceSyncV05();

    String line = "WiFi verbinden...";
    if (syncNetwork < 3 && wifiProfileConfiguredV05(syncNetwork)) {
      line = String("WiFi: ") + wifiSsidV05(syncNetwork);
    }
    vsLanDraw("LAN TEST", line, VISITESCRIBE_LAN_BENCH_HOST, C_BLUE);
    M5.update();
    delay(40);
  }

  return syncPhase == SyncPhase::CONNECTED && WiFi.status() == WL_CONNECTED;
#else
  return false;
#endif
}

static void vsLanMenuBenchmark() {
  Serial.printf(
      "LAN BENCH: start host=%s port=%u PCM=%lu bytes OPUS_EQ=%lu bytes\n",
      VISITESCRIBE_LAN_BENCH_HOST,
      (unsigned)VISITESCRIBE_LAN_BENCH_PORT,
      (unsigned long)VS_LAN_PCM_BYTES,
      (unsigned long)VS_LAN_OPUS_EQ_BYTES);

  pwrClickPendingV03 = false;
  pwrFirstClickMsV03 = 0;
  vsLanFillBuffer();
  noteActivity();

  if (!vsLanConnectWifi()) {
    vsLanBenchStatus = "WiFi mislukt";
    Serial.println("LAN BENCH: WiFi connection failed");
    vsLanDraw("LAN TEST FOUT", "WiFi niet verbonden",
              "controleer wifi_secrets.h", C_RED);
    delay(1800);
    wifiOff();
    syncPhase = SyncPhase::NOT_STARTED;
    state = AppState::MENU;
    screenDirty = true;
    return;
  }

  WiFi.setSleep(false);
  delay(100);
  Serial.printf("LAN BENCH: WiFi OK SSID=%s IP=%s RSSI=%d dBm\n",
                WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str(),
                WiFi.RSSI());

  vsLanDraw("LAN TEST", "10 min 16k mono PCM", "19.2 MB dummy");
  VsLanResult pcm =
      vsLanUpload("pcm16-16k-mono-10min",
                  VS_LAN_PCM_BYTES,
                  VS_LAN_BENCH_AUDIO_SECONDS);
  vsLanPrintResult("PCM16_16K_MONO_10MIN", pcm);

  VsLanResult opus;
  if (pcm.ok) {
    vsLanDraw("LAN TEST", "10 min 24k Opus-size", "1.8 MB dummy");
    opus = vsLanUpload("opus24k-size-only-10min",
                       VS_LAN_OPUS_EQ_BYTES,
                       VS_LAN_BENCH_AUDIO_SECONDS);
    vsLanPrintResult("OPUS24K_SIZE_ONLY_10MIN", opus);
  }

  if (pcm.ok) {
    const uint32_t pcmHourMs = static_cast<uint32_t>(
        (115200000ULL * pcm.payloadMs) / pcm.bytes);
    const uint32_t opusHourMs = static_cast<uint32_t>(
        (10800000ULL * pcm.payloadMs) / pcm.bytes);

    char rate[64];
    snprintf(rate, sizeof(rate), "%.2f Mbit/s",
             pcm.kbitPerSec / 1000.0);

    char estimate[96];
    snprintf(estimate, sizeof(estimate),
             "1u PCM~%.1fs  |  1u Opus~%.1fs",
             pcmHourMs / 1000.0,
             opusHourMs / 1000.0);

    vsLanBenchStatus = rate;
    vsLanDraw("LAN TEST KLAAR", rate, estimate, C_GREEN);

    Serial.printf(
        "LAN BENCH SUMMARY: transport=%.3f Mbit/s; "
        "estimated 1h 16k-mono-PCM=%.2fs; 1h 24kbit-Opus-sized=%.2fs\n",
        pcm.kbitPerSec / 1000.0,
        pcmHourMs / 1000.0,
        opusHourMs / 1000.0);
  } else {
    vsLanBenchStatus = "LAN test mislukt";
    vsLanDraw("LAN TEST FOUT", pcm.error, VISITESCRIBE_LAN_BENCH_HOST, C_RED);
  }

  delay(4000);
  WiFi.setSleep(true);
  wifiOff();
  syncPhase = SyncPhase::NOT_STARTED;
  state = AppState::MENU;
  lastUserActivityMs = millis();
  screenDirty = true;
}

struct VsLanHookInstaller {
  VsLanHookInstaller() {
    vsLanBenchmarkHook = &vsLanMenuBenchmark;
    vsLanBenchmarkStatusHook = &vsLanStatusText;
  }
};
static VsLanHookInstaller vsLanHookInstaller;
