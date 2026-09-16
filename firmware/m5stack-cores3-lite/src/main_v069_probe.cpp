// VisiteScribe CoreS3-Lite v0.6.9 synthetic transport probe.
//
// Extends the temporary v0.6.8 SYNC TEST with a raw HTTP/1.1-over-TLS PUT path
// so payload write time can be measured separately from server response time.
// Normal recording and normal sync still use the production v0.6.7 path.

#include "main_v068_test.cpp"

static bool vs069ParseServer(String& host, uint16_t& port) {
  String base = VISITESCRIBE_SERVER_BASE_URL;
  if (base.startsWith("https://")) base.remove(0, 8);
  else if (base.startsWith("http://")) return false;
  int slash = base.indexOf('/');
  if (slash >= 0) base = base.substring(0, slash);
  port = 443;
  int colon = base.lastIndexOf(':');
  if (colon > 0) {
    port = static_cast<uint16_t>(base.substring(colon + 1).toInt());
    base = base.substring(0, colon);
  }
  host = base;
  return host.length() > 0 && port > 0;
}

static bool vs069ReadLine(WiFiClientSecure& tls, String& line,
                          uint32_t timeoutMs, uint32_t* firstByteMs = nullptr,
                          uint32_t waitStarted = 0) {
  line = "";
  const uint32_t started = millis();
  bool sawAny = false;
  while (millis() - started < timeoutMs) {
    while (tls.available()) {
      const int c = tls.read();
      if (c < 0) break;
      if (!sawAny) {
        sawAny = true;
        if (firstByteMs && *firstByteMs == UINT32_MAX) {
          *firstByteMs = millis() - waitStarted;
        }
      }
      if (c == '\n') return true;
      if (c != '\r') line += static_cast<char>(c);
    }
    if (!tls.connected() && !tls.available()) return false;
    delay(1);
  }
  return false;
}

static bool vs069ReadExactTls(WiFiClientSecure& tls, size_t bytes, uint32_t timeoutMs) {
  uint8_t scratch[256];
  size_t gotTotal = 0;
  uint32_t lastProgress = millis();
  while (gotTotal < bytes) {
    int avail = tls.available();
    if (avail > 0) {
      size_t want = bytes - gotTotal;
      if (want > sizeof(scratch)) want = sizeof(scratch);
      if (want > static_cast<size_t>(avail)) want = static_cast<size_t>(avail);
      int got = tls.read(scratch, want);
      if (got > 0) {
        gotTotal += static_cast<size_t>(got);
        lastProgress = millis();
        continue;
      }
    }
    if (!tls.connected() && !tls.available()) return false;
    if (millis() - lastProgress >= timeoutMs) return false;
    delay(1);
  }
  return true;
}

static bool vs069DrainChunked(WiFiClientSecure& tls, uint32_t timeoutMs) {
  while (true) {
    String sizeLine;
    if (!vs069ReadLine(tls, sizeLine, timeoutMs)) return false;
    int semi = sizeLine.indexOf(';');
    if (semi >= 0) sizeLine = sizeLine.substring(0, semi);
    sizeLine.trim();
    const size_t chunkBytes = static_cast<size_t>(strtoul(sizeLine.c_str(), nullptr, 16));
    if (chunkBytes == 0) {
      // Consume optional trailers through the final blank line.
      while (true) {
        String trailer;
        if (!vs069ReadLine(tls, trailer, timeoutMs)) return false;
        if (trailer.length() == 0) return true;
      }
    }
    if (!vs069ReadExactTls(tls, chunkBytes, timeoutMs)) return false;
    String crlf;
    if (!vs069ReadLine(tls, crlf, timeoutMs)) return false;
  }
}

struct Vs069HttpTiming {
  uint32_t writeMs = 0;
  uint32_t firstByteMs = 0;
  uint32_t responseMs = 0;
  uint32_t writeCalls = 0;
  bool keepAlive = false;
  int status = 0;
};

