// VisiteScribe CoreS3-Lite v0.7.1 experimental live Opus load probe.
//
// This file is included ONLY in the dedicated experimental Arduino build.
// It does NOT encode real patient audio and writes no Opus files. Instead it
// generates one synthetic 16 kHz mono / 20 ms frame every 20 ms while the real
// 48 kHz stereo recorder is capturing. That reproduces the CPU load of live
// Opus encoding without changing, replacing, deleting or delaying the master
// WAV path.
//
// Goal: prove that recorder + SD + UI + Espressif Opus CPU load can coexist on
// the real production Arduino/ESP-IDF stack before any sidecar path is added.
//
// Safety rule for this experiment: microphone capture always outranks Opus.
// M5Unified defaults the mic task to priority 2 and no pinned core; the first
// version of this probe also used priority 2 on Core 0 and the SD master failed.
// This revision explicitly pins the mic task to Core 0 at priority 4 and runs
// Opus on the same core at priority 1, so capture preempts codec work. The load
// probe also disables itself immediately on audioError or stalled WAV progress.

#ifdef VISITESCRIBE_OPUS_ESP_LOAD_EXPERIMENT

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include "esp_audio_types.h"
#include "esp_opus_enc.h"

static constexpr int VS071_RATE = 16000;
static constexpr int VS071_CHANNELS = 1;
static constexpr int VS071_FRAME_MS = 20;
static constexpr int VS071_FRAME_SAMPLES = VS071_RATE * VS071_FRAME_MS / 1000; // 320
static constexpr int VS071_BITRATE = 24000;
static constexpr int VS071_COMPLEXITY = 0;
static constexpr uint32_t VS071_TASK_STACK_BYTES = 32768;
static constexpr BaseType_t VS071_TASK_CORE = 0;
static constexpr UBaseType_t VS071_TASK_PRIORITY = 1;
static constexpr BaseType_t VS071_MIC_CORE = 0;
static constexpr uint8_t VS071_MIC_PRIORITY = 4;
static constexpr uint32_t VS071_LOG_MS = 10000;
static constexpr uint32_t VS071_WAV_STALL_ABORT_MS = 1000;

static volatile bool vs071Enabled = false;
static volatile bool vs071TaskRunning = false;
static volatile bool vs071TaskReady = false;
static volatile bool vs071CodecOk = false;
static volatile uint32_t vs071TotalFrames = 0;
static volatile uint32_t vs071MissedDeadlines = 0;
static TaskHandle_t vs071TaskHandle = nullptr;

static inline int16_t vs071Clamp16(int32_t v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return static_cast<int16_t>(v);
}

static void vs071GenerateFrame(int16_t* pcm, uint32_t& phaseA,
                               uint32_t& phaseB, uint32_t& noise,
                               uint32_t frameIndex) {
  const uint32_t envelopeStep = (frameIndex / 25U) % 10U;
  const int32_t envelope = 5000 + static_cast<int32_t>(envelopeStep) * 900;
  for (int i = 0; i < VS071_FRAME_SAMPLES; ++i) {
    phaseA += 5905580U;
    phaseB += 9126805U;
    const int32_t sawA = static_cast<int32_t>(phaseA >> 16) - 32768;
    const int32_t sawB = static_cast<int32_t>(phaseB >> 16) - 32768;
    noise ^= noise << 13;
    noise ^= noise >> 17;
    noise ^= noise << 5;
    const int32_t n = static_cast<int16_t>(noise & 0xFFFFU);
    const int32_t sample = ((sawA * envelope) >> 15) +
                           ((sawB * (envelope / 3)) >> 15) +
                           (n >> 5);
    pcm[i] = vs071Clamp16(sample);
  }
}

