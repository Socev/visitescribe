// VisiteScribe CoreS3-Lite temporary synthetic sync benchmark.
//
// Adds MENU > SYNC TEST without creating a recorder session on microSD.
// The benchmark creates a fresh server session and sends ten full-size
// production speech chunks (30 s, 16 kHz, mono PCM WAV; 960060 encrypted bytes
// each) through the exact v0.6.7 AES-GCM + persistent HTTPS chunk path.
//
// The server session is deliberately NOT finalized. That means the server still
// performs authentication, ciphertext hash, AES-GCM decrypt/authentication,
// plaintext hash, WAV validation and durable chunk storage, but no downstream
// transcription/processing is started for the dummy audio.

static constexpr uint32_t VS068_TEST_CHUNKS = 10;
static constexpr uint32_t VS068_TEST_DATA_BYTES =
    VS_SPEECH_RATE * VS_SPEECH_CHANNELS * (VS_SPEECH_BITS / 8) * VS_SYNC_CHUNK_SECONDS;
static constexpr size_t VS068_TEST_PLAIN_BYTES = sizeof(WAVHeader) + VS068_TEST_DATA_BYTES;
static constexpr size_t VS068_TEST_CIPHER_BYTES = VS068_TEST_PLAIN_BYTES + VS_GCM_TAG_BYTES;

static void vs068PrepareDummyWav(uint32_t sequence, uint32_t& fillMs) {
  const uint32_t started = millis();
  WAVHeader h;
  h.audioFormat = 1;
  h.numChannels = VS_SPEECH_CHANNELS;
  h.sampleRate = VS_SPEECH_RATE;
  h.bitsPerSample = VS_SPEECH_BITS;
  h.blockAlign = VS_SPEECH_CHANNELS * (VS_SPEECH_BITS / 8);
  h.byteRate = VS_SPEECH_RATE * h.blockAlign;
  h.dataSize = VS068_TEST_DATA_BYTES;
  h.fileSize = 36 + h.dataSize;
  memcpy(vsPlain, &h, sizeof(h));

  // Silence is intentional: after AES-GCM the transmitted bytes are still
  // pseudo-random and incompressible, so this is a realistic transport load
  // without wasting CPU generating fake speech. Put a tiny sequence marker in
  // the first samples so plaintext hashes differ between chunks as well.
  memset(vsPlain + sizeof(WAVHeader), 0, VS068_TEST_DATA_BYTES);
  int16_t* pcm = reinterpret_cast<int16_t*>(vsPlain + sizeof(WAVHeader));
  pcm[0] = static_cast<int16_t>(sequence);
  pcm[1] = static_cast<int16_t>(-static_cast<int32_t>(sequence));
  fillMs += millis() - started;
}

static bool vs068BuildManifest(const VsLocalSession& session,
                               const uint8_t sessionKey[32],
                               const String& serverKeyId,
                               const String& wrappedKeyB64,
                               String& out) {
  out = "";
  out.reserve(9000);
  out += "{\"schema_version\":2,\"session_id\":\"" + session.uuid +
         "\",\"device_id\":\"" + String(VISITESCRIBE_DEVICE_ID) +
         "\",\"mode\":\"meeting\",\"status\":\"complete\",";
  out += "\"audio\":{\"codec\":\"wav\",\"sample_rate\":" + String(VS_SPEECH_RATE) +
         ",\"channels\":" + String(VS_SPEECH_CHANNELS) +
         ",\"sample_format\":\"S16_LE\",\"chunk_seconds\":" +
         String(VS_SYNC_CHUNK_SECONDS) + "},";
  out += "\"encryption\":{\"algorithm\":\"AES-256-GCM\",\"local_key_wrap\":null,";
  out += "\"server_key_wrap\":{\"algorithm\":\"RSA-OAEP-SHA256\",\"key_id\":\"" +
         serverKeyId + "\",\"ciphertext_b64\":\"" + wrappedKeyB64 + "\"}},";
  out += "\"chunks\":[";

  for (uint32_t sequence = 1; sequence <= VS068_TEST_CHUNKS; ++sequence) {
    VsChunkMeta meta;
    if (!vsFillCryptoMeta(sessionKey, session.uuid, sequence, false, meta)) return false;
    if (sequence > 1) out += ',';
    char item[520];
    snprintf(item, sizeof(item),
             "{\"sequence\":%lu,\"file\":\"benchmark/chunk-%06lu.wav.enc\","
             "\"nonce_b64\":\"%s\",\"aad\":\"%s\","
             "\"plaintext_size\":%u,\"ciphertext_size\":%u,"
             "\"start_offset_ms\":%lu,\"duration_ms\":%lu}",
             (unsigned long)sequence, (unsigned long)sequence,
             meta.nonceB64.c_str(), meta.aad.c_str(),
             (unsigned)VS068_TEST_PLAIN_BYTES, (unsigned)VS068_TEST_CIPHER_BYTES,
             (unsigned long)((sequence - 1) * VS_SYNC_CHUNK_SECONDS * 1000UL),
             (unsigned long)(VS_SYNC_CHUNK_SECONDS * 1000UL));
    out += item;
  }
  out += "]}";
  return true;
}

