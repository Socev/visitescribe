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

void serialEventRun() {
  static bool printed = false;
  if (printed) return;
  printed = true;
  Serial.printf(
      "SERVER: v0.6.4 shared in-place sync buffer=%s bytes=%u psram_total=%u psram_free=%u psram_largest=%u\n",
      vs064SharedReady ? "OK" : "FAIL",
      (unsigned)VS064_SHARED_SYNC_BYTES,
      (unsigned)ESP.getPsramSize(),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