static void vs071Worker(void*) {
  vs071TaskRunning = true;

  esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
  cfg.sample_rate = VS071_RATE;
  cfg.channel = VS071_CHANNELS;
  cfg.bits_per_sample = 16;
  cfg.bitrate = VS071_BITRATE;
  cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
  cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
  cfg.complexity = VS071_COMPLEXITY;
  cfg.enable_fec = false;
  cfg.enable_dtx = false;
  cfg.enable_vbr = false;

  void* encoder = nullptr;
  const uint32_t createStarted = micros();
  const esp_audio_err_t openRc = esp_opus_enc_open(&cfg, sizeof(cfg), &encoder);
  const uint32_t createUs = micros() - createStarted;
  if (openRc != ESP_AUDIO_ERR_OK || !encoder) {
    Serial.printf("OPUS LOAD: encoder open FAILED rc=%d create=%uus core=%d stack_hwm=%u\n",
                  (int)openRc, (unsigned)createUs, (int)xPortGetCoreID(),
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    Serial.flush();
    vs071CodecOk = false;
    vs071TaskReady = true;
    screenDirty = true;
    vs071TaskRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  int inBytes = 0;
  int outBytes = 0;
  const esp_audio_err_t sizeRc = esp_opus_enc_get_frame_size(encoder, &inBytes, &outBytes);
  if (sizeRc != ESP_AUDIO_ERR_OK ||
      inBytes != VS071_FRAME_SAMPLES * (int)sizeof(int16_t) ||
      outBytes <= 0 || outBytes > 512) {
    Serial.printf("OPUS LOAD: frame shape FAILED rc=%d in=%d out=%d expected_in=%u\n",
                  (int)sizeRc, inBytes, outBytes,
                  (unsigned)(VS071_FRAME_SAMPLES * sizeof(int16_t)));
    Serial.flush();
    esp_opus_enc_close(encoder);
    vs071CodecOk = false;
    vs071TaskReady = true;
    vs071TaskRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  static int16_t pcm[VS071_FRAME_SAMPLES];
  static uint8_t packet[512];
  esp_audio_enc_in_frame_t input = {};
  input.buffer = reinterpret_cast<uint8_t*>(pcm);
  input.len = sizeof(pcm);
  esp_audio_enc_out_frame_t output = {};
  output.buffer = packet;
  output.len = sizeof(packet);

  Serial.printf(
      "OPUS LOAD: READY esp_audio_codec=2.5.0 rate=%d mono frame=%dms bitrate=%d complexity=%d "
      "frame_in=%d frame_out=%d core=%d priority=%u stack_hwm=%u internal_free=%u psram_free=%u\n",
      VS071_RATE, VS071_FRAME_MS, VS071_BITRATE, VS071_COMPLEXITY,
      inBytes, outBytes, (int)xPortGetCoreID(), (unsigned)uxTaskPriorityGet(nullptr),
      (unsigned)uxTaskGetStackHighWaterMark(nullptr),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.println("OPUS LOAD: waiting for real recorder capture; encoded packets are discarded");
  Serial.flush();

  vs071CodecOk = true;
  vs071TaskReady = true;
  screenDirty = true;

  uint32_t phaseA = 0;
  uint32_t phaseB = 0x12345678U;
  uint32_t noise = 0xA5C39E17U;
  uint32_t syntheticFrameIndex = 0;
  bool wasCapturing = false;
  uint32_t nextDueUs = 0;
  uint32_t windowStartedMs = 0;
  uint32_t windowStartedWavBytes = 0;
  uint32_t lastWavObservedBytes = 0;
  uint32_t lastWavProgressMs = 0;
  uint64_t windowEncodeUs = 0;
  uint64_t windowGenUs = 0;
  uint32_t windowFrames = 0;
  uint32_t windowBytes = 0;
  uint32_t windowMaxEncodeUs = 0;
  uint32_t windowMissed = 0;

  for (;;) {
    if (!vs071Enabled) {
      if (wasCapturing) {
        Serial.println("OPUS LOAD: load paused; master recorder remains active");
        Serial.flush();
      }
      wasCapturing = false;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    const bool capturing = captureRunning;
    if (!capturing) {
      if (wasCapturing) {
        const uint32_t elapsed = millis() - windowStartedMs;
        const double audioSec = windowFrames * (VS071_FRAME_MS / 1000.0);
        const double realtime = windowEncodeUs
            ? (audioSec * 1000000.0) / static_cast<double>(windowEncodeUs) : 0.0;
        Serial.printf(
            "OPUS LOAD: CAPTURE END wall=%lums frames=%lu encoded_audio=%.1fs realtime=%.2fx "
            "max_encode=%luus missed=%lu wav_bytes=%lu audioError=%d stack_hwm=%u\n",
            (unsigned long)elapsed, (unsigned long)windowFrames, audioSec, realtime,
            (unsigned long)windowMaxEncodeUs, (unsigned long)windowMissed,
            (unsigned long)wavDataBytes, audioError ? 1 : 0,
            (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        Serial.flush();
      }
      wasCapturing = false;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (!wasCapturing) {
      wasCapturing = true;
      windowStartedMs = millis();
      windowStartedWavBytes = wavDataBytes;
      lastWavObservedBytes = wavDataBytes;
      lastWavProgressMs = windowStartedMs;
      windowEncodeUs = 0;
      windowGenUs = 0;
      windowFrames = 0;
      windowBytes = 0;
      windowMaxEncodeUs = 0;
      windowMissed = 0;
      nextDueUs = micros();
      Serial.printf(
          "OPUS LOAD: CAPTURE START session=%u wav=%s audioError=%d opus_core=%d opus_prio=%u mic_core=%d mic_prio=%u\n",
          (unsigned)sessionId, wavFinalPath, audioError ? 1 : 0,
          (int)VS071_TASK_CORE, (unsigned)VS071_TASK_PRIORITY,
          (int)VS071_MIC_CORE, (unsigned)VS071_MIC_PRIORITY);
      Serial.flush();
    }

    const uint32_t nowBeforeEncode = millis();
    const uint32_t currentWavBeforeEncode = wavDataBytes;
    if (currentWavBeforeEncode != lastWavObservedBytes) {
      lastWavObservedBytes = currentWavBeforeEncode;
      lastWavProgressMs = nowBeforeEncode;
    }

    if (audioError ||
        (currentWavBeforeEncode > 0 &&
         nowBeforeEncode - lastWavProgressMs >= VS071_WAV_STALL_ABORT_MS)) {
      Serial.printf(
          "OPUS LOAD: FAILSAFE ABORT reason=%s wav_bytes=%lu stalled=%lums; disabling Opus load, master capture continues\n",
          audioError ? "audioError" : "wav_stall",
          (unsigned long)currentWavBeforeEncode,
          (unsigned long)(nowBeforeEncode - lastWavProgressMs));
      Serial.flush();
      vs071Enabled = false;
      screenDirty = true;
      wasCapturing = false;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    uint32_t started = micros();
    vs071GenerateFrame(pcm, phaseA, phaseB, noise, syntheticFrameIndex++);
    const uint32_t genUs = micros() - started;

    output.encoded_bytes = 0;
    output.pts = 0;
    started = micros();
    const esp_audio_err_t rc = esp_opus_enc_process(encoder, &input, &output);
    const uint32_t encUs = micros() - started;
    if (rc != ESP_AUDIO_ERR_OK) {
      Serial.printf("OPUS LOAD: encode FAILED rc=%d frame=%lu enc=%luus\n",
                    (int)rc, (unsigned long)syntheticFrameIndex,
                    (unsigned long)encUs);
      Serial.flush();
      vs071CodecOk = false;
      vs071Enabled = false;
      continue;
    }

    ++vs071TotalFrames;
    ++windowFrames;
    windowEncodeUs += encUs;
    windowGenUs += genUs;
    windowBytes += output.encoded_bytes;
    if (encUs > windowMaxEncodeUs) windowMaxEncodeUs = encUs;

    nextDueUs += VS071_FRAME_MS * 1000U;
    int32_t waitUs = static_cast<int32_t>(nextDueUs - micros());
    if (waitUs <= 0) {
      ++vs071MissedDeadlines;
      ++windowMissed;
      nextDueUs = micros();
    } else {
      TickType_t ticks = pdMS_TO_TICKS(static_cast<uint32_t>(waitUs) / 1000U);
      if (ticks > 0) vTaskDelay(ticks);
      else taskYIELD();
    }

    const uint32_t nowMs = millis();
    const uint32_t elapsedMs = nowMs - windowStartedMs;
    if (elapsedMs >= VS071_LOG_MS) {
      const double audioSec = windowFrames * (VS071_FRAME_MS / 1000.0);
      const double realtime = windowEncodeUs
          ? (audioSec * 1000000.0) / static_cast<double>(windowEncodeUs) : 0.0;
      const double cpuPct = elapsedMs
          ? (static_cast<double>(windowEncodeUs) / (elapsedMs * 1000.0)) * 100.0 : 0.0;
      const double kbps = audioSec > 0.0
          ? (windowBytes * 8.0) / (audioSec * 1000.0) : 0.0;
      uint32_t currentWavBytes = wavDataBytes;
      uint32_t wavDelta = currentWavBytes >= windowStartedWavBytes
          ? currentWavBytes - windowStartedWavBytes : 0;
      const double wavKiBs = elapsedMs
          ? (static_cast<double>(wavDelta) * 1000.0 / elapsedMs) / 1024.0 : 0.0;
      Serial.printf(
          "OPUS LOAD: LIVE wall=%lums frames=%lu audio=%.1fs kbps=%.2f encode_cpu=%.1f%% "
          "realtime=%.2fx max_encode=%luus gen=%.1fms missed=%lu wav_rate=%.1fKiB/s "
          "audioError=%d stack_hwm=%u\n",
          (unsigned long)elapsedMs, (unsigned long)windowFrames, audioSec, kbps,
          cpuPct, realtime, (unsigned long)windowMaxEncodeUs,
          static_cast<double>(windowGenUs) / 1000.0,
          (unsigned long)windowMissed, wavKiBs, audioError ? 1 : 0,
          (unsigned)uxTaskGetStackHighWaterMark(nullptr));
      Serial.flush();

      windowStartedMs = nowMs;
      windowStartedWavBytes = currentWavBytes;
      windowEncodeUs = 0;
      windowGenUs = 0;
      windowFrames = 0;
      windowBytes = 0;
      windowMaxEncodeUs = 0;
      windowMissed = 0;
    }
  }
}

static void vs071PrepareMicPriority() {
  // Called from MENU while the mic is stopped. startCapture() later retrieves
  // this config and only changes sample rate/channel/oversampling/noise, so the
  // task priority/core settings survive into M5.Mic.begin().
  auto micCfg = M5.Mic.config();
  micCfg.task_priority = VS071_MIC_PRIORITY;
  micCfg.task_pinned_core = VS071_MIC_CORE;
  M5.Mic.config(micCfg);
  Serial.printf("OPUS LOAD: recorder protection mic_core=%d mic_priority=%u; opus_core=%d opus_priority=%u\n",
                (int)VS071_MIC_CORE, (unsigned)VS071_MIC_PRIORITY,
                (int)VS071_TASK_CORE, (unsigned)VS071_TASK_PRIORITY);
  Serial.flush();
}


static const char* vs071MenuStatus() {
  if (!vs071TaskRunning && !vs071TaskReady) return "tik om te starten";
  if (vs071TaskRunning && !vs071TaskReady) return "STARTEN...";
  if (!vs071CodecOk) return "FOUT - reboot";
  if (vs071Enabled) return "AAN - start een VISITE";
  return "UIT - tik om aan te zetten";
}

static void vs071MenuToggleLoad() {
  noteActivity();
  if (!vs071TaskRunning && !vs071TaskReady) {
    vs071PrepareMicPriority();
    vs071Enabled = true;
    Serial.printf(
        "OPUS LOAD: starting background task stack=%uB core=%d priority=%u; master WAV path unchanged\n",
        (unsigned)VS071_TASK_STACK_BYTES, (int)VS071_TASK_CORE,
        (unsigned)VS071_TASK_PRIORITY);
    Serial.flush();
    BaseType_t created = xTaskCreatePinnedToCore(
        vs071Worker, "opus_esp_load", VS071_TASK_STACK_BYTES,
        nullptr, VS071_TASK_PRIORITY, &vs071TaskHandle, VS071_TASK_CORE);
    if (created != pdPASS) {
      vs071Enabled = false;
      vs071TaskRunning = false;
      vs071TaskReady = false;
      Serial.printf("OPUS LOAD: task create FAILED internal_free=%u largest=%u\n",
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      Serial.flush();
    }
  } else if (vs071CodecOk) {
    if (!vs071Enabled) vs071PrepareMicPriority();
    vs071Enabled = !vs071Enabled;
    Serial.printf("OPUS LOAD: %s; total_frames=%lu missed_total=%lu\n",
                  vs071Enabled ? "ENABLED" : "PAUSED",
                  (unsigned long)vs071TotalFrames,
                  (unsigned long)vs071MissedDeadlines);
    Serial.flush();
  } else {
    Serial.println("OPUS LOAD: codec/task not healthy; reboot before retry");
    Serial.flush();
  }
  screenDirty = true;
}

struct Vs071InstallHook {
  Vs071InstallHook() {
    // The menu hook itself runs only after Arduino setup and user interaction.
    // Global construction merely installs the function pointer.
    vsSyntheticTestHook = vs071MenuToggleLoad;
    vsSyntheticTestStatusHook = vs071MenuStatus;
  }
};
static Vs071InstallHook vs071InstallHook;

#endif // VISITESCRIBE_OPUS_ESP_LOAD_EXPERIMENT
