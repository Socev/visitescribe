// VisiteScribe CoreS3-Lite v0.6.6
//
// Fast/resumable server sync for the CoreS3-Lite.
//
// Local recording remains untouched: 48 kHz / 16-bit / stereo PCM WAV stays
// on microSD as the source-of-truth master. During SYNC only, audio is converted
// on the fly to 16 kHz / 16-bit / mono WAV and grouped in 30-second chunks.
// This cuts upload bytes by 6x and HTTP request count by 3x versus v0.6.
//
// Retry/resume properties:
// - the local session UUID remains persistent on microSD;
// - session keys and nonces are deterministic for byte-identical retries;
// - after create/retry, GET /status is used to upload only missing chunks;
// - a pre-v0.6.6 server session is detected by its expected legacy chunk count
//   and resumed using the original 48 kHz stereo / 10-second wire format;
// - local WAV files are never deleted by sync.

#define VISITESCRIBE_V05_SETUP_NAME setup_v05
#define VISITESCRIBE_V05_LOOP_NAME loop_v05
#include "main_v05.cpp"
#undef VISITESCRIBE_V05_SETUP_NAME
#undef VISITESCRIBE_V05_LOOP_NAME

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>

#if __has_include("server_secrets.h")
#include "server_secrets.h"
#endif

#ifndef VISITESCRIBE_SERVER_BASE_URL
#define VISITESCRIBE_SERVER_BASE_URL "https://scribe.primumnonnocere.olares.com"
#endif
#ifndef VISITESCRIBE_DEVICE_ID
#define VISITESCRIBE_DEVICE_ID "visitescribe-001"
#endif
#ifndef VISITESCRIBE_DEVICE_TOKEN
#define VISITESCRIBE_DEVICE_TOKEN ""
#endif
#ifndef VISITESCRIBE_SERVER_CA_PEM
#error "VISITESCRIBE_SERVER_CA_PEM must be supplied by main_v062.cpp or server_secrets.h"
#endif

static constexpr uint32_t VS_SPEECH_RATE = 16000;
static constexpr uint16_t VS_SPEECH_CHANNELS = 1;
static constexpr uint16_t VS_SPEECH_BITS = 16;
static constexpr uint32_t VS_SYNC_CHUNK_SECONDS = 30;
static constexpr uint32_t VS_LEGACY_CHUNK_SECONDS = 10;
static constexpr size_t VS_GCM_TAG_BYTES = 16;
static constexpr size_t VS_GCM_NONCE_BYTES = 12;
static constexpr uint32_t VS_HTTP_TIMEOUT_MS = 60000;

// 6144 int16 samples = 12 KiB. For the current 48 kHz stereo source this is
// exactly 1024 output samples per read (3 source frames x 2 channels each).
static int16_t vsSourceScratch[6144];

struct VsLocalSession {
  String prefix;
  String eventsPath;
  String uuid;
  String mode;
  std::vector<String> wavs;
};

struct VsChunkMeta {
  uint32_t sequence = 0;
  uint32_t durationMs = 0;
  size_t plaintextBytes = 0;
  size_t ciphertextBytes = 0;
  String nonceB64;
  String aad;
  String plaintextSha256;
  String ciphertextSha256;
};

struct VsRemoteStatus {
  bool valid = false;
  bool confirmed = false;
  uint32_t expectedChunks = 0;
  uint32_t receivedChunks = 0;
  std::vector<uint32_t> missing;
};

enum class VsServerStage : uint8_t {
  IDLE, FETCH_KEY, PREPARE, CREATE_SESSION, UPLOAD_CHUNKS,
  EVENTS, COMPLETE, CONFIRM, DONE, NOTHING, ERROR,
};

enum class VsManifestPost : uint8_t { OK, CONFLICT, FAILED };

static VsServerStage vsServerStage = VsServerStage::IDLE;
static String vsServerMessage;
static String vsServerError;
static String vsServerSessionPrefix;
static uint32_t vsServerChunkCurrent = 0;
static uint32_t vsServerChunkTotal = 0;
static uint32_t vsServerSessionsDone = 0;
static uint32_t vsServerSessionsTotal = 0;
static uint32_t vsServerLastDrawMs = 0;
static bool vsServerSyncRunning = false;

static uint8_t vsDeviceRootKey[32] = {0};
static bool vsDeviceRootKeyReady = false;
static WiFiClientSecure vsTls;
static bool vsTlsReady = false;
static uint8_t* vsPlain = nullptr;
static uint8_t* vsCipher = nullptr;
static size_t vsPlainCapacity = 0;
static size_t vsCipherCapacity = 0;

static const char* vsStageName(VsServerStage stage) {
  switch (stage) {
    case VsServerStage::IDLE: return "WACHT OP WIFI";
    case VsServerStage::FETCH_KEY: return "SERVER SLEUTEL";
    case VsServerStage::PREPARE: return "VOORBEREIDEN";
    case VsServerStage::CREATE_SESSION: return "SESSIE AANMAKEN";
    case VsServerStage::UPLOAD_CHUNKS: return "AUDIO UPLOAD";
    case VsServerStage::EVENTS: return "EVENTS UPLOAD";
    case VsServerStage::COMPLETE: return "AFRONDEN";
    case VsServerStage::CONFIRM: return "CONTROLEREN";
    case VsServerStage::DONE: return "SYNC KLAAR";
    case VsServerStage::NOTHING: return "NIETS TE SYNCEN";
    case VsServerStage::ERROR: return "SYNC FOUT";
  }
  return "SYNC";
}

static void vsDrawServerSync(bool force = false) {
  if (state != AppState::SYNC) return;
  if (!force && millis() - vsServerLastDrawMs < 200) return;
  vsServerLastDrawMs = millis();
  drawHeader("SERVER SYNC");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, 150, C_BG);
  centeredText(58, vsStageName(vsServerStage),
               vsServerStage == VsServerStage::ERROR ? C_RED :
               ((vsServerStage == VsServerStage::DONE || vsServerStage == VsServerStage::NOTHING) ? C_GREEN : C_NAVY), 2);
  String wifiLine = String("WIFI  ") + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "-");
  centeredText(83, wifiLine.c_str(), C_GREY, 1);
  if (vsServerSessionPrefix.length()) {
    String s = String("SESSIE  ") + vsServerSessionPrefix;
    centeredText(105, s.c_str(), C_NAVY, 1);
  }
  if (vsServerStage == VsServerStage::UPLOAD_CHUNKS && vsServerChunkTotal) {
    char p[48];
    snprintf(p, sizeof(p), "CHUNK %lu / %lu", (unsigned long)vsServerChunkCurrent, (unsigned long)vsServerChunkTotal);
    centeredText(128, p, C_BLUE, 2);
  } else if (vsServerSessionsTotal) {
    char p[48];
    snprintf(p, sizeof(p), "SESSIES %lu / %lu", (unsigned long)vsServerSessionsDone, (unsigned long)vsServerSessionsTotal);
    centeredText(128, p, C_BLUE, 1);
  }
  if (vsServerStage == VsServerStage::ERROR) {
    String e = vsServerError;
    if (e.length() > 45) e = e.substring(0, 45);
    centeredText(155, e.c_str(), C_RED, 1);
    centeredText(174, "OPNIEUW = retry", C_GREY, 1);
  } else {
    String m = vsServerMessage;
    if (m.length() > 45) m = m.substring(0, 45);
    if (m.length()) centeredText(158, m.c_str(), C_GREY, 1);
  }
  zone(SYNC_RETRY, "OPNIEUW", C_TEAL, C_WHITE);
  zone(SYNC_BACK, "TERUG", C_NAVY, C_WHITE);
}

