// VisiteScribe CoreS3-Lite v0.6
//
// Adds real server sync for completed recordings already present on microSD.
// The CoreS3 records PCM WAV locally; during SYNC it splits each recording into
// independently valid 10-second WAV chunks, encrypts each chunk with a
// per-session AES-256-GCM key, wraps that key with the server RSA-OAEP-SHA256
// public key, and uses the canonical /v1 ingest sequence.
//
// Important retry properties:
// - every local session gets a persistent random UUID sidecar on the SD card;
// - the per-session key and per-chunk nonce are deterministically derived from
//   a device root key kept in ESP32 NVS, so an interrupted upload regenerates
//   byte-identical ciphertext and can safely use the server's idempotency;
// - local WAV files are never deleted after upload in this revision.

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
// Existing installations generally already have this device registered.
// A dedicated CoreS3 ID can be supplied locally in server_secrets.h later.
#define VISITESCRIBE_DEVICE_ID "visitescribe-001"
#endif
#ifndef VISITESCRIBE_DEVICE_TOKEN
#define VISITESCRIBE_DEVICE_TOKEN ""
#endif

// Default trust anchor for the production endpoint. server_secrets.h may
// override VISITESCRIBE_SERVER_CA_PEM when the deployment uses another CA.
static const char VS_DEFAULT_SERVER_CA[] PROGMEM = R"VSCA(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)VSCA";

#ifndef VISITESCRIBE_SERVER_CA_PEM
#define VISITESCRIBE_SERVER_CA_PEM VS_DEFAULT_SERVER_CA
#endif

static constexpr uint32_t VS_SYNC_CHUNK_SECONDS = 10;
static constexpr size_t VS_GCM_TAG_BYTES = 16;
static constexpr size_t VS_GCM_NONCE_BYTES = 12;
static constexpr uint32_t VS_HTTP_TIMEOUT_MS = 60000;

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

enum class VsServerStage : uint8_t {
  IDLE,
  FETCH_KEY,
  PREPARE,
  CREATE_SESSION,
  UPLOAD_CHUNKS,
  EVENTS,
  COMPLETE,
  CONFIRM,
  DONE,
  NOTHING,
  ERROR,
};

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
               ((vsServerStage == VsServerStage::DONE || vsServerStage == VsServerStage::NOTHING) ? C_GREEN : C_NAVY),
               2);

  String wifiLine = String("WIFI  ") + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "-");
  centeredText(83, wifiLine.c_str(), C_GREY, 1);

  if (vsServerSessionPrefix.length()) {
    String s = String("SESSIE  ") + vsServerSessionPrefix;
    centeredText(105, s.c_str(), C_NAVY, 1);
  }
  if (vsServerStage == VsServerStage::UPLOAD_CHUNKS && vsServerChunkTotal) {
    char p[48];
    snprintf(p, sizeof(p), "CHUNK %lu / %lu",
             (unsigned long)vsServerChunkCurrent,
             (unsigned long)vsServerChunkTotal);
    centeredText(128, p, C_BLUE, 2);
  } else if (vsServerSessionsTotal) {
    char p[48];
    snprintf(p, sizeof(p), "SESSIES %lu / %lu",
             (unsigned long)vsServerSessionsDone,
             (unsigned long)vsServerSessionsTotal);
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
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
           b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return String(out);
}

static String vsSyncPath(const String& prefix) {
  return String("/visitescribe/") + prefix + "_sync.txt";
}

static bool vsReadSyncMeta(const String& prefix, String& uuid, String& syncState) {
  File f = SD.open(vsSyncPath(prefix), FILE_READ);
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
      if (base.startsWith(prefix + "_") && base.endsWith(".wav")) {
        wavs.push_back(String("/visitescribe/") + base);
      }
    }
    f.close();
  }
  dir.close();
  std::sort(wavs.begin(), wavs.end(), [](const String& a, const String& b) {
    return a.compareTo(b) < 0;
  });
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
  std::sort(prefixes.begin(), prefixes.end(), [](const String& a, const String& b) {
    return a.compareTo(b) < 0;
  });
  prefixes.erase(std::unique(prefixes.begin(), prefixes.end(), [](const String& a, const String& b) {
    return a == b;
  }), prefixes.end());
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

