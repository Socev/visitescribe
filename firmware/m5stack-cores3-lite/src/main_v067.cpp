// VisiteScribe CoreS3-Lite v0.6.7
//
// Performance layer over v0.6.6:
// - keeps one HTTPClient alive for the complete chunk upload sequence so
//   HTTP/1.1 keep-alive can preserve the TCP/TLS connection between chunks;
// - replaces the 12-16 KiB SD read loop with a 512 KiB PSRAM scratch buffer;
// - splits preparation timing into SD read, downsample/mix, SHA and AES time;
// - leaves recording format, manifests, crypto identity, resume semantics and
//   the original 48 kHz stereo WAV masters unchanged.

#define setup setup_v066_base
#define loop loop_v066_base
#include "main_v066.cpp"
#undef setup
#undef loop

#include <esp32-hal-psram.h>

static constexpr size_t VS067_SCRATCH_BYTES = 512U * 1024U;
static uint8_t* vs067Scratch = nullptr;

static bool vsUsbSyncActive = false;
static String vsUsbRxLine;

static bool vsUsbSafePath(const String& path) {
  return path.startsWith("/visitescribe/") &&
         path.indexOf("..") < 0 &&
         path.length() < 180;
}

static void vsUsbDraw(const char* status, const char* sub = nullptr) {
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

static void vsUsbReplyList() {
  const auto prefixes = vsPendingPrefixes();
  uint32_t listed = 0;
  for (const auto& prefix : prefixes) {
    VsLocalSession local;
    if (!vsLoadLocalSession(prefix, local)) continue;

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
    }
    Serial.println("VSUSB ENDSESSION");
    ++listed;
  }
  Serial.printf("VSUSB ENDLIST %lu\n", (unsigned long)listed);
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
    Serial.write(vs067Scratch, got);
    sent += got;
  }
  f.close();
  Serial.flush();
  Serial.printf("\nVSUSB ENDDATA %lu\n", (unsigned long)sent);
  Serial.flush();
}

static void vsUsbHandleCommand(String line) {
  line.trim();
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
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    vsUsbDraw("PC VERBONDEN", "USB protocol v1");
    Serial.println("VSUSB OK ENTER");
    return;
  }

  if (!vsUsbSyncActive) {
    Serial.println("VSUSB ERROR NOT_ENTERED");
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
    vsUsbReadFile(path, offset, length);
    return;
  }

  Serial.println("VSUSB ERROR UNKNOWN");
}

static bool vsUsbSyncService() {
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
    http.setTimeout(VS_HTTP_TIMEOUT_MS);
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

  const auto prefixes = vsPendingPrefixes();
  vsServerSessionsTotal = prefixes.size();
  vsServerSessionsDone = 0;
  vsServerSessionsSkipped = 0;
  if (prefixes.empty()) {
    vsSetStage(VsServerStage::NOTHING, "Alle opnames zijn gesynchroniseerd");
    return true;
  }

  String serverKeyId, serverPublicPem;
  if (!vsFetchServerKey(serverKeyId, serverPublicPem)) return false;
  for (const auto& prefix : prefixes) {
    if (WiFi.status() != WL_CONNECTED) return vsFail("WiFi verbinding verloren");

    VsLocalSession local;
    if (!vsLoadLocalSession(prefix, local)) {
      return vsFail(String("Lokale sessie fout: ") + prefix);
    }

    // A damaged/empty local recording must never block later consultations.
    // Keep every local file and sync marker untouched so it remains available
    // for manual recovery, but skip it for this automatic queue.
    const uint32_t localSpeechChunks = vsCountChunks(local, false);
    if (localSpeechChunks == 0) {
      ++vsServerSessionsSkipped;
      vsServerSessionPrefix = local.prefix;
      vsServerChunkCurrent = vsServerChunkTotal = 0;
      Serial.printf(
          "SERVER: SKIP session %s: no valid 16k speech chunks; local files retained\n",
          local.prefix.c_str());
      vsSetStage(VsServerStage::PREPARE, "OVERGESLAGEN - lokale audio ongeldig");
      continue;
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
  vs067SyncAllPending();
  vsReleaseChunkBuffers();
  WiFi.setSleep(true);
  Serial.println("SERVER: v0.6.7 sync ended; WiFi power-save ON");
  vsServerSyncRunning = false;
}

void setup() {
  setup_v066_base();
  if (!vs067EnsureScratch()) {
    Serial.println("SERVER: WARNING v0.6.7 large-read scratch unavailable");
  }
  Serial.println("VisiteScribe CoreS3-Lite v0.6.7; SD=512KiB reads; HTTP/TLS=persistent; timing=split");
}

void loop() {
  // The PC sync app gets exclusive use of USB Serial after an explicit ENTER
  // handshake. While active, do not run normal UI/Wi-Fi/server code so binary
  // file reads cannot be polluted by debug output.
  if (vsUsbSyncService()) {
    delay(1);
    return;
  }

  // Deliberately do not call loop_v066_base(): that would invoke the v0.6.6
  // sync engine as well. Reuse the proven recorder/UI loop and service only the
  // v0.6.7 sync engine here.
  loop_v05();
  vs067ServiceServerSync();

  // Server-sync screens are redrawn explicitly on real state/progress changes.
  // Do not repaint the complete screen continuously here; that caused visible
  // flicker during long uploads without adding any information.
}