static void vsSetStage(VsServerStage stage, const String& message = String()) {
  vsServerStage = stage;
  vsServerMessage = message;
  if (stage != VsServerStage::ERROR) vsServerError = "";
  Serial.printf("SERVER: stage=%s %s\n", vsStageName(stage), message.c_str());
  vsDrawServerSync(true);
}

static bool vsFail(const String& message) {
  vsServerError = message;
  vsServerStage = VsServerStage::ERROR;
  Serial.printf("SERVER: ERROR %s\n", message.c_str());
  vsDrawServerSync(true);
  return false;
}

static bool vsEnsureDeviceRootKey() {
  if (vsDeviceRootKeyReady) return true;
  Preferences prefs;
  if (!prefs.begin("visitescribe", false)) return false;
  size_t n = prefs.getBytesLength("rootkey");
  if (n == sizeof(vsDeviceRootKey)) {
    prefs.getBytes("rootkey", vsDeviceRootKey, sizeof(vsDeviceRootKey));
  } else {
    esp_fill_random(vsDeviceRootKey, sizeof(vsDeviceRootKey));
    if (prefs.putBytes("rootkey", vsDeviceRootKey, sizeof(vsDeviceRootKey)) != sizeof(vsDeviceRootKey)) {
      prefs.end();
      memset(vsDeviceRootKey, 0, sizeof(vsDeviceRootKey));
      return false;
    }
  }
  prefs.end();
  vsDeviceRootKeyReady = true;
  return true;
}

static bool vsHmacSha256(const uint8_t* key, size_t keyLen, const String& text, uint8_t out[32]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  return mbedtls_md_hmac(info, key, keyLen,
                         reinterpret_cast<const unsigned char*>(text.c_str()), text.length(), out) == 0;
}

static String vsHex(const uint8_t* data, size_t len) {
  static const char* hex = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += hex[data[i] >> 4];
    out += hex[data[i] & 0x0F];
  }
  return out;
}

static String vsSha256Hex(const uint8_t* data, size_t len) {
  uint8_t digest[32];
  if (mbedtls_sha256_ret(data, len, digest, 0) != 0) return String();
  return vsHex(digest, sizeof(digest));
}

static String vsBase64(const uint8_t* data, size_t len) {
  const size_t cap = 4 * ((len + 2) / 3) + 1;
  std::vector<unsigned char> out(cap);
  size_t written = 0;
  if (mbedtls_base64_encode(out.data(), out.size(), &written, data, len) != 0) return String();
  out[written] = 0;
  return String(reinterpret_cast<char*>(out.data()));
}

static String vsRandomUuid() {
  uint8_t b[16];
  esp_fill_random(b, sizeof(b));
  b[6] = (b[6] & 0x0F) | 0x40;
  b[8] = (b[8] & 0x3F) | 0x80;
  char out[37];
  snprintf(out, sizeof(out),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return String(out);
}

static String vsSyncPath(const String& prefix) {
  return String("/visitescribe/") + prefix + "_sync.txt";
}

static bool vsReadSyncMeta(const String& prefix, String& uuid, String& syncState) {
  String path = vsSyncPath(prefix);
  if (!SD.exists(path)) return false;
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("uuid=")) uuid = line.substring(5);
    else if (line.startsWith("state=")) syncState = line.substring(6);
  }
  f.close();
  return uuid.length() == 36;
}

static bool vsWriteSyncMeta(const String& prefix, const String& uuid, const char* syncState) {
  String path = vsSyncPath(prefix);
  if (SD.exists(path)) SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  f.printf("uuid=%s\nstate=%s\n", uuid.c_str(), syncState);
  f.flush();
  f.close();
  return true;
}

static bool vsEnsureSessionUuid(const String& prefix, String& uuid, String& syncState) {
  if (vsReadSyncMeta(prefix, uuid, syncState)) return true;
  uuid = vsRandomUuid();
  syncState = "queued";
  return vsWriteSyncMeta(prefix, uuid, syncState.c_str());
}

static String vsBaseName(const char* name) {
  String s(name ? name : "");
  int slash = s.lastIndexOf('/');
  if (slash >= 0) s = s.substring(slash + 1);
  return s;
}

static bool vsEventsShowComplete(const String& eventsPath) {
  File f = SD.open(eventsPath, FILE_READ);
  if (!f) return false;
  bool complete = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.indexOf(",session_stopped,") >= 0) {
      complete = true;
      break;
    }
  }
  f.close();
  return complete;
}

static std::vector<String> vsCollectWavs(const String& prefix) {
  std::vector<String> wavs;
  File dir = SD.open("/visitescribe");
  if (!dir) return wavs;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      String base = vsBaseName(f.name());
      if (base.startsWith(prefix + "_") && base.endsWith(".wav")) wavs.push_back(String("/visitescribe/") + base);
    }
    f.close();
  }
  dir.close();
  std::sort(wavs.begin(), wavs.end(), [](const String& a, const String& b) { return a.compareTo(b) < 0; });
  return wavs;
}

static String vsModeFromWavs(const std::vector<String>& wavs) {
  for (const auto& w : wavs) {
    if (w.indexOf("_round_") >= 0) return "multi_patient";
    if (w.indexOf("_meeting") >= 0) return "meeting";
  }
  return "single_patient";
}