static bool vsEnsureChunkBuffers(size_t plainNeeded) {
  size_t cipherNeeded = plainNeeded + VS_GCM_TAG_BYTES;
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
  Serial.printf("SERVER: allocated sync buffers plain=%u cipher=%u PSRAM free=%u\n",
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

static bool vsChunkNonce(const uint8_t sessionKey[32], const String& uuid,
                         uint32_t sequence, uint8_t nonce[VS_GCM_NONCE_BYTES]) {
  uint8_t full[32];
  String msg = String("nonce:") + uuid + ":" + String(sequence);
  if (!vsHmacSha256(sessionKey, 32, msg, full)) return false;
  memcpy(nonce, full, VS_GCM_NONCE_BYTES);
  return true;
}

static bool vsEncryptChunk(const uint8_t sessionKey[32], const String& uuid,
                           uint32_t sequence, size_t plaintextLen,
                           VsChunkMeta& meta) {
  uint8_t nonce[VS_GCM_NONCE_BYTES];
  if (!vsChunkNonce(sessionKey, uuid, sequence, nonce)) return false;
  meta.sequence = sequence;
  meta.aad = String("visitescribe-v2:") + uuid + ":" + String(sequence);
  meta.nonceB64 = vsBase64(nonce, sizeof(nonce));
  meta.plaintextBytes = plaintextLen;
  meta.plaintextSha256 = vsSha256Hex(vsPlain, plaintextLen);
  if (!meta.nonceB64.length() || !meta.plaintextSha256.length()) return false;

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, sessionKey, 256);
  uint8_t tag[VS_GCM_TAG_BYTES];
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(
        &gcm, MBEDTLS_GCM_ENCRYPT, plaintextLen,
        nonce, sizeof(nonce),
        reinterpret_cast<const unsigned char*>(meta.aad.c_str()), meta.aad.length(),
        vsPlain, vsCipher,
        sizeof(tag), tag);
  }
  mbedtls_gcm_free(&gcm);
  if (rc != 0) return false;
  memcpy(vsCipher + plaintextLen, tag, sizeof(tag));
  meta.ciphertextBytes = plaintextLen + sizeof(tag);
  meta.ciphertextSha256 = vsSha256Hex(vsCipher, meta.ciphertextBytes);
  return meta.ciphertextSha256.length() == 64;
}

static bool vsPrepareNextChunk(File& wav, const WAVHeader& sourceHeader,
                               uint32_t& remaining, const uint8_t sessionKey[32],
                               const String& uuid, uint32_t sequence,
                               VsChunkMeta& meta) {
  const uint32_t wanted = sourceHeader.byteRate * VS_SYNC_CHUNK_SECONDS;
  uint32_t dataBytes = remaining < wanted ? remaining : wanted;
  dataBytes -= dataBytes % sourceHeader.blockAlign;
  if (dataBytes == 0) return false;
  const size_t plainLen = sizeof(WAVHeader) + dataBytes;
  if (!vsEnsureChunkBuffers(sizeof(WAVHeader) + wanted)) return false;

  WAVHeader chunkHeader = sourceHeader;
  chunkHeader.fileSize = 36 + dataBytes;
  chunkHeader.dataSize = dataBytes;
  memcpy(vsPlain, &chunkHeader, sizeof(chunkHeader));
  if (wav.read(vsPlain + sizeof(WAVHeader), dataBytes) != dataBytes) return false;
  remaining -= dataBytes;

  if (!vsEncryptChunk(sessionKey, uuid, sequence, plainLen, meta)) return false;
  meta.durationMs = static_cast<uint32_t>((static_cast<uint64_t>(dataBytes) * 1000ULL) /
                                          sourceHeader.byteRate);
  return true;
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
    } else {
      out += c;
    }
  }
  return false;
}

