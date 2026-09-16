// VisiteScribe CoreS3-Lite v0.7.0 experimental Opus encoder benchmark.
//
// This file is included ONLY when VISITESCRIBE_OPUS_EXPERIMENT is enabled by
// the dedicated PlatformIO environment. It does not alter recording, sync,
// crypto or server formats. Its only job is to prove encoder speed and output
// size on the exact CoreS3-Lite hardware before Opus becomes a production wire
// format.
//
// IMPORTANT: esp32_opus is built with USE_ALLOCA. Opus therefore needs far
// more stack than Arduino's normal loop task provides. The upstream ESP32
// example also runs the codec in a 32000-byte FreeRTOS task. The benchmark does
// the same instead of calling opus_encode() from loop().

#ifdef VISITESCRIBE_OPUS_EXPERIMENT

#include <opus.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>

static constexpr int VS070_RATE = 16000;
static constexpr int VS070_CHANNELS = 1;
static constexpr int VS070_FRAME_MS = 20;
static constexpr int VS070_FRAME_SAMPLES = VS070_RATE * VS070_FRAME_MS / 1000; // 320
static constexpr int VS070_BITRATE = 24000;
static constexpr int VS070_COMPLEXITY = 1;
static constexpr int VS070_SECONDS = 300;             // 5 minutes
static constexpr int VS070_BLOCK_SECONDS = 30;
static constexpr int VS070_FRAMES_PER_BLOCK = VS070_BLOCK_SECONDS * 1000 / VS070_FRAME_MS;
static constexpr int VS070_MAX_PACKET = 512;
static constexpr uint32_t VS070_TASK_STACK_BYTES = 32000;

static volatile bool vs070TaskRunning = false;
static volatile bool vs070TaskDone = false;
static volatile bool vs070TaskSuccess = false;
static volatile int vs070ProgressBlock = 0;

static void vs070DrawProgress(int block, int totalBlocks, const char* detail) {
  drawHeader("OPUS TEST");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, 150, C_BG);
  centeredText(66, "16 kHz mono / 24 kbps", C_NAVY, 1);
  centeredText(90, "20 ms / VOIP / CBR", C_GREY, 1);
  char line[48];
  snprintf(line, sizeof(line), "BLOK %d / %d", block, totalBlocks);
  centeredText(122, line, C_BLUE, 2);
  if (detail && detail[0]) centeredText(154, detail, C_GREY, 1);
  zone(SYNC_BACK, "TERUG", C_NAVY, C_WHITE);
}

static inline int16_t vs070Clamp16(int32_t v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return static_cast<int16_t>(v);
}

static void vs070GenerateSpeechLikeFrame(int16_t* pcm, uint32_t& phaseA,
                                         uint32_t& phaseB, uint32_t& noise,
                                         uint32_t frameIndex) {
  const uint32_t envelopeStep = (frameIndex / 25U) % 10U;
  const int32_t envelope = 5000 + static_cast<int32_t>(envelopeStep) * 900;
  for (int i = 0; i < VS070_FRAME_SAMPLES; ++i) {
    phaseA += 5905580U;
    phaseB += 9126805U;
    int32_t sawA = static_cast<int32_t>(phaseA >> 16) - 32768;
    int32_t sawB = static_cast<int32_t>(phaseB >> 16) - 32768;
    noise ^= noise << 13;
    noise ^= noise >> 17;
    noise ^= noise << 5;
    int32_t n = static_cast<int16_t>(noise & 0xFFFFU);
    int32_t sample = ((sawA * envelope) >> 15) +
                     ((sawB * (envelope / 3)) >> 15) +
                     (n >> 5);
    pcm[i] = vs070Clamp16(sample);
  }
}