static std::vector<String> vsPendingPrefixes() {
  std::vector<String> prefixes;
  File dir = SD.open("/visitescribe");
  if (!dir) return prefixes;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      String base = vsBaseName(f.name());
      if (base.startsWith("s") && base.endsWith("_events.csv") && base.length() >= 6) {
        String prefix = base.substring(0, 6);
        String eventsPath = String("/visitescribe/") + base;
        String uuid, st;
        bool hasMeta = vsReadSyncMeta(prefix, uuid, st);
        if ((!hasMeta || st != "ingested") && vsEventsShowComplete(eventsPath)) {
          auto wavs = vsCollectWavs(prefix);
          if (!wavs.empty()) prefixes.push_back(prefix);
        }
      }
    }
    f.close();
  }
  dir.close();
  std::sort(prefixes.begin(), prefixes.end(), [](const String& a, const String& b) { return a.compareTo(b) < 0; });
  prefixes.erase(std::unique(prefixes.begin(), prefixes.end(), [](const String& a, const String& b) { return a == b; }), prefixes.end());
  return prefixes;
}

static bool vsLoadLocalSession(const String& prefix, VsLocalSession& out) {
  out.prefix = prefix;
  out.eventsPath = String("/visitescribe/") + prefix + "_events.csv";
  out.wavs = vsCollectWavs(prefix);
  if (out.wavs.empty()) return false;
  out.mode = vsModeFromWavs(out.wavs);
  String st;
  if (!vsEnsureSessionUuid(prefix, out.uuid, st)) return false;
  return st != "ingested";
}

static bool vsReadWavHeader(File& f, WAVHeader& h) {
  if (f.read(reinterpret_cast<uint8_t*>(&h), sizeof(h)) != sizeof(h)) return false;
  if (memcmp(h.riff, "RIFF", 4) || memcmp(h.wave, "WAVE", 4) ||
      memcmp(h.fmt, "fmt ", 4) || memcmp(h.data, "data", 4)) return false;
  if (h.audioFormat != 1 || h.numChannels == 0 || h.sampleRate == 0 ||
      h.bitsPerSample != 16 || h.blockAlign == 0 || h.byteRate == 0) return false;
  return true;
}

static bool vsReadExact(File& f, uint8_t* dst, size_t len) {
  size_t total = 0;
  while (total < len) {
    size_t request = len - total;
    if (request > 16U * 1024U) request = 16U * 1024U;
    size_t got = f.read(dst + total, request);
    if (got == 0) return false;
    total += got;
  }
  return true;
}