static bool vs069RawPutChunk(WiFiClientSecure& tls, const String& host,
                             const VsLocalSession& session, const VsChunkMeta& meta,
                             Vs069HttpTiming& timing) {
  String headers;
  headers.reserve(900);
  headers += "PUT /v1/sessions/" + session.uuid + "/chunks/" + String(meta.sequence) + " HTTP/1.1\r\n";
  headers += "Host: " + host + "\r\n";
  headers += "User-Agent: VisiteScribe-ESP32/v0.6.9-probe\r\n";
  headers += "Connection: keep-alive\r\n";
  headers += "Content-Type: application/octet-stream\r\n";
  headers += "Content-Length: " + String(meta.ciphertextBytes) + "\r\n";
  headers += "X-Device-ID: " + String(VISITESCRIBE_DEVICE_ID) + "\r\n";
  if (strlen(VISITESCRIBE_DEVICE_TOKEN)) {
    headers += "Authorization: Bearer " + String(VISITESCRIBE_DEVICE_TOKEN) + "\r\n";
  }
  headers += "X-Chunk-SHA256: " + meta.ciphertextSha256 + "\r\n";
  headers += "X-Chunk-Nonce: " + meta.nonceB64 + "\r\n";
  headers += "X-Chunk-AAD: " + meta.aad + "\r\n";
  headers += "X-Plaintext-SHA256: " + meta.plaintextSha256 + "\r\n";
  headers += "Idempotency-Key: " + session.uuid + ":chunk:" + String(meta.sequence) + "\r\n\r\n";

  const size_t headerWritten = tls.write(
      reinterpret_cast<const uint8_t*>(headers.c_str()), headers.length());
  if (headerWritten != headers.length()) {
    return vsFail(String("TEST raw header short write ") + headerWritten + "/" + headers.length());
  }

  size_t sent = 0;
  const uint32_t writeStarted = millis();
  while (sent < meta.ciphertextBytes) {
    const size_t n = tls.write(vsCipher + sent, meta.ciphertextBytes - sent);
    ++timing.writeCalls;
    if (n == 0) {
      return vsFail(String("TEST raw payload write stop at ") + sent);
    }
    sent += n;
  }
  timing.writeMs = millis() - writeStarted;

  const uint32_t responseStarted = millis();
  uint32_t firstByte = UINT32_MAX;
  String statusLine;
  if (!vs069ReadLine(tls, statusLine, VS_HTTP_TIMEOUT_MS, &firstByte, responseStarted)) {
    return vsFail("TEST raw response status timeout");
  }
  timing.firstByteMs = firstByte == UINT32_MAX ? 0 : firstByte;

  int firstSpace = statusLine.indexOf(' ');
  if (firstSpace < 0) return vsFail(String("TEST bad HTTP status: ") + statusLine);
  timing.status = statusLine.substring(firstSpace + 1).toInt();

  int64_t contentLength = -1;
  bool chunked = false;
  bool connectionClose = false;
  while (true) {
    String line;
    if (!vs069ReadLine(tls, line, VS_HTTP_TIMEOUT_MS)) {
      return vsFail("TEST raw response headers timeout");
    }
    if (line.length() == 0) break;
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) {
      String v = line.substring(line.indexOf(':') + 1);
      v.trim();
      contentLength = strtoll(v.c_str(), nullptr, 10);
    } else if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0) {
      chunked = true;
    } else if (lower.startsWith("connection:") && lower.indexOf("close") >= 0) {
      connectionClose = true;
    }
  }

  if (contentLength >= 0) {
    if (!vs069ReadExactTls(tls, static_cast<size_t>(contentLength), VS_HTTP_TIMEOUT_MS)) {
      return vsFail("TEST raw response body timeout");
    }
  } else if (chunked) {
    if (!vs069DrainChunked(tls, VS_HTTP_TIMEOUT_MS)) {
      return vsFail("TEST raw chunked response timeout");
    }
  } else if (!connectionClose) {
    return vsFail("TEST raw response has no length/chunking");
  } else {
    uint32_t last = millis();
    while (tls.connected() || tls.available()) {
      while (tls.available()) { tls.read(); last = millis(); }
      if (millis() - last > 1000) break;
      delay(1);
    }
  }

  timing.responseMs = millis() - responseStarted;
  timing.keepAlive = tls.connected() && !connectionClose;
  if (timing.status < 200 || timing.status >= 300) {
    return vsFail(String("TEST raw HTTP status ") + timing.status);
  }
  return true;
}

