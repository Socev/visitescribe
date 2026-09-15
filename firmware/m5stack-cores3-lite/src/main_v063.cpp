// VisiteScribe CoreS3-Lite v0.6.3
//
// v0.6.2 reaches the server successfully, but the first WAV chunk can fail
// during local preparation. A 48 kHz / 16-bit / stereo / 10 s chunk needs
// about 1.92 MB plaintext plus 1.92 MB ciphertext. Reserve those two PSRAM
// blocks during Arduino init(), before Wi-Fi/TLS allocations can fragment the
// external RAM. The normal v0.6 sync code then reuses these buffers.
//
// This file changes recorder-side memory timing only; the ingest protocol and
// server API are unchanged.

#include "main_v062.cpp"

static bool vs063EarlyBuffersOk = false;
static size_t vs063PlainNeed = 0;
static size_t vs063CipherNeed = 0;

// Arduino-ESP32 provides a weak, empty init(). initArduino() calls it after
// PSRAM has been initialised/added to the heap and before initVariant()/setup().
// Providing this strong implementation lets us reserve the large contiguous
// sync buffers at the earliest useful point without wrapping setup()/loop().
void init() {
  const size_t audioBytes =
      static_cast<size_t>(AUDIO_RATE) * AUDIO_CHANNELS * (AUDIO_BITS / 8) *
      VS_SYNC_CHUNK_SECONDS;
  vs063PlainNeed = sizeof(WAVHeader) + audioBytes;
  vs063CipherNeed = vs063PlainNeed + VS_GCM_TAG_BYTES;

  vsPlain = static_cast<uint8_t*>(
      heap_caps_malloc(vs063PlainNeed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  vsCipher = static_cast<uint8_t*>(
      heap_caps_malloc(vs063CipherNeed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (vsPlain && vsCipher) {
    vsPlainCapacity = vs063PlainNeed;
    vsCipherCapacity = vs063CipherNeed;
    vs063EarlyBuffersOk = true;
  } else {
    if (vsPlain) free(vsPlain);
    if (vsCipher) free(vsCipher);
    vsPlain = vsCipher = nullptr;
    vsPlainCapacity = vsCipherCapacity = 0;
  }
}

// Arduino's main loop calls this weak hook after setup()/loop(). Use it once to
// report the real PSRAM situation after Serial/USB is fully available.
void serialEventRun() {
  static bool printed = false;
  if (printed) return;
  printed = true;
  Serial.printf(
      "SERVER: v0.6.3 early sync buffers=%s plain=%u cipher=%u psram_total=%u psram_free=%u psram_largest=%u\n",
      vs063EarlyBuffersOk ? "OK" : "FAIL",
      (unsigned)vs063PlainNeed,
      (unsigned)vs063CipherNeed,
      (unsigned)ESP.getPsramSize(),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
