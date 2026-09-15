// VisiteScribe CoreS3-Lite v0.6.4
//
// Recorder-side memory fix for server sync.
//
// A 10 s 48 kHz / 16-bit / stereo WAV chunk is about 1.92 MB. v0.6 used
// separate plaintext and ciphertext PSRAM buffers, so chunk preparation needed
// two large contiguous allocations (~3.84 MB). On the CoreS3-Lite that can fail
// after the display, SD and TLS stacks have allocated memory.
//
// AES-GCM encryption on ESP32 supports in-place encryption (input == output).
// Reserve one 2 MiB PSRAM buffer early and let the existing v0.6 code use that
// same buffer as both vsPlain and vsCipher. The 16-byte GCM tag is appended in
// the spare capacity. This keeps the existing 10-second chunking and wire/API
// protocol unchanged.
//
// Only recorder firmware is changed here. No server/API code is touched.

#include <Arduino.h>
#include <M5Unified.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>

static constexpr size_t VS064_SHARED_SYNC_BYTES = 2U * 1024U * 1024U;
static uint8_t* vs064SharedSyncBuffer = nullptr;
static bool vs064SharedReady = false;

// Allocate before setup(), immediately after Arduino has initialised PSRAM.
void init() {
  vs064SharedSyncBuffer = static_cast<uint8_t*>(
      heap_caps_malloc(VS064_SHARED_SYNC_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  vs064SharedReady = (vs064SharedSyncBuffer != nullptr);
}

static void* vs064HeapCapsMalloc(size_t size, uint32_t caps) {
  // main_v06 only uses explicit SPIRAM heap_caps_malloc() for the sync chunk
  // buffers. Return the same reserved block for plaintext and ciphertext so
  // mbedTLS performs AES-GCM encryption in place.
  if (vs064SharedReady &&
      (caps & MALLOC_CAP_SPIRAM) &&
      size <= VS064_SHARED_SYNC_BYTES) {
    return vs064SharedSyncBuffer;
  }
  return heap_caps_malloc(size, caps);
}

static void vs064Free(void* ptr) {
  // The inherited release path frees vsPlain and vsCipher separately. They are
  // aliases in v0.6.4, so keep the reserved block alive for retries and skip
  // both frees. All unrelated allocations retain normal free() behaviour.
  if (!ptr || ptr == vs064SharedSyncBuffer) return;
  free(ptr);
}

// The relevant headers are already included above, so these macros only affect
// explicit allocations/frees in the inherited recorder source, not library
// declarations. All networking, crypto metadata and API calls remain v0.6.2.
#define heap_caps_malloc(size, caps) vs064HeapCapsMalloc((size), (caps))
#define free(ptr) vs064Free((ptr))
#include "main_v062.cpp"
#undef free
#undef heap_caps_malloc

static void vs064ListVisiteScribeDir() {
  Serial.println("SERVER DEBUG: === /visitescribe ===");
  File dir = SD.open("/visitescribe");
  if (!dir) {
    Serial.println("SERVER DEBUG: /visitescribe openen mislukt");
    return;
  }

  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    Serial.printf("SERVER DEBUG: %s size=%u dir=%d\n",
                  f.name(),
                  (unsigned)f.size(),
                  f.isDirectory() ? 1 : 0);
    f.close();
  }
  dir.close();
  Serial.println("SERVER DEBUG: =====================");
}

static void vs064InspectWavRead(const String& path) {
  File wav = SD.open(path, FILE_READ);
  if (!wav) {
    Serial.printf("SERVER DEBUG: WAV open mislukt path=%s\n", path.c_str());
    return;
  }

  const size_t physicalBytes = wav.size();
  WAVHeader h;
  if (!vsReadWavHeader(wav, h)) {
    Serial.printf("SERVER DEBUG: WAV header ongeldig path=%s physical=%u\n",
                  path.c_str(), (unsigned)physicalBytes);
    wav.close();
    return;
  }

  const size_t payloadPos = wav.position();
  const uint32_t wanted = h.byteRate * VS_SYNC_CHUNK_SECONDS;
  uint32_t dataBytes = h.dataSize < wanted ? h.dataSize : wanted;
  if (h.blockAlign) dataBytes -= dataBytes % h.blockAlign;
  const size_t plainNeeded = sizeof(WAVHeader) + wanted;
  const size_t firstChunkBytes = sizeof(WAVHeader) + dataBytes;
  const size_t physicalPayload = physicalBytes > payloadPos ? physicalBytes - payloadPos : 0;

  Serial.printf(
      "SERVER DEBUG: WAV path=%s physical=%u headerData=%u payloadPhysical=%u payloadPos=%u rate=%u channels=%u bits=%u byteRate=%u blockAlign=%u\n",
      path.c_str(),
      (unsigned)physicalBytes,
      (unsigned)h.dataSize,
      (unsigned)physicalPayload,
      (unsigned)payloadPos,
      (unsigned)h.sampleRate,
      (unsigned)h.numChannels,
      (unsigned)h.bitsPerSample,
      (unsigned)h.byteRate,
      (unsigned)h.blockAlign);
  Serial.printf(
      "SERVER DEBUG: chunk wantedData=%u dataBytes=%u plainNeeded=%u firstChunk=%u sharedReady=%d sharedBytes=%u\n",
      (unsigned)wanted,
      (unsigned)dataBytes,
      (unsigned)plainNeeded,
      (unsigned)firstChunkBytes,
      vs064SharedReady ? 1 : 0,
      (unsigned)VS064_SHARED_SYNC_BYTES);

  if (!vs064SharedReady || !vs064SharedSyncBuffer) {
    Serial.println("SERVER DEBUG: geen shared PSRAM-buffer voor read-probe");
    wav.close();
    return;
  }
  if (firstChunkBytes > VS064_SHARED_SYNC_BYTES) {
    Serial.println("SERVER DEBUG: eerste chunk past niet in shared PSRAM-buffer");
    wav.close();
    return;
  }
  if (dataBytes == 0) {
    Serial.println("SERVER DEBUG: dataBytes=0 na alignment");
    wav.close();
    return;
  }

  size_t gotSingle = wav.read(vs064SharedSyncBuffer + sizeof(WAVHeader), dataBytes);
  Serial.printf("SERVER DEBUG: single-read requested=%u got=%u remainingAvailable=%u\n",
                (unsigned)dataBytes,
                (unsigned)gotSingle,
                (unsigned)wav.available());

  if (!wav.seek(payloadPos)) {
    Serial.println("SERVER DEBUG: seek terug naar WAV payload mislukt");
    wav.close();
    return;
  }

  static constexpr size_t PROBE_READ_BYTES = 64U * 1024U;
  size_t total = 0;
  size_t reads = 0;
  while (total < dataBytes) {
    size_t request = dataBytes - total;
    if (request > PROBE_READ_BYTES) request = PROBE_READ_BYTES;
    size_t got = wav.read(vs064SharedSyncBuffer + sizeof(WAVHeader) + total, request);
    ++reads;
    total += got;
    if (got == 0) break;
  }
  Serial.printf("SERVER DEBUG: chunked-read requested=%u got=%u reads=%u remainingAvailable=%u\n",
                (unsigned)dataBytes,
                (unsigned)total,
                (unsigned)reads,
                (unsigned)wav.available());
  wav.close();
}

static void vs064DumpPrepareFailure() {
  Serial.printf(
      "SERVER DEBUG: PREPARE failure session=%s error=%s psram_total=%u psram_free=%u psram_largest=%u shared=%p ready=%d\n",
      vsServerSessionPrefix.c_str(),
      vsServerError.c_str(),
      (unsigned)ESP.getPsramSize(),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
      (void*)vs064SharedSyncBuffer,
      vs064SharedReady ? 1 : 0);

  vs064ListVisiteScribeDir();

  if (!vsServerSessionPrefix.length()) {
    Serial.println("SERVER DEBUG: geen actuele sessieprefix");
    return;
  }

  auto wavs = vsCollectWavs(vsServerSessionPrefix);
  Serial.printf("SERVER DEBUG: sessie %s heeft %u WAV(s)\n",
                vsServerSessionPrefix.c_str(), (unsigned)wavs.size());
  for (const auto& path : wavs) {
    vs064InspectWavRead(path);
  }
  Serial.println("SERVER DEBUG: einde PREPARE failure dump");
}

void serialEventRun() {
  static bool bannerPrinted = false;
  static bool prepareFailureDumped = false;

  if (!bannerPrinted) {
    bannerPrinted = true;
    Serial.printf(
        "SERVER: v0.6.4 shared in-place sync buffer=%s bytes=%u psram_total=%u psram_free=%u psram_largest=%u\n",
        vs064SharedReady ? "OK" : "FAIL",
        (unsigned)VS064_SHARED_SYNC_BYTES,
        (unsigned)ESP.getPsramSize(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  }

  const bool prepareError =
      vsServerStage == VsServerStage::ERROR &&
      (vsServerError.indexOf("Audio chunk voorbereiden mislukt") >= 0 ||
       vsServerError.indexOf("Chunk opnieuw opbouwen mislukt") >= 0);

  if (prepareError && !prepareFailureDumped) {
    prepareFailureDumped = true;
    vs064DumpPrepareFailure();
  } else if (!prepareError) {
    prepareFailureDumped = false;
  }
}