static bool vsEnsureChunkBuffers(size_t plainNeeded) {
  const size_t cipherNeeded = plainNeeded + VS_GCM_TAG_BYTES;
  if (vsPlainCapacity >= plainNeeded && vsCipherCapacity >= cipherNeeded && vsPlain && vsCipher) return true;
  if (vsPlain) { free(vsPlain); vsPlain = nullptr; }
  if (vsCipher) { free(vsCipher); vsCipher = nullptr; }
  vsPlainCapacity = vsCipherCapacity = 0;
  vsPlain = static_cast<uint8_t*>(heap_caps_malloc(plainNeeded, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  vsCipher = static_cast<uint8_t*>(heap_caps_malloc(cipherNeeded, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!vsPlain || !vsCipher) {
    if (vsPlain) free(vsPlain);
    if (vsCipher) free(vsCipher);
    vsPlain = vsCipher = nullptr;
    return false;
  }
  vsPlainCapacity = plainNeeded;
  vsCipherCapacity = cipherNeeded;
  Serial.printf("SERVER: sync buffer plain=%u cipher=%u PSRAM free=%u\n",
                (unsigned)plainNeeded, (unsigned)cipherNeeded,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  return true;
}

static void vsReleaseChunkBuffers() {
  if (vsPlain) free(vsPlain);
  if (vsCipher) free(vsCipher);
  vsPlain = vsCipher = nullptr;
  vsPlainCapacity = vsCipherCapacity = 0;
}

static bool vsSessionKey(const String& uuid, uint8_t out[32]) {
  if (!vsEnsureDeviceRootKey()) return false;
  return vsHmacSha256(vsDeviceRootKey, sizeof(vsDeviceRootKey), String("session:") + uuid, out);
}

static bool vsChunkNonceFlavor(const uint8_t sessionKey[32], const String& uuid,
                               uint32_t sequence, bool legacy,
                               uint8_t nonce[VS_GCM_NONCE_BYTES]) {
  uint8_t full[32];
  String msg = legacy ? String("nonce:") + uuid + ":" + String(sequence)
                      : String("nonce:v3-speech:") + uuid + ":" + String(sequence);
  if (!vsHmacSha256(sessionKey, 32, msg, full)) return false;
  memcpy(nonce, full, VS_GCM_NONCE_BYTES);
  return true;
}

static bool vsFillCryptoMeta(const uint8_t sessionKey[32], const String& uuid,
                             uint32_t sequence, bool legacy, VsChunkMeta& meta) {
  uint8_t nonce[VS_GCM_NONCE_BYTES];
  if (!vsChunkNonceFlavor(sessionKey, uuid, sequence, legacy, nonce)) return false;
  meta.sequence = sequence;
  meta.aad = legacy ? String("visitescribe-v2:") + uuid + ":" + String(sequence)
                    : String("visitescribe-v3-speech:") + uuid + ":" + String(sequence);
  meta.nonceB64 = vsBase64(nonce, sizeof(nonce));
  return meta.nonceB64.length() > 0;
}

static bool vsEncryptPreparedChunk(const uint8_t sessionKey[32], const String& uuid,
                                   uint32_t sequence, size_t plaintextLen,
                                   bool legacy, VsChunkMeta& meta) {
  uint8_t nonce[VS_GCM_NONCE_BYTES];
  if (!vsChunkNonceFlavor(sessionKey, uuid, sequence, legacy, nonce)) return false;
  if (!vsFillCryptoMeta(sessionKey, uuid, sequence, legacy, meta)) return false;
  meta.plaintextBytes = plaintextLen;
  meta.plaintextSha256 = vsSha256Hex(vsPlain, plaintextLen);
  if (!meta.plaintextSha256.length()) return false;

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, sessionKey, 256);
  uint8_t tag[VS_GCM_TAG_BYTES];
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(
        &gcm, MBEDTLS_GCM_ENCRYPT, plaintextLen,
        nonce, sizeof(nonce),
        reinterpret_cast<const unsigned char*>(meta.aad.c_str()), meta.aad.length(),
        vsPlain, vsCipher, sizeof(tag), tag);
  }
  mbedtls_gcm_free(&gcm);
  if (rc != 0) return false;
  memcpy(vsCipher + plaintextLen, tag, sizeof(tag));
  meta.ciphertextBytes = plaintextLen + sizeof(tag);
  meta.ciphertextSha256 = vsSha256Hex(vsCipher, meta.ciphertextBytes);
  return meta.ciphertextSha256.length() == 64;
}

static bool vsSpeechSourceShape(const WAVHeader& h, uint32_t& ratio,
                                uint32_t& sourceGroupBytes) {
  if (h.bitsPerSample != 16 || h.numChannels < 1 || h.numChannels > 2) return false;
  if (h.sampleRate < VS_SPEECH_RATE || (h.sampleRate % VS_SPEECH_RATE) != 0) return false;
  ratio = h.sampleRate / VS_SPEECH_RATE;
  if (ratio == 0 || ratio > 8) return false;
  if (h.blockAlign != h.numChannels * sizeof(int16_t)) return false;
  sourceGroupBytes = h.blockAlign * ratio;
  return sourceGroupBytes > 0;
}

static uint32_t vsSpeechSourceBytes(const WAVHeader& h, uint32_t remaining) {
  uint32_t ratio = 0, groupBytes = 0;
  if (!vsSpeechSourceShape(h, ratio, groupBytes)) return 0;
  uint32_t wanted = h.byteRate * VS_SYNC_CHUNK_SECONDS;
  uint32_t bytes = remaining < wanted ? remaining : wanted;
  bytes -= bytes % groupBytes;
  return bytes;
}

static bool vsDescribeSpeechChunk(const WAVHeader& h, uint32_t sourceBytes,
                                  const uint8_t sessionKey[32], const String& uuid,
                                  uint32_t sequence, VsChunkMeta& meta) {
  uint32_t ratio = 0, groupBytes = 0;
  if (!vsSpeechSourceShape(h, ratio, groupBytes) || sourceBytes == 0) return false;
  const uint32_t outputSamples = sourceBytes / groupBytes;
  const uint32_t outputDataBytes = outputSamples * sizeof(int16_t);
  if (!vsFillCryptoMeta(sessionKey, uuid, sequence, false, meta)) return false;
  meta.plaintextBytes = sizeof(WAVHeader) + outputDataBytes;
  meta.ciphertextBytes = meta.plaintextBytes + VS_GCM_TAG_BYTES;
  meta.durationMs = static_cast<uint32_t>((static_cast<uint64_t>(outputSamples) * 1000ULL) / VS_SPEECH_RATE);
  return true;
}

static bool vsPrepareSpeechChunk(File& wav, const WAVHeader& sourceHeader,
                                 uint32_t& remaining, const uint8_t sessionKey[32],
                                 const String& uuid, uint32_t sequence,
                                 VsChunkMeta& meta) {
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
  const size_t scratchSamples = sizeof(vsSourceScratch) / sizeof(vsSourceScratch[0]);
  const size_t maxOutputPerRead = scratchSamples / samplesPerOutput;
  if (maxOutputPerRead == 0) return false;

  uint32_t produced = 0;
  while (produced < outputSamples) {
    size_t groups = outputSamples - produced;
    if (groups > maxOutputPerRead) groups = maxOutputPerRead;
    const size_t sourceSamples = groups * samplesPerOutput;
    const size_t readBytes = sourceSamples * sizeof(int16_t);
    if (!vsReadExact(wav, reinterpret_cast<uint8_t*>(vsSourceScratch), readBytes)) return false;

    for (size_t g = 0; g < groups; ++g) {
      int32_t sum = 0;
      const size_t base = g * samplesPerOutput;
      for (size_t s = 0; s < samplesPerOutput; ++s) sum += vsSourceScratch[base + s];
      dst[produced + g] = static_cast<int16_t>(sum / static_cast<int32_t>(samplesPerOutput));
    }
    produced += groups;
  }

  remaining -= sourceBytes;
  if (!vsEncryptPreparedChunk(sessionKey, uuid, sequence, plainLen, false, meta)) return false;
  meta.durationMs = static_cast<uint32_t>((static_cast<uint64_t>(outputSamples) * 1000ULL) / VS_SPEECH_RATE);
  return true;
}

static uint32_t vsLegacySourceBytes(const WAVHeader& h, uint32_t remaining) {
  const uint32_t wanted = h.byteRate * VS_LEGACY_CHUNK_SECONDS;
  uint32_t bytes = remaining < wanted ? remaining : wanted;
  bytes -= bytes % h.blockAlign;
  return bytes;
}

static bool vsPrepareLegacyChunk(File& wav, const WAVHeader& h, uint32_t& remaining,
                                 const uint8_t sessionKey[32], const String& uuid,
                                 uint32_t sequence, VsChunkMeta& meta) {
  const uint32_t dataBytes = vsLegacySourceBytes(h, remaining);
  if (dataBytes == 0) return false;
  const size_t plainLen = sizeof(WAVHeader) + dataBytes;
  if (!vsEnsureChunkBuffers(plainLen)) return false;
  WAVHeader chunkHeader = h;
  chunkHeader.fileSize = 36 + dataBytes;
  chunkHeader.dataSize = dataBytes;
  memcpy(vsPlain, &chunkHeader, sizeof(chunkHeader));
  if (!vsReadExact(wav, vsPlain + sizeof(WAVHeader), dataBytes)) return false;
  remaining -= dataBytes;
  if (!vsEncryptPreparedChunk(sessionKey, uuid, sequence, plainLen, true, meta)) return false;
  meta.durationMs = static_cast<uint32_t>((static_cast<uint64_t>(dataBytes) * 1000ULL) / h.byteRate);
  return true;
}

static bool vsSkipBytes(File& wav, uint32_t bytes) {
  const size_t next = wav.position() + bytes;
  return wav.seek(next);
}

static uint32_t vsCountChunks(const VsLocalSession& session, bool legacy) {
  uint32_t count = 0;
  for (const auto& path : session.wavs) {
    File wav = SD.open(path, FILE_READ);
    WAVHeader h;
    if (!wav || !vsReadWavHeader(wav, h)) {
      if (wav) wav.close();
      return 0;
    }
    uint32_t remaining = h.dataSize;
    while (remaining) {
      uint32_t bytes = legacy ? vsLegacySourceBytes(h, remaining) : vsSpeechSourceBytes(h, remaining);
      if (bytes == 0) {
        // At most a sub-resampling-group tail can remain; recorder WAVs are
        // normally exactly frame-aligned, so ignore only that tiny tail.
        remaining = 0;
        break;
      }
      ++count;
      remaining -= bytes;
    }
    wav.close();
  }
  return count;
}

static bool vsJsonExtractString(const String& json, const char* key, String& out) {
  String marker = String("\"") + key + "\"";
  int p = json.indexOf(marker);
  if (p < 0) return false;
  p = json.indexOf(':', p + marker.length());
  if (p < 0) return false;
  ++p;
  while (p < static_cast<int>(json.length()) && isspace(static_cast<unsigned char>(json[p]))) ++p;
  if (p >= static_cast<int>(json.length()) || json[p] != '"') return false;
  ++p;
  out = "";
  while (p < static_cast<int>(json.length())) {
    char c = json[p++];
    if (c == '"') return true;
    if (c == '\\' && p < static_cast<int>(json.length())) {
      char e = json[p++];
      if (e == 'n') out += '\n';
      else if (e == 'r') out += '\r';
      else if (e == 't') out += '\t';
      else if (e == '"') out += '"';
      else if (e == '\\') out += '\\';
      else if (e == '/') out += '/';
      else return false;
    } else out += c;
  }
  return false;
}

static bool vsJsonExtractUInt(const String& json, const char* key, uint32_t& out) {
  String marker = String("\"") + key + "\"";
  int p = json.indexOf(marker);
  if (p < 0) return false;
  p = json.indexOf(':', p + marker.length());
  if (p < 0) return false;
  ++p;
  while (p < static_cast<int>(json.length()) && isspace(static_cast<unsigned char>(json[p]))) ++p;
  if (p >= static_cast<int>(json.length()) || !isdigit(static_cast<unsigned char>(json[p]))) return false;
  uint64_t value = 0;
  while (p < static_cast<int>(json.length()) && isdigit(static_cast<unsigned char>(json[p]))) {
    value = value * 10 + static_cast<uint32_t>(json[p] - '0');
    if (value > 0xFFFFFFFFULL) return false;
    ++p;
  }
  out = static_cast<uint32_t>(value);
  return true;
}

static bool vsJsonExtractUIntArray(const String& json, const char* key, std::vector<uint32_t>& out) {
  out.clear();
  String marker = String("\"") + key + "\"";
  int p = json.indexOf(marker);
  if (p < 0) return false;
  p = json.indexOf('[', p + marker.length());
  if (p < 0) return false;
  ++p;
  while (p < static_cast<int>(json.length())) {
    while (p < static_cast<int>(json.length()) &&
           (isspace(static_cast<unsigned char>(json[p])) || json[p] == ',')) ++p;
    if (p >= static_cast<int>(json.length())) return false;
    if (json[p] == ']') return true;
    if (!isdigit(static_cast<unsigned char>(json[p]))) return false;
    uint64_t value = 0;
    while (p < static_cast<int>(json.length()) && isdigit(static_cast<unsigned char>(json[p]))) {
      value = value * 10 + static_cast<uint32_t>(json[p] - '0');
      if (value > 0xFFFFFFFFULL) return false;
      ++p;
    }
    out.push_back(static_cast<uint32_t>(value));
  }
  return false;
}

static bool vsStatusConfirmedBody(const String& body) {
  if (body.indexOf("\"ingest_confirmed\":true") >= 0 ||
      body.indexOf("\"ingest_confirmed\": true") >= 0) return true;
  static const char* states[] = {
    "INGESTED", "READY_FOR_PROCESSING", "TRANSCRIBING", "PROCESSING",
    "REVIEW_REQUIRED", "APPROVED"
  };
  for (const char* s : states) {
    if (body.indexOf(String("\"state\":\"") + s + "\"") >= 0 ||
        body.indexOf(String("\"state\": \"") + s + "\"") >= 0) return true;
  }
  return false;
}

static void vsAddCommonHeaders(HTTPClient& http) {
  http.addHeader("X-Device-ID", VISITESCRIBE_DEVICE_ID);
  if (strlen(VISITESCRIBE_DEVICE_TOKEN)) http.addHeader("Authorization", String("Bearer ") + VISITESCRIBE_DEVICE_TOKEN);
}

static bool vsBeginHttp(HTTPClient& http, const String& url) {
  if (!vsTlsReady) {
    vsTls.setCACert(VISITESCRIBE_SERVER_CA_PEM);
    vsTls.setTimeout(VS_HTTP_TIMEOUT_MS / 1000);
    vsTlsReady = true;
  }
  if (!http.begin(vsTls, url)) return false;
  http.setTimeout(VS_HTTP_TIMEOUT_MS);
  http.setReuse(true);
  vsAddCommonHeaders(http);
  return true;
}

static String vsHttpErrorBody(String body, int code) {
  body.replace("\n", " ");
  if (body.length() > 180) body = body.substring(0, 180);
  return String("HTTP ") + code + " " + body;
}

static bool vsFetchServerKey(String& keyId, String& publicPem) {
  vsSetStage(VsServerStage::FETCH_KEY, "GET /v1/server/public-key");
  HTTPClient http;
  String url = String(VISITESCRIBE_SERVER_BASE_URL) + "/v1/server/public-key";
  if (!vsBeginHttp(http, url)) return vsFail("TLS/HTTP start mislukt");
  int code = http.GET();
  String body = code >= 0 ? http.getString() : String();
  http.end();
  if (code < 200 || code >= 300) {
    return vsFail(code < 0 ? String("HTTPS fout ") + code : vsHttpErrorBody(body, code));
  }
  if (!vsJsonExtractString(body, "key_id", keyId) ||
      !vsJsonExtractString(body, "public_key_pem", publicPem)) {
    return vsFail("Server public-key JSON ongeldig");
  }
  Serial.printf("SERVER: public key %s, PEM bytes=%u\n", keyId.c_str(), (unsigned)publicPem.length());
  return true;
}

static int vsRng(void*, unsigned char* output, size_t len) {
  esp_fill_random(output, len);
  return 0;
}

static bool vsWrapSessionKey(const uint8_t sessionKey[32], const String& publicPem, String& wrappedB64) {
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int rc = mbedtls_pk_parse_public_key(
      &pk, reinterpret_cast<const unsigned char*>(publicPem.c_str()), publicPem.length() + 1);
  if (rc != 0 || !mbedtls_pk_can_do(&pk, MBEDTLS_PK_RSA)) {
    mbedtls_pk_free(&pk);
    Serial.printf("SERVER: RSA public key parse failed rc=%d\n", rc);
    return false;
  }
  mbedtls_rsa_context* rsa = mbedtls_pk_rsa(pk);
  mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);
  size_t outLen = mbedtls_pk_get_len(&pk);
  std::vector<uint8_t> wrapped(outLen);
  rc = mbedtls_rsa_rsaes_oaep_encrypt(
      rsa, vsRng, nullptr, MBEDTLS_RSA_PUBLIC,
      nullptr, 0, 32, sessionKey, wrapped.data());
  mbedtls_pk_free(&pk);
  if (rc != 0) {
    Serial.printf("SERVER: RSA OAEP wrap failed rc=%d\n", rc);
    return false;
  }
  wrappedB64 = vsBase64(wrapped.data(), wrapped.size());
  return wrappedB64.length() > 0;
}

static bool vsWriteSpeechManifest(const VsLocalSession& session,
                                  const uint8_t sessionKey[32],
                                  const String& serverKeyId,
                                  const String& wrappedKeyB64,
                                  const String& manifestPath,
                                  uint32_t& chunkCount) {
  File manifest = SD.open(manifestPath, FILE_WRITE);
  if (!manifest) return vsFail("Manifest bestand niet te maken");

  manifest.printf(
      "{\"schema_version\":2,\"session_id\":\"%s\",\"device_id\":\"%s\","
      "\"mode\":\"%s\",\"status\":\"complete\","
      "\"audio\":{\"codec\":\"wav\",\"sample_rate\":%lu,\"channels\":%u,"
      "\"sample_format\":\"S16_LE\",\"chunk_seconds\":%lu},"
      "\"encryption\":{\"algorithm\":\"AES-256-GCM\",\"local_key_wrap\":null,"
      "\"server_key_wrap\":{\"algorithm\":\"RSA-OAEP-SHA256\",\"key_id\":\"%s\","
      "\"ciphertext_b64\":\"%s\"}},\"chunks\":[",
      session.uuid.c_str(), VISITESCRIBE_DEVICE_ID, session.mode.c_str(),
      (unsigned long)VS_SPEECH_RATE, (unsigned)VS_SPEECH_CHANNELS,
      (unsigned long)VS_SYNC_CHUNK_SECONDS, serverKeyId.c_str(), wrappedKeyB64.c_str());

  const uint32_t started = millis();
  bool firstChunk = true;
  chunkCount = 0;
  uint64_t audioOffsetMs = 0;
  for (const auto& path : session.wavs) {
    File wav = SD.open(path, FILE_READ);
    WAVHeader h;
    if (!wav || !vsReadWavHeader(wav, h)) {
      if (wav) wav.close();
      manifest.close();
      return vsFail(String("WAV lezen mislukt: ") + path);
    }
    uint32_t ratio = 0, groupBytes = 0;
    if (!vsSpeechSourceShape(h, ratio, groupBytes)) {
      wav.close();
      manifest.close();
      return vsFail("WAV niet geschikt voor 16 kHz mono sync");
    }
    uint32_t remaining = h.dataSize;
    while (remaining) {
      const uint32_t sourceBytes = vsSpeechSourceBytes(h, remaining);
      if (sourceBytes == 0) { remaining = 0; break; }
      ++chunkCount;
      VsChunkMeta meta;
      if (!vsDescribeSpeechChunk(h, sourceBytes, sessionKey, session.uuid, chunkCount, meta)) {
        wav.close();
        manifest.close();
        return vsFail("Audio chunk metadata mislukt");
      }
      remaining -= sourceBytes;
      if (!firstChunk) manifest.print(',');
      firstChunk = false;
      manifest.printf(
          "{\"sequence\":%lu,\"file\":\"audio/chunk-%06lu.wav.enc\","
          "\"nonce_b64\":\"%s\",\"aad\":\"%s\","
          "\"plaintext_size\":%u,\"ciphertext_size\":%u,"
          "\"start_offset_ms\":%llu,\"duration_ms\":%lu}",
          (unsigned long)meta.sequence, (unsigned long)meta.sequence,
          meta.nonceB64.c_str(), meta.aad.c_str(),
          (unsigned)meta.plaintextBytes, (unsigned)meta.ciphertextBytes,
          (unsigned long long)audioOffsetMs, (unsigned long)meta.durationMs);
      audioOffsetMs += meta.durationMs;
      if ((chunkCount % 32) == 0) manifest.flush();
    }
    wav.close();
  }
  manifest.print("]}");
  manifest.flush();
  manifest.close();
  Serial.printf("SERVER: speech manifest chunks=%lu built=%lums format=16k-mono/30s\n",
                (unsigned long)chunkCount, (unsigned long)(millis() - started));
  return chunkCount > 0;
}

static VsManifestPost vsPostManifest(const VsLocalSession& session, const String& manifestPath) {
  vsSetStage(VsServerStage::CREATE_SESSION, session.prefix);
  File manifest = SD.open(manifestPath, FILE_READ);
  if (!manifest) {
    vsFail("Manifest niet te openen");
    return VsManifestPost::FAILED;
  }
  HTTPClient http;
  String url = String(VISITESCRIBE_SERVER_BASE_URL) + "/v1/sessions";
  if (!vsBeginHttp(http, url)) {
    manifest.close();
    vsFail("HTTPS sessie start mislukt");
    return VsManifestPost::FAILED;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Idempotency-Key", session.uuid + ":create");
  int code = http.sendRequest("POST", &manifest, manifest.size());
  manifest.close();
  String body = code >= 0 ? http.getString() : String();
  http.end();
  if (code >= 200 && code < 300) {
    Serial.printf("SERVER: create %s -> HTTP %d\n", session.uuid.c_str(), code);
    return VsManifestPost::OK;
  }
  if (code == 409) {
    body.replace("\n", " ");
    if (body.length() > 160) body = body.substring(0, 160);
    Serial.printf("SERVER: create conflict; probing existing session: %s\n", body.c_str());
    return VsManifestPost::CONFLICT;
  }
  vsFail(code < 0 ? String("HTTPS create fout ") + code : vsHttpErrorBody(body, code));
  return VsManifestPost::FAILED;
}

static bool vsFetchRemoteStatus(const VsLocalSession& session, VsRemoteStatus& out,
                                bool failOnError) {
  HTTPClient http;
  String url = String(VISITESCRIBE_SERVER_BASE_URL) + "/v1/sessions/" + session.uuid + "/status";
  if (!vsBeginHttp(http, url)) {
    if (failOnError) return vsFail("HTTPS status start mislukt");
    return false;
  }
  int code = http.GET();
  String body = code >= 0 ? http.getString() : String();
  http.end();
  if (code < 200 || code >= 300) {
    if (failOnError) return vsFail(code < 0 ? String("HTTPS status fout ") + code : vsHttpErrorBody(body, code));
    return false;
  }
  out = VsRemoteStatus();
  out.confirmed = vsStatusConfirmedBody(body);
  const bool haveExpected = vsJsonExtractUInt(body, "expected_chunks", out.expectedChunks);
  vsJsonExtractUInt(body, "received_chunks", out.receivedChunks);
  const bool haveMissing = vsJsonExtractUIntArray(body, "missing_chunks", out.missing);
  out.valid = haveExpected && haveMissing;
  Serial.printf("SERVER: remote status expected=%lu received=%lu missing=%u confirmed=%d\n",
                (unsigned long)out.expectedChunks, (unsigned long)out.receivedChunks,
                (unsigned)out.missing.size(), out.confirmed ? 1 : 0);
  return out.valid || out.confirmed;
}

static bool vsPutChunk(const VsLocalSession& session, const VsChunkMeta& meta) {
  HTTPClient http;
  String url = String(VISITESCRIBE_SERVER_BASE_URL) + "/v1/sessions/" + session.uuid +
               "/chunks/" + String(meta.sequence);
  if (!vsBeginHttp(http, url)) return vsFail("HTTPS chunk start mislukt");
  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("X-Chunk-SHA256", meta.ciphertextSha256);
  http.addHeader("X-Chunk-Nonce", meta.nonceB64);
  http.addHeader("X-Chunk-AAD", meta.aad);
  http.addHeader("X-Plaintext-SHA256", meta.plaintextSha256);
  http.addHeader("Idempotency-Key", session.uuid + ":chunk:" + String(meta.sequence));
  int code = http.sendRequest("PUT", vsCipher, meta.ciphertextBytes);
  String body = (code >= 200 && code < 300) ? String() : (code >= 0 ? http.getString() : String());
  http.end();
  if (code < 200 || code >= 300) {
    return vsFail(code < 0 ? String("HTTPS chunk fout ") + code : vsHttpErrorBody(body, code));
  }
  return true;
}

static bool vsNeedSequence(uint32_t sequence, const std::vector<uint32_t>* missing) {
  if (!missing) return true;
  return std::find(missing->begin(), missing->end(), sequence) != missing->end();
}

static bool vsUploadSpeechChunks(const VsLocalSession& session,
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
      const uint32_t prepStarted = millis();
      if (!vsPrepareSpeechChunk(wav, h, remaining, sessionKey, session.uuid, sequence, meta)) {
        wav.close();
        return vsFail("16k mono chunk voorbereiden mislukt");
      }
      const uint32_t prepMs = millis() - prepStarted;
      vsServerMessage = String("Upload ") + sequence;
      vsDrawServerSync(true);
      const uint32_t httpStarted = millis();
      if (!vsPutChunk(session, meta)) {
        wav.close();
        return false;
      }
      const uint32_t httpMs = millis() - httpStarted;
      uploadedBytes += meta.ciphertextBytes;
      ++uploadedNow;
      const uint32_t kibPerSec = httpMs ? static_cast<uint32_t>((static_cast<uint64_t>(meta.ciphertextBytes) * 1000ULL) / (1024ULL * httpMs)) : 0;
      Serial.printf("SERVER: speech chunk %lu/%lu accepted prep=%lums upload=%lums rate=%lu KiB/s bytes=%u\n",
                    (unsigned long)sequence, (unsigned long)expectedChunks,
                    (unsigned long)prepMs, (unsigned long)httpMs,
                    (unsigned long)kibPerSec, (unsigned)meta.ciphertextBytes);
      delay(1);
    }
    wav.close();
  }

  const uint32_t totalMs = millis() - uploadStarted;
  const uint32_t avgKibPerSec = totalMs ? static_cast<uint32_t>((uploadedBytes * 1000ULL) / (1024ULL * totalMs)) : 0;
  Serial.printf("SERVER: speech upload done expected=%lu uploaded_now=%lu skipped=%lu bytes=%llu total=%lums avg=%lu KiB/s\n",
                (unsigned long)expectedChunks, (unsigned long)uploadedNow,
                (unsigned long)skipped, (unsigned long long)uploadedBytes,
                (unsigned long)totalMs, (unsigned long)avgKibPerSec);
  return sequence == expectedChunks;
}