static bool vs068PostManifest(const VsLocalSession& session, const String& payload) {
  vsSetStage(VsServerStage::CREATE_SESSION, "SYNC TEST");
  HTTPClient http;
  const String url = String(VISITESCRIBE_SERVER_BASE_URL) + "/v1/sessions";
  if (!vsBeginHttp(http, url)) return vsFail("TEST HTTPS sessie start mislukt");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Idempotency-Key", session.uuid + ":create");
  const int code = http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(payload.c_str())),
                             payload.length());
  String body = code >= 0 ? http.getString() : String();
  http.end();
  if (code < 200 || code >= 300) {
    return vsFail(code < 0 ? String("TEST HTTPS create fout ") + code
                           : vsHttpErrorBody(body, code));
  }
  Serial.printf("SERVER TEST: create uuid=%s -> HTTP %d manifest=%u bytes\n",
                session.uuid.c_str(), code, (unsigned)payload.length());
  return true;
}

static bool vs068RunSyntheticUpload() {
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

  vsServerStage = VsServerStage::UPLOAD_CHUNKS;
  HTTPClient chunkHttp;
  bool httpStarted = false;
  uint64_t sentBytes = 0;
  const uint32_t testStarted = millis();

  for (uint32_t sequence = 1; sequence <= VS068_TEST_CHUNKS; ++sequence) {
    vsServerChunkCurrent = sequence;
    vsServerMessage = String("Dummy ") + sequence + "/" + VS068_TEST_CHUNKS;
    vsDrawServerSync(true);

    Vs067PrepTiming timing;
    uint32_t fillMs = 0;
    const uint32_t prepStarted = millis();
    vs068PrepareDummyWav(sequence, fillMs);

    VsChunkMeta meta;
    if (!vs067EncryptPreparedChunk(sessionKey, test.uuid, sequence,
                                   VS068_TEST_PLAIN_BYTES, false, meta, timing)) {
      chunkHttp.end();
      memset(sessionKey, 0, sizeof(sessionKey));
      return vsFail("TEST chunk encryptie mislukt");
    }
    meta.durationMs = VS_SYNC_CHUNK_SECONDS * 1000UL;
    const uint32_t prepMs = millis() - prepStarted;

    bool reused = false, keptAlive = false;
    const uint32_t httpStartedAt = millis();
    if (!vs067PutChunk(chunkHttp, httpStarted, test, meta, reused, keptAlive)) {
      chunkHttp.end();
      memset(sessionKey, 0, sizeof(sessionKey));
      return false;
    }
    const uint32_t httpMs = millis() - httpStartedAt;
    sentBytes += meta.ciphertextBytes;
    const uint32_t kibPerSec = httpMs
        ? static_cast<uint32_t>((static_cast<uint64_t>(meta.ciphertextBytes) * 1000ULL) /
                                (1024ULL * httpMs))
        : 0;

    Serial.printf(
        "SERVER TEST: chunk %lu/%lu prep=%lums [fill=%lu sha=%lu aes=%lu] "
        "http=%lums rate=%lu KiB/s tls_reused=%d keepalive=%d bytes=%u\n",
        (unsigned long)sequence, (unsigned long)VS068_TEST_CHUNKS,
        (unsigned long)prepMs, (unsigned long)fillMs,
        (unsigned long)timing.shaMs, (unsigned long)timing.aesMs,
        (unsigned long)httpMs, (unsigned long)kibPerSec,
        reused ? 1 : 0, keptAlive ? 1 : 0, (unsigned)meta.ciphertextBytes);
    delay(1);
  }
  chunkHttp.end();
  memset(sessionKey, 0, sizeof(sessionKey));

  VsRemoteStatus remote;
  if (!vsFetchRemoteStatus(test, remote, true)) return false;
  if (!remote.valid || remote.expectedChunks != VS068_TEST_CHUNKS ||
      remote.receivedChunks != VS068_TEST_CHUNKS || !remote.missing.empty()) {
    return vsFail(String("TEST server status onverwacht received=") + remote.receivedChunks +
                  " missing=" + remote.missing.size());
  }

  const uint32_t totalMs = millis() - testStarted;
  const uint32_t avgKibPerSec = totalMs
      ? static_cast<uint32_t>((sentBytes * 1000ULL) / (1024ULL * totalMs))
      : 0;
  vsServerSessionsDone = 1;
  vsSetStage(VsServerStage::DONE, "TEST 10 chunks klaar");
  Serial.printf(
      "SERVER TEST: DONE uuid=%s chunks=%lu bytes=%llu total=%lums avg=%lu KiB/s; "
      "session bewust NIET finalized (geen transcriptie)\n",
      test.uuid.c_str(), (unsigned long)VS068_TEST_CHUNKS,
      (unsigned long long)sentBytes, (unsigned long)totalMs,
      (unsigned long)avgKibPerSec);
  return true;
}

static void vs068MenuSyntheticTest() {
  Serial.println("SERVER TEST: MENU request; connecting WiFi without syncing real recordings");
  beginSync();
  screenDirty = true;
  render(true);

  // This hook runs synchronously from the menu touch handler. Drive the proven
  // v0.5 Wi-Fi state machine here so v0.6.7 never gets a chance to start the
  // normal pending-recording queue before the benchmark takes ownership.
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
    Serial.println("SERVER TEST: WiFi connection failed; benchmark not started");
    return;
  }

  vsServerSyncRunning = true;
  noteActivity();
  WiFi.setSleep(false);
  delay(20);
  Serial.println("SERVER TEST: starting 10 full-size chunks; WiFi power-save OFF");
  vs068RunSyntheticUpload();
  vsReleaseChunkBuffers();
  WiFi.setSleep(true);
  Serial.println("SERVER TEST: ended; WiFi power-save ON");
  vsServerSyncRunning = false;
}

struct Vs068HookInstaller {
  Vs068HookInstaller() { vsSyntheticTestHook = &vs068MenuSyntheticTest; }
};
static Vs068HookInstaller vs068HookInstaller;