static bool vs070RunBenchmark() {
  const size_t encoderBytes = static_cast<size_t>(opus_encoder_get_size(VS070_CHANNELS));
  Serial.printf(
      "OPUS TEST: worker entered stack_hwm=%u encoder_state=%uB internal_free=%u "
      "internal_largest=%u psram_free=%u\n",
      (unsigned)uxTaskGetStackHighWaterMark(nullptr),
      (unsigned)encoderBytes,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.flush();

  int error = OPUS_OK;
  Serial.println("OPUS TEST: creating encoder...");
  Serial.flush();
  OpusEncoder* encoder = opus_encoder_create(
      VS070_RATE, VS070_CHANNELS, OPUS_APPLICATION_VOIP, &error);
  if (!encoder || error != OPUS_OK) {
    Serial.printf("OPUS TEST: encoder create FAILED error=%d %s internal_free=%u\n",
                  error, opus_strerror(error),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (encoder) opus_encoder_destroy(encoder);
    return false;
  }
  Serial.printf("OPUS TEST: encoder created stack_hwm=%u internal_free=%u\n",
                (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  Serial.flush();

  int ctlError = OPUS_OK;
  ctlError = opus_encoder_ctl(encoder, OPUS_SET_BITRATE(VS070_BITRATE));
  if (ctlError == OPUS_OK) ctlError = opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(VS070_COMPLEXITY));
  if (ctlError == OPUS_OK) ctlError = opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  if (ctlError == OPUS_OK) ctlError = opus_encoder_ctl(encoder, OPUS_SET_VBR(0));
  if (ctlError == OPUS_OK) ctlError = opus_encoder_ctl(encoder, OPUS_SET_DTX(0));
  if (ctlError == OPUS_OK) ctlError = opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(0));
  if (ctlError != OPUS_OK) {
    Serial.printf("OPUS TEST: encoder ctl FAILED error=%d %s\n",
                  ctlError, opus_strerror(ctlError));
    opus_encoder_destroy(encoder);
    return false;
  }

  static int16_t pcm[VS070_FRAME_SAMPLES];
  static uint8_t packet[VS070_MAX_PACKET];
  uint32_t phaseA = 0;
  uint32_t phaseB = 0x12345678U;
  uint32_t noise = 0xA5C39E17U;

  uint64_t totalBytes = 0;
  uint64_t totalEncodeUs = 0;
  uint64_t totalGenerateUs = 0;
  uint32_t minPacket = UINT32_MAX;
  uint32_t maxPacket = 0;
  int completedFrames = 0;
  const int totalBlocks = VS070_SECONDS / VS070_BLOCK_SECONDS;
  const uint32_t wallStarted = millis();

  for (int block = 0; block < totalBlocks; ++block) {
    vs070ProgressBlock = block + 1;
    uint64_t blockBytes = 0;
    uint64_t blockEncodeUs = 0;
    uint64_t blockGenerateUs = 0;

    for (int f = 0; f < VS070_FRAMES_PER_BLOCK; ++f) {
      const uint32_t frameIndex = static_cast<uint32_t>(block * VS070_FRAMES_PER_BLOCK + f);
      uint32_t us = micros();
      vs070GenerateSpeechLikeFrame(pcm, phaseA, phaseB, noise, frameIndex);
      const uint32_t genUs = micros() - us;
      blockGenerateUs += genUs;
      totalGenerateUs += genUs;

      us = micros();
      const int bytes = opus_encode(
          encoder, pcm, VS070_FRAME_SAMPLES, packet, sizeof(packet));
      const uint32_t encUs = micros() - us;
      if (bytes < 0) {
        Serial.printf("OPUS TEST: encode FAILED frame=%lu error=%d %s stack_hwm=%u\n",
                      (unsigned long)frameIndex, bytes, opus_strerror(bytes),
                      (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        opus_encoder_destroy(encoder);
        return false;
      }

      blockEncodeUs += encUs;
      totalEncodeUs += encUs;
      blockBytes += static_cast<uint32_t>(bytes);
      totalBytes += static_cast<uint32_t>(bytes);
      if (static_cast<uint32_t>(bytes) < minPacket) minPacket = static_cast<uint32_t>(bytes);
      if (static_cast<uint32_t>(bytes) > maxPacket) maxPacket = static_cast<uint32_t>(bytes);
      ++completedFrames;
      if ((f & 127) == 0) taskYIELD();
    }

    const double blockEncodeMs = static_cast<double>(blockEncodeUs) / 1000.0;
    const double realtime = blockEncodeMs > 0.0
        ? (VS070_BLOCK_SECONDS * 1000.0) / blockEncodeMs : 0.0;
    const double kbps = (static_cast<double>(blockBytes) * 8.0) /
                        (VS070_BLOCK_SECONDS * 1000.0);
    Serial.printf(
        "OPUS TEST: block %d/%d audio=%ds bytes=%llu kbps=%.2f "
        "encode=%.1fms gen=%.1fms realtime=%.2fx stack_hwm=%u\n",
        block + 1, totalBlocks, VS070_BLOCK_SECONDS,
        (unsigned long long)blockBytes, kbps,
        blockEncodeMs, static_cast<double>(blockGenerateUs) / 1000.0,
        realtime, (unsigned)uxTaskGetStackHighWaterMark(nullptr));

    if (blockEncodeUs > static_cast<uint64_t>(VS070_BLOCK_SECONDS) * 1000000ULL) {
      Serial.println("OPUS TEST: STOP early; encoder is slower than realtime");
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  const uint32_t wallMs = millis() - wallStarted;
  const double audioSeconds = completedFrames * (VS070_FRAME_MS / 1000.0);
  const double encodeMs = static_cast<double>(totalEncodeUs) / 1000.0;
  const double realtime = encodeMs > 0.0 ? (audioSeconds * 1000.0) / encodeMs : 0.0;
  const double kbps = audioSeconds > 0.0
      ? (static_cast<double>(totalBytes) * 8.0) / (audioSeconds * 1000.0) : 0.0;
  Serial.printf(
      "OPUS TEST: DONE audio=%.1fs frames=%d bytes=%llu kbps=%.2f "
      "encode=%.1fms gen=%.1fms wall=%lums realtime=%.2fx packet=%lu..%lu "
      "stack_hwm=%u\n",
      audioSeconds, completedFrames, (unsigned long long)totalBytes, kbps,
      encodeMs, static_cast<double>(totalGenerateUs) / 1000.0,
      (unsigned long)wallMs, realtime,
      (unsigned long)(minPacket == UINT32_MAX ? 0 : minPacket),
      (unsigned long)maxPacket,
      (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  opus_encoder_destroy(encoder);
  return true;
}

static void vs070Worker(void*) {
  vs070TaskSuccess = vs070RunBenchmark();
  vs070TaskRunning = false;
  vs070TaskDone = true;
  Serial.printf("OPUS TEST: worker exit success=%d stack_hwm=%u\n",
                vs070TaskSuccess ? 1 : 0,
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  Serial.flush();
  vTaskDelete(nullptr);
}

static void vs070MenuOpusTest() {
  noteActivity();
  if (vs070TaskRunning) {
    Serial.println("OPUS TEST: already running");
    return;
  }

  Serial.printf(
      "OPUS TEST: launch from loop stack_hwm=%u; worker_stack=%uB "
      "internal_free=%u internal_largest=%u psram_free=%u\n",
      (unsigned)uxTaskGetStackHighWaterMark(nullptr),
      (unsigned)VS070_TASK_STACK_BYTES,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.printf("OPUS TEST: config rate=%d channels=%d frame=%dms bitrate=%d complexity=%d cbr=1\n",
                VS070_RATE, VS070_CHANNELS, VS070_FRAME_MS,
                VS070_BITRATE, VS070_COMPLEXITY);
  Serial.flush();

  vs070TaskDone = false;
  vs070TaskSuccess = false;
  vs070TaskRunning = true;
  vs070ProgressBlock = 0;

  BaseType_t created = xTaskCreate(
      vs070Worker, "opus_probe", VS070_TASK_STACK_BYTES,
      nullptr, 2, nullptr);
  if (created != pdPASS) {
    vs070TaskRunning = false;
    vs070TaskDone = true;
    Serial.printf("OPUS TEST: task create FAILED internal_free=%u internal_largest=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    Serial.flush();
    screenDirty = true;
    return;
  }

  const int totalBlocks = VS070_SECONDS / VS070_BLOCK_SECONDS;
  int lastBlock = -1;
  while (!vs070TaskDone) {
    const int block = vs070ProgressBlock;
    if (block != lastBlock) {
      lastBlock = block;
      vs070DrawProgress(block > 0 ? block : 1, totalBlocks,
                        block > 0 ? "codec benchmark loopt" : "encoder starten...");
    }
    M5.update();
    delay(20);
  }

  vs070DrawProgress(vs070ProgressBlock > 0 ? vs070ProgressBlock : 1,
                    totalBlocks,
                    vs070TaskSuccess ? "klaar - zie serial" : "fout - zie serial");
  delay(300);
  screenDirty = true;
}

struct Vs070HookInstaller {
  Vs070HookInstaller() { vsSyntheticTestHook = &vs070MenuOpusTest; }
};
static Vs070HookInstaller vs070HookInstaller;

#endif  // VISITESCRIBE_OPUS_EXPERIMENT