static bool vsUploadLegacyChunks(const VsLocalSession& session,
                                 const uint8_t sessionKey[32],
                                 uint32_t expectedChunks,
                                 const std::vector<uint32_t>* missing) {
  vsServerStage = VsServerStage::UPLOAD_CHUNKS;
  vsServerChunkTotal = expectedChunks;
  vsServerChunkCurrent = 0;
  uint32_t sequence = 0;
  uint32_t uploadedNow = 0;
  uint32_t skipped = 0;
  Serial.println("SERVER: legacy resume 48k-stereo/10s; alleen ontbrekende chunks worden verstuurd");

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
      const uint32_t prepStarted = millis();
      if (!vsPrepareLegacyChunk(wav, h, remaining, sessionKey, session.uuid, sequence, meta)) {
        wav.close();
        return vsFail("Legacy chunk voorbereiden mislukt");
      }
      const uint32_t prepMs = millis() - prepStarted;
      vsServerMessage = String("Legacy ") + sequence;
      vsDrawServerSync(true);
      const uint32_t httpStarted = millis();
      if (!vsPutChunk(session, meta)) {
        wav.close();
        return false;
      }
      const uint32_t httpMs = millis() - httpStarted;
      ++uploadedNow;
      Serial.printf("SERVER: legacy chunk %lu/%lu accepted prep=%lums upload=%lums\n",
                    (unsigned long)sequence, (unsigned long)expectedChunks,
                    (unsigned long)prepMs, (unsigned long)httpMs);
    }
    wav.close();
  }
  Serial.printf("SERVER: legacy resume done uploaded_now=%lu skipped=%lu\n",
                (unsigned long)uploadedNow, (unsigned long)skipped);
  return sequence == expectedChunks;
}