static bool vs069RunSyntheticUpload() {
  if (WiFi.status() != WL_CONNECTED) return vsFail("TEST WiFi niet verbonden");
  if (!vsEnsureDeviceRootKey()) return vsFail("TEST device root key ontbreekt");

  vsServerSessionPrefix = "TEST10";
  vsServerSessionsTotal = 1;
  vsServerSessionsDone = 0;
  vsServerChunkTotal = VS068_TEST_CHUNKS;
  vsServerChunkCurrent = 0;

  String serverKeyId, serverPublicPem;
  if (!vsFetchServerKey(serverKeyId, serverPublicPem)) return false;

  VsLocalSession test;
  test.prefix = "TEST10";
  test.uuid = vsRandomUuid();
  test.mode = "meeting";

  uint8_t sessionKey[32];
  if (!vsSessionKey(test.uuid, sessionKey)) return vsFail("TEST sessiesleutel mislukt");
  String wrappedKey;
  if (!vsWrapSessionKey(sessionKey, serverPublicPem, wrappedKey)) {
    memset(sessionKey, 0, sizeof(sessionKey));
    return vsFail("TEST RSA key wrap mislukt");
  }

  String manifest;
  if (!vs068BuildManifest(test, sessionKey, serverKeyId, wrappedKey, manifest)) {
    memset(sessionKey, 0, sizeof(sessionKey));
    return vsFail("TEST manifest bouwen mislukt");
  }
  if (!vs068PostManifest(test, manifest)) {
    memset(sessionKey, 0, sizeof(sessionKey));
    return false;
  }
  manifest = "";

  if (!vsEnsureChunkBuffers(VS068_TEST_PLAIN_BYTES)) {
    memset(sessionKey, 0, sizeof(sessionKey));
    return vsFail("TEST sync buffer ontbreekt");
  }

  String host;
  uint16_t port = 443;
  if (!vs069ParseServer(host, port)) {
    memset(sessionKey, 0, sizeof(sessionKey));
    return vsFail("TEST server URL niet parsebaar");
  }

  WiFiClientSecure probeTls;
  probeTls.setCACert(VISITESCRIBE_SERVER_CA_PEM);
  probeTls.setTimeout(VS_HTTP_TIMEOUT_MS / 1000);
  const uint32_t connectStarted = millis();
  if (!probeTls.connect(host.c_str(), port)) {
    memset(sessionKey, 0, sizeof(sessionKey));
    return vsFail("TEST raw TLS connect mislukt");
  }
  const uint32_t connectMs = millis() - connectStarted;
  Serial.printf("SERVER TEST PROBE: TLS connected host=%s port=%u connect=%lums\n",
                host.c_str(), (unsigned)port, (unsigned long)connectMs);

  vsServerStage = VsServerStage::UPLOAD_CHUNKS;
  uint64_t sentBytes = 0;
  uint64_t writeMsTotal = 0;
  uint64_t responseMsTotal = 0;
  const uint32_t testStarted = millis();

  for (uint32_t sequence = 1; sequence <= VS068_TEST_CHUNKS; ++sequence) {
    vsServerChunkCurrent = sequence;
    vsServerMessage = String("Probe ") + sequence + "/" + VS068_TEST_CHUNKS;
    vsDrawServerSync(true);

    Vs067PrepTiming prepTiming;
    uint32_t fillMs = 0;
    const uint32_t prepStarted = millis();
    vs068PrepareDummyWav(sequence, fillMs);
    VsChunkMeta meta;
    if (!vs067EncryptPreparedChunk(sessionKey, test.uuid, sequence,
                                   VS068_TEST_PLAIN_BYTES, false, meta, prepTiming)) {
      probeTls.stop();
      memset(sessionKey, 0, sizeof(sessionKey));
      return vsFail("TEST chunk encryptie mislukt");
    }
    meta.durationMs = VS_SYNC_CHUNK_SECONDS * 1000UL;
    const uint32_t prepMs = millis() - prepStarted;

    Vs069HttpTiming net;
    if (!vs069RawPutChunk(probeTls, host, test, meta, net)) {
      probeTls.stop();
      memset(sessionKey, 0, sizeof(sessionKey));
      return false;
    }
    sentBytes += meta.ciphertextBytes;
    writeMsTotal += net.writeMs;
    responseMsTotal += net.responseMs;
    const uint32_t rawKibPerSec = net.writeMs
        ? static_cast<uint32_t>((static_cast<uint64_t>(meta.ciphertextBytes) * 1000ULL) /
                                (1024ULL * net.writeMs))
        : 0;

    Serial.printf(
        "SERVER TEST PROBE: chunk %lu/%lu prep=%lums [fill=%lu sha=%lu aes=%lu] "
        "write=%lums raw_rate=%lu KiB/s calls=%lu firstbyte=%lums response=%lums "
        "keepalive=%d bytes=%u\n",
        (unsigned long)sequence, (unsigned long)VS068_TEST_CHUNKS,
        (unsigned long)prepMs, (unsigned long)fillMs,
        (unsigned long)prepTiming.shaMs, (unsigned long)prepTiming.aesMs,
        (unsigned long)net.writeMs, (unsigned long)rawKibPerSec,
        (unsigned long)net.writeCalls, (unsigned long)net.firstByteMs,
        (unsigned long)net.responseMs, net.keepAlive ? 1 : 0,
        (unsigned)meta.ciphertextBytes);
    delay(1);
  }

  probeTls.stop();
  memset(sessionKey, 0, sizeof(sessionKey));

  VsRemoteStatus remote;
  if (!vsFetchRemoteStatus(test, remote, true)) return false;
  if (!remote.valid || remote.expectedChunks != VS068_TEST_CHUNKS ||
      remote.receivedChunks != VS068_TEST_CHUNKS || !remote.missing.empty()) {
    return vsFail(String("TEST server status onverwacht received=") + remote.receivedChunks +
                  " missing=" + remote.missing.size());
  }

  const uint32_t totalMs = millis() - testStarted;
  const uint32_t rawAvgKib = writeMsTotal
      ? static_cast<uint32_t>((sentBytes * 1000ULL) / (1024ULL * writeMsTotal)) : 0;
  vsServerSessionsDone = 1;
  vsSetStage(VsServerStage::DONE, "TEST probe klaar");
  Serial.printf(
      "SERVER TEST PROBE: DONE uuid=%s chunks=%lu bytes=%llu total=%lums "
      "tls_connect=%lums write_total=%llums response_total=%llums raw_avg=%lu KiB/s; "
      "session bewust NIET finalized\n",
      test.uuid.c_str(), (unsigned long)VS068_TEST_CHUNKS,
      (unsigned long long)sentBytes, (unsigned long)totalMs,
      (unsigned long)connectMs, (unsigned long long)writeMsTotal,
      (unsigned long long)responseMsTotal, (unsigned long)rawAvgKib);
  return true;
}