static void vsAddCommonHeaders(HTTPClient& http) {
  http.addHeader("X-Device-ID", VISITESCRIBE_DEVICE_ID);
  if (strlen(VISITESCRIBE_DEVICE_TOKEN)) {
    http.addHeader("Authorization", String("Bearer ") + VISITESCRIBE_DEVICE_TOKEN);
  }
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

static String vsHttpError(HTTPClient& http, int code) {
  String body = http.getString();
  body.replace("\n", " ");
  if (body.length() > 160) body = body.substring(0, 160);
  return String("HTTP ") + code + " " + body;
}

static bool vsFetchServerKey(String& keyId, String& publicPem) {
  vsSetStage(VsServerStage::FETCH_KEY, "GET /v1/server/public-key");
  HTTPClient http;
  String url = String(VISITESCRIBE_SERVER_BASE_URL) + "/v1/server/public-key";
  if (!vsBeginHttp(http, url)) return vsFail("TLS/HTTP start mislukt");
  int code = http.GET();
  if (code < 200 || code >= 300) {
    String err = code < 0 ? String("HTTPS fout ") + code : vsHttpError(http, code);
    http.end();
    return vsFail(err);
  }
  String body = http.getString();
  http.end();
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

static bool vsWrapSessionKey(const uint8_t sessionKey[32], const String& publicPem,
                             String& wrappedB64) {
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

static bool vsWriteManifest(const VsLocalSession& session,
                            const uint8_t sessionKey[32],
                            const String& serverKeyId,
                            const String& wrappedKeyB64,
                            const String& manifestPath,
                            uint32_t& chunkCount) {
  File manifest = SD.open(manifestPath, FILE_WRITE);
  if (!manifest) return vsFail("Manifest bestand niet te maken");

  WAVHeader firstHeader;
  {
    File first = SD.open(session.wavs.front(), FILE_READ);
    if (!first || !vsReadWavHeader(first, firstHeader)) {
      if (first) first.close();
      manifest.close();
      return vsFail("WAV header ongeldig");
    }
    first.close();
  }

  manifest.printf(
      "{\"schema_version\":2,\"session_id\":\"%s\",\"device_id\":\"%s\","
      "\"mode\":\"%s\",\"status\":\"complete\","
      "\"audio\":{\"codec\":\"wav\",\"sample_rate\":%lu,\"channels\":%u,"
      "\"sample_format\":\"S16_LE\",\"chunk_seconds\":%lu},"
      "\"encryption\":{\"algorithm\":\"AES-256-GCM\",\"local_key_wrap\":null,"
      "\"server_key_wrap\":{\"algorithm\":\"RSA-OAEP-SHA256\",\"key_id\":\"%s\","
      "\"ciphertext_b64\":\"%s\"}},\"chunks\":[",
      session.uuid.c_str(), VISITESCRIBE_DEVICE_ID, session.mode.c_str(),
      (unsigned long)firstHeader.sampleRate, (unsigned)firstHeader.numChannels,
      (unsigned long)VS_SYNC_CHUNK_SECONDS, serverKeyId.c_str(), wrappedKeyB64.c_str());

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
    if (h.sampleRate != firstHeader.sampleRate || h.numChannels != firstHeader.numChannels ||
        h.bitsPerSample != firstHeader.bitsPerSample) {
      wav.close();
      manifest.close();
      return vsFail("WAV parameters verschillen binnen sessie");
    }
    uint32_t remaining = h.dataSize;
    while (remaining) {
      VsChunkMeta meta;
      ++chunkCount;
      if (!vsPrepareNextChunk(wav, h, remaining, sessionKey, session.uuid, chunkCount, meta)) {
        wav.close();
        manifest.close();
        return vsFail("Audio chunk voorbereiden mislukt");
      }
      if (!firstChunk) manifest.print(',');
      firstChunk = false;
      manifest.printf(
          "{\"sequence\":%lu,\"file\":\"audio/chunk-%06lu.wav.enc\","
          "\"nonce_b64\":\"%s\",\"aad\":\"%s\","
          "\"plaintext_sha256\":\"%s\",\"ciphertext_sha256\":\"%s\","
          "\"plaintext_size\":%u,\"ciphertext_size\":%u,"
          "\"start_offset_ms\":%llu,\"duration_ms\":%lu}",
          (unsigned long)meta.sequence, (unsigned long)meta.sequence,
          meta.nonceB64.c_str(), meta.aad.c_str(),
          meta.plaintextSha256.c_str(), meta.ciphertextSha256.c_str(),
          (unsigned)meta.plaintextBytes, (unsigned)meta.ciphertextBytes,
          (unsigned long long)audioOffsetMs, (unsigned long)meta.durationMs);
      audioOffsetMs += meta.durationMs;
      if ((chunkCount % 8) == 0) {
        manifest.flush();
        vsServerMessage = String("Manifest chunk ") + chunkCount;
        vsDrawServerSync(true);
      }
    }
    wav.close();
  }
  manifest.print("]}");
  manifest.flush();
  manifest.close();
  return chunkCount > 0;
}

static bool vsPostManifest(const VsLocalSession& session, const String& manifestPath) {
  vsSetStage(VsServerStage::CREATE_SESSION, session.prefix);
  File manifest = SD.open(manifestPath, FILE_READ);
  if (!manifest) return vsFail("Manifest niet te openen");
  HTTPClient http;
  String url = String(VISITESCRIBE_SERVER_BASE_URL) + "/v1/sessions";
  if (!vsBeginHttp(http, url)) {
    manifest.close();
    return vsFail("HTTPS sessie start mislukt");
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Idempotency-Key", session.uuid + ":create");
  int code = http.sendRequest("POST", &manifest, manifest.size());
  manifest.close();
  if (code < 200 || code >= 300) {
    String err = code < 0 ? String("HTTPS create fout ") + code : vsHttpError(http, code);
    http.end();
    return vsFail(err);
  }
  Serial.printf("SERVER: create %s -> HTTP %d\n", session.uuid.c_str(), code);
  http.end();
  return true;
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
  if (code < 200 || code >= 300) {
    String err = code < 0 ? String("HTTPS chunk fout ") + code : vsHttpError(http, code);
    http.end();
    return vsFail(err);
  }
  http.end();
  return true;
}

static bool vsUploadChunks(const VsLocalSession& session, const uint8_t sessionKey[32],
                           uint32_t expectedChunks) {
  vsServerStage = VsServerStage::UPLOAD_CHUNKS;
  vsServerChunkTotal = expectedChunks;
  vsServerChunkCurrent = 0;
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
      VsChunkMeta meta;
      ++sequence;
      if (!vsPrepareNextChunk(wav, h, remaining, sessionKey, session.uuid, sequence, meta)) {
        wav.close();
        return vsFail("Chunk opnieuw opbouwen mislukt");
      }
      vsServerChunkCurrent = sequence;
      vsServerMessage = String("Encrypt + upload ") + sequence;
      vsDrawServerSync(true);
      if (!vsPutChunk(session, meta)) {
        wav.close();
        return false;
      }
      Serial.printf("SERVER: chunk %lu/%lu accepted\n",
                    (unsigned long)sequence, (unsigned long)expectedChunks);
      delay(1);
    }
    wav.close();
  }
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
  if (code < 200 || code >= 300) {
    String err = code < 0 ? String("HTTPS events fout ") + code : vsHttpError(http, code);
    http.end();
    return vsFail(err);
  }
  http.end();
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
  if (code < 200 || code >= 300) {
    String err = code < 0 ? String("HTTPS complete fout ") + code : vsHttpError(http, code);
    http.end();
    return vsFail(err);
  }
  http.end();
  return true;
}

static bool vsStatusConfirmed(const String& body) {
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

static bool vsConfirm(const VsLocalSession& session) {
  vsSetStage(VsServerStage::CONFIRM, session.prefix);
  HTTPClient http;
  String url = String(VISITESCRIBE_SERVER_BASE_URL) + "/v1/sessions/" + session.uuid + "/status";
  if (!vsBeginHttp(http, url)) return vsFail("HTTPS status start mislukt");
  int code = http.GET();
  if (code < 200 || code >= 300) {
    String err = code < 0 ? String("HTTPS status fout ") + code : vsHttpError(http, code);
    http.end();
    return vsFail(err);
  }
  String body = http.getString();
  http.end();
  if (!vsStatusConfirmed(body)) {
    body.replace("\n", " ");
    if (body.length() > 120) body = body.substring(0, 120);
    return vsFail(String("Ingest niet bevestigd: ") + body);
  }
  return true;
}

static bool vsSyncOne(const VsLocalSession& session,
                      const String& serverKeyId, const String& serverPublicPem) {
  vsServerSessionPrefix = session.prefix;
  vsServerChunkCurrent = vsServerChunkTotal = 0;
  vsSetStage(VsServerStage::PREPARE, session.prefix);

  uint8_t sessionKey[32];
  if (!vsSessionKey(session.uuid, sessionKey)) return vsFail("Sessiesleutel maken mislukt");
  String wrappedKey;
  if (!vsWrapSessionKey(sessionKey, serverPublicPem, wrappedKey)) return vsFail("RSA key wrap mislukt");

  String manifestPath = String("/visitescribe/") + session.prefix + "_upload_manifest.json";
  if (SD.exists(manifestPath)) SD.remove(manifestPath);
  uint32_t chunkCount = 0;
  if (!vsWriteManifest(session, sessionKey, serverKeyId, wrappedKey, manifestPath, chunkCount)) {
    SD.remove(manifestPath);
    return false;
  }
  vsServerChunkTotal = chunkCount;

  bool ok = vsPostManifest(session, manifestPath) &&
            vsUploadChunks(session, sessionKey, chunkCount) &&
            vsPostEvents(session) &&
            vsComplete(session, chunkCount) &&
            vsConfirm(session);
  SD.remove(manifestPath);
  memset(sessionKey, 0, sizeof(sessionKey));
  if (!ok) return false;

  if (!vsWriteSyncMeta(session.prefix, session.uuid, "ingested")) {
    return vsFail("Server OK, maar lokale sync-status schrijven faalde");
  }
  ++vsServerSessionsDone;
  return true;
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

  // A manual OPNIEUW sends the inherited Wi-Fi state back to CONNECTING.
  // Arm a fresh server attempt as soon as connectivity returns.
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
  Serial.println("SERVER: starting sync queue");
  vsSyncAllPending();
  vsReleaseChunkBuffers();
  vsServerSyncRunning = false;
}

void setup() {
  setup_v05();
  if (!vsEnsureDeviceRootKey()) {
    Serial.println("SERVER: WARNING device root key unavailable");
  }
  vsTls.setCACert(VISITESCRIBE_SERVER_CA_PEM);
  vsTlsReady = true;
  Serial.printf("VisiteScribe CoreS3-Lite v0.6; server=%s device=%s auth=%s\n",
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