static bool vsParseCsvEvent(const String& line, uint32_t& offsetMs,
                            String& eventName, int& patientIndex) {
  int c1 = line.indexOf(',');
  if (c1 <= 0) return false;
  int c2 = line.indexOf(',', c1 + 1);
  if (c2 <= c1) return false;
  int c3 = line.indexOf(',', c2 + 1);
  if (c3 <= c2) return false;
  String off = line.substring(0, c1);
  if (off == "elapsed_ms") return false;
  eventName = line.substring(c1 + 1, c2);
  String patient = line.substring(c2 + 1, c3);
  offsetMs = static_cast<uint32_t>(strtoul(off.c_str(), nullptr, 10));
  patientIndex = patient.length() ? patient.toInt() : 0;
  return eventName.length() > 0;
}

static bool vsPostEvents(const VsLocalSession& session) {
  vsSetStage(VsServerStage::EVENTS, session.prefix);
  File f = SD.open(session.eventsPath, FILE_READ);
  if (!f) return vsFail("Events CSV niet te openen");
  String payload = "{\"events\":[";
  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    uint32_t offset = 0;
    String eventName;
    int patient = 0;
    if (!vsParseCsvEvent(line, offset, eventName, patient)) continue;
    if (!first) payload += ',';
    first = false;
    payload += "{\"event\":\"" + eventName + "\",\"offset_ms\":" + String(offset);
    if (patient > 0) payload += ",\"patient_index\":" + String(patient);
    payload += "}";
  }
  f.close();
  payload += "]}";
  HTTPClient http;
  String url = String(VISITESCRIBE_SERVER_BASE_URL) + "/v1/sessions/" + session.uuid + "/events";
  if (!vsBeginHttp(http, url)) return vsFail("HTTPS events start mislukt");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Idempotency-Key", session.uuid + ":events");
  int code = http.POST(reinterpret_cast<uint8_t*>(payload.begin()), payload.length());
  String body = (code >= 200 && code < 300) ? String() : (code >= 0 ? http.getString() : String());
  http.end();
  if (code < 200 || code >= 300) {
    return vsFail(code < 0 ? String("HTTPS events fout ") + code : vsHttpErrorBody(body, code));
  }
  return true;
}