static void vs069MenuSyntheticTest() {
  Serial.println("SERVER TEST PROBE: MENU request; connecting WiFi without real recordings");
  beginSync();
  screenDirty = true;
  render(true);

  while (state == AppState::SYNC &&
         syncPhase != SyncPhase::CONNECTED &&
         syncPhase != SyncPhase::FAILED &&
         syncPhase != SyncPhase::NO_CREDENTIALS) {
    serviceSyncV05();
    if (screenDirty) {
      screenDirty = false;
      drawSync();
    }
    M5.update();
    delay(20);
  }

  if (syncPhase != SyncPhase::CONNECTED) {
    Serial.println("SERVER TEST PROBE: WiFi connection failed; benchmark not started");
    return;
  }

  vsServerSyncRunning = true;
  noteActivity();
  WiFi.setSleep(false);
  delay(20);
  Serial.println("SERVER TEST PROBE: starting 10 chunks with split write/response timing");
  vs069RunSyntheticUpload();
  vsReleaseChunkBuffers();
  WiFi.setSleep(true);
  Serial.println("SERVER TEST PROBE: ended; WiFi power-save ON");
  vsServerSyncRunning = false;
}

struct Vs069HookInstaller {
  Vs069HookInstaller() { vsSyntheticTestHook = &vs069MenuSyntheticTest; }
};
static Vs069HookInstaller vs069HookInstaller;