static bool vsComplete(const VsLocalSession& session, uint32_t chunkCount) {
  vsSetStage(VsServerStage::COMPLETE, session.prefix);
  String payload = String("{\"chunk_count\":") + chunkCount + ",\"status\":\"complete\"}";
  HTTPClient http;
  String url = String(VISITESCRIBE_SERVER_BASE_URL) + "/v1/sessions/" + session.uuid + "/complete";
  if (!vsBeginHttp(http, url)) return vsFail("HTTPS complete start mislukt");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Idempotency-Key", session.uuid + ":complete");
  int code = http.POST(reinterpret_cast<uint8_t*>(payload.begin()), payload.length());
  String body = (code >= 200 && code < 300) ? String() : (code >= 0 ? http.getString() : String());
  http.end();
  if (code < 200 || code >= 300) {
    return vsFail(code < 0 ? String("HTTPS complete fout ") + code : vsHttpErrorBody(body, code));
  }
  return true;
}

static bool vsConfirm(const VsLocalSession& session) {
  vsSetStage(VsServerStage::CONFIRM, session.prefix);
  VsRemoteStatus remote;
  if (!vsFetchRemoteStatus(session, remote, true)) return false;
  if (!remote.confirmed) {
    return vsFail(String("Ingest niet bevestigd; ontbrekend=") + remote.missing.size());
  }
  return true;
}

static bool vsFinishLocalSession(const VsLocalSession& session, uint32_t startedMs) {
  if (!vsWriteSyncMeta(session.prefix, session.uuid, "ingested")) {
    return vsFail("Server OK, maar lokale sync-status schrijven faalde");
  }
  ++vsServerSessionsDone;
  Serial.printf("SERVER: session %s complete in %lums\n",
                session.prefix.c_str(), (unsigned long)(millis() - startedMs));
  return true;
}

static bool vsSyncOne(const VsLocalSession& session,
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
  if (!vsWriteSpeechManifest(session, sessionKey, serverKeyId, wrappedKey, manifestPath, speechCount)) {
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
  bool haveRemote = vsFetchRemoteStatus(session, remote, post == VsManifestPost::CONFLICT);
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
        Serial.printf("SERVER: oude sessie gedetecteerd expected=%lu; legacy resume missing=%u\n",
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
      ? vsUploadLegacyChunks(session, sessionKey, activeCount, missing)
      : vsUploadSpeechChunks(session, sessionKey, activeCount, missing);

  if (ok) ok = vsPostEvents(session);
  if (ok) ok = vsComplete(session, activeCount);
  if (ok) ok = vsConfirm(session);
  memset(sessionKey, 0, sizeof(sessionKey));
  if (!ok) return false;
  return vsFinishLocalSession(session, sessionStarted);
}

static bool vsSyncAllPending() {
  if (WiFi.status() != WL_CONNECTED) return vsFail("WiFi niet verbonden");
  if (!sdOk) return vsFail("microSD niet beschikbaar");
  if (!vsEnsureDeviceRootKey()) return vsFail("Device root key niet beschikbaar");
  auto prefixes = vsPendingPrefixes();
  vsServerSessionsTotal = prefixes.size();
  vsServerSessionsDone = 0;
  if (prefixes.empty()) {
    vsSetStage(VsServerStage::NOTHING, "Alle opnames zijn gesynchroniseerd");
    return true;
  }
  String serverKeyId, serverPublicPem;
  if (!vsFetchServerKey(serverKeyId, serverPublicPem)) return false;
  for (const auto& prefix : prefixes) {
    if (WiFi.status() != WL_CONNECTED) return vsFail("WiFi verbinding verloren");
    VsLocalSession local;
    if (!vsLoadLocalSession(prefix, local)) return vsFail(String("Lokale sessie fout: ") + prefix);
    if (!vsSyncOne(local, serverKeyId, serverPublicPem)) return false;
  }
  vsSetStage(VsServerStage::DONE, String(vsServerSessionsDone) + " sessie(s) geupload");
  return true;
}

static void vsServiceServerSync() {
  if (state != AppState::SYNC) {
    if (!vsServerSyncRunning) {
      vsServerStage = VsServerStage::IDLE;
      vsServerSessionPrefix = "";
    }
    return;
  }
  if (syncPhase != SyncPhase::CONNECTED) {
    if (!vsServerSyncRunning &&
        (vsServerStage == VsServerStage::DONE || vsServerStage == VsServerStage::NOTHING ||
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
  Serial.println("SERVER: starting sync queue; WiFi power-save OFF");
  vsSyncAllPending();
  vsReleaseChunkBuffers();
  WiFi.setSleep(true);
  Serial.println("SERVER: sync queue ended; WiFi power-save ON");
  vsServerSyncRunning = false;
}

void setup() {
  setup_v05();
  if (!vsEnsureDeviceRootKey()) Serial.println("SERVER: WARNING device root key unavailable");
  vsTls.setCACert(VISITESCRIBE_SERVER_CA_PEM);
  vsTlsReady = true;
  Serial.printf("VisiteScribe CoreS3-Lite v0.6.6; sync=16k-mono/%lus resume=missing-only server=%s device=%s auth=%s\n",
                (unsigned long)VS_SYNC_CHUNK_SECONDS,
                VISITESCRIBE_SERVER_BASE_URL, VISITESCRIBE_DEVICE_ID,
                strlen(VISITESCRIBE_DEVICE_TOKEN) ? "token" : "device-id");
}

void loop() {
  loop_v05();
  vsServiceServerSync();
  if (state == AppState::SYNC &&
      (syncPhase == SyncPhase::CONNECTED || vsServerStage != VsServerStage::IDLE)) {
    vsDrawServerSync(false);
  }
}
