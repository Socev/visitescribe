#pragma once

// Direct-to-Ogg/Opus recorder backend for the CoreS3-Lite.
//
// Enabled only by VISITESCRIBE_DIRECT_OPUS. The normal WAV recorder is left
// untouched in all other environments.
//
// Capture path:
//   ES7210 / M5 Mic 48 kHz stereo PCM16
//     -> streaming 3:1 downsample + stereo average
//     -> 16 kHz mono PCM16, 20 ms frames
//     -> Espressif esp_audio_codec Opus, 24 kbit/s VBR, VOIP
//     -> self-contained Ogg/Opus files of at most 30 seconds
//
// Patient audio is never buffered as a full PCM/WAV file. A small queue absorbs
// encoder/SD jitter. Every completed .opus chunk is independently decodable.

#ifdef VISITESCRIBE_DIRECT_OPUS

#include <Arduino.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <vector>
#include <algorithm>
#include "esp_audio_types.h"
#include "esp_opus_enc.h"

static constexpr uint32_t VS_DO_RATE = 16000;
static constexpr uint16_t VS_DO_CHANNELS = 1;
static constexpr uint16_t VS_DO_BITS = 16;
static constexpr uint32_t VS_DO_BITRATE = 24000;
static constexpr uint32_t VS_DO_FRAME_MS = 20;
static constexpr uint32_t VS_DO_FRAME_SAMPLES =
    VS_DO_RATE * VS_DO_FRAME_MS / 1000;  // 320
static constexpr uint32_t VS_DO_GRANULE_PER_FRAME =
    48000 * VS_DO_FRAME_MS / 1000;       // 960
static constexpr uint32_t VS_DO_FRAMES_PER_CHUNK =
    30000 / VS_DO_FRAME_MS;              // 1500
static constexpr uint8_t VS_DO_PACKETS_PER_PAGE = 10;
static constexpr size_t VS_DO_MAX_PACKET = 512;
static constexpr uint8_t VS_DO_QUEUE_FRAMES = 24;
static constexpr uint32_t VS_DO_WORKER_STACK = 32768;
static constexpr BaseType_t VS_DO_WORKER_CORE = 0;
static constexpr UBaseType_t VS_DO_WORKER_PRIORITY = 1;
static constexpr BaseType_t VS_DO_MIC_CORE = 0;
static constexpr uint8_t VS_DO_MIC_PRIORITY = 4;

// libopus' normal algorithmic look-ahead is 312 samples on the mandatory
// 48 kHz Ogg granule clock (104 samples at our 16 kHz encoder input). Each
// chunk is therefore flushed with one extra zero-input Opus packet and its
// final granule trims that packet back to exactly the real input duration.
static constexpr uint16_t VS_DO_PRE_SKIP = 312;

struct VsDirectOpusChunkInfo {
  uint32_t sequence = 0;
  String path;
  uint32_t durationMs = 0;
  uint32_t bytes = 0;
};

static std::vector<VsDirectOpusChunkInfo>
vsDirectOpusChunksForPrefix(const String& prefix) {
  std::vector<VsDirectOpusChunkInfo> rows;
  const String metaPath = String("/visitescribe/") + prefix + "_opus.csv";
  File meta = SD.open(metaPath, FILE_READ);
  if (!meta) return rows;

  bool first = true;
  while (meta.available()) {
    String line = meta.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;

    if (first) {
      first = false;
      if (line.startsWith("sequence,")) continue;
    }

    const int c1 = line.indexOf(',');
    const int c2 = c1 >= 0 ? line.indexOf(',', c1 + 1) : -1;
    const int c3 = c2 >= 0 ? line.indexOf(',', c2 + 1) : -1;
    if (c1 <= 0 || c2 <= c1 || c3 <= c2) continue;

    VsDirectOpusChunkInfo row;
    row.sequence = static_cast<uint32_t>(line.substring(0, c1).toInt());
    row.path = line.substring(c1 + 1, c2);
    row.durationMs =
        static_cast<uint32_t>(line.substring(c2 + 1, c3).toInt());
    row.bytes = static_cast<uint32_t>(line.substring(c3 + 1).toInt());

    if (!row.sequence || !row.durationMs ||
        !row.path.startsWith("/visitescribe/") ||
        row.path.indexOf("..") >= 0) {
      continue;
    }

    File opus = SD.open(row.path, FILE_READ);
    if (!opus) continue;
    row.bytes = static_cast<uint32_t>(opus.size());
    opus.close();
    if (row.bytes) rows.push_back(row);
  }
  meta.close();

  std::sort(
      rows.begin(), rows.end(),
      [](const VsDirectOpusChunkInfo& a, const VsDirectOpusChunkInfo& b) {
        return a.sequence < b.sequence;
      });

  // A missing sequence means the local recording is incomplete/corrupt. Do
  // not silently renumber it: the v4 manifest/API sequence is part of the
  // authenticated recording identity.
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].sequence != i + 1) {
      rows.clear();
      break;
    }
  }
  return rows;
}

static String vsDirectOpusModeFromEvents(const String& eventsPath) {
  File events = SD.open(eventsPath, FILE_READ);
  if (!events) return "single_patient";

  bool meeting = false;
  bool patientBoundary = false;
  while (events.available()) {
    String line = events.readStringUntil('\n');
    if (line.indexOf(",mode_selected_meeting,") >= 0) meeting = true;
    if (line.indexOf(",patient_boundary,") >= 0) patientBoundary = true;
  }
  events.close();

  if (meeting) return "meeting";
  if (patientBoundary) return "multi_patient";
  return "single_patient";
}

struct VsDoQueueItem {
  uint8_t kind = 0;  // 0 = audio, 1 = stop
  int16_t pcm[VS_DO_FRAME_SAMPLES] = {0};
};

static QueueHandle_t vsDoQueue = nullptr;
static TaskHandle_t vsDoWorkerHandle = nullptr;
static SemaphoreHandle_t vsDoSdMutex = nullptr;

static volatile bool vsDoWorkerReady = false;
static volatile bool vsDoWorkerDone = true;
static volatile bool vsDoWorkerFailed = false;
static volatile uint32_t vsDoDroppedFrames = 0;
static volatile uint32_t vsDoEncodedFrames = 0;
static volatile uint32_t vsDoEncodedBytes = 0;

static uint16_t vsDoSeenSessionId = 0;
static uint32_t vsDoChunkSequence = 0;

// Main-loop downsampler state. One mono output sample is the arithmetic mean
// of L/R over three 48 kHz source frames.
static int32_t vsDoDownsampleSum = 0;
static uint8_t vsDoDownsampleFrames = 0;
static uint16_t vsDoPcmFill = 0;
// Producer-owned latch: stopCapture() may drain more buffers after an error.
static bool vsDoProducerFailed = false;
static int16_t vsDoPcmFrame[VS_DO_FRAME_SAMPLES];

// Worker-owned Ogg/encoder state.
static void* vsDoEncoder = nullptr;
static File vsDoFile;
static char vsDoTmpPath[128] = {0};
static char vsDoFinalPath[128] = {0};
static uint32_t vsDoOggSerial = 0;
static uint32_t vsDoOggPageSequence = 0;
static uint32_t vsDoChunkFrames = 0;

static uint8_t vsDoPagePacketData[VS_DO_PACKETS_PER_PAGE][VS_DO_MAX_PACKET];
static uint16_t vsDoPagePacketLen[VS_DO_PACKETS_PER_PAGE] = {0};
static uint8_t vsDoPagePacketCount = 0;
static uint32_t vsDoPageGranule = 0;

// Max lacing per packet is 3 for our 512-byte cap (255, 255, 2), plus one
// terminating zero when packet size is an exact multiple of 255.
static uint8_t vsDoOggPageBuffer[
    27 + VS_DO_PACKETS_PER_PAGE * 4 +
    VS_DO_PACKETS_PER_PAGE * VS_DO_MAX_PACKET];

static uint32_t vsDoOggCrc(const uint8_t* data, size_t len) {
  uint32_t crc = 0;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint32_t>(data[i]) << 24;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80000000U)
          ? (crc << 1) ^ 0x04C11DB7U
          : (crc << 1);
    }
  }
  return crc;
}

static void vsDoPutLe16(uint8_t* dst, uint16_t value) {
  dst[0] = value & 0xFF;
  dst[1] = (value >> 8) & 0xFF;
}

static void vsDoPutLe32(uint8_t* dst, uint32_t value) {
  dst[0] = value & 0xFF;
  dst[1] = (value >> 8) & 0xFF;
  dst[2] = (value >> 16) & 0xFF;
  dst[3] = (value >> 24) & 0xFF;
}

static void vsDoPutLe64(uint8_t* dst, uint64_t value) {
  for (int i = 0; i < 8; ++i) dst[i] = (value >> (8 * i)) & 0xFF;
}

static bool vsDirectOpusLockSd(TickType_t wait = pdMS_TO_TICKS(1000)) {
  if (!vsDoSdMutex) {
    vsDoSdMutex = xSemaphoreCreateMutex();
    if (!vsDoSdMutex) return false;
  }
  return xSemaphoreTake(vsDoSdMutex, wait) == pdTRUE;
}

static void vsDirectOpusUnlockSd() {
  if (vsDoSdMutex) xSemaphoreGive(vsDoSdMutex);
}

static bool vsDoWriteOggPage(const uint8_t* const* packets,
                             const uint16_t* lengths,
                             uint8_t packetCount,
                             uint8_t flags,
                             int64_t granule) {
  if (!vsDoFile || packetCount == 0) return false;

  uint8_t lacing[VS_DO_PACKETS_PER_PAGE * 4] = {0};
  size_t laceCount = 0;
  size_t bodyBytes = 0;

  for (uint8_t p = 0; p < packetCount; ++p) {
    uint16_t left = lengths[p];
    bodyBytes += left;
    while (left >= 255) {
      if (laceCount >= sizeof(lacing)) return false;
      lacing[laceCount++] = 255;
      left -= 255;
    }
    if (laceCount >= sizeof(lacing)) return false;
    lacing[laceCount++] = static_cast<uint8_t>(left);
  }

  const size_t headerBytes = 27 + laceCount;
  const size_t totalBytes = headerBytes + bodyBytes;
  if (totalBytes > sizeof(vsDoOggPageBuffer)) return false;

  uint8_t* out = vsDoOggPageBuffer;
  memset(out, 0, headerBytes);
  memcpy(out, "OggS", 4);
  out[4] = 0;
  out[5] = flags;
  vsDoPutLe64(out + 6, static_cast<uint64_t>(granule));
  vsDoPutLe32(out + 14, vsDoOggSerial);
  vsDoPutLe32(out + 18, vsDoOggPageSequence++);
  // CRC bytes 22..25 stay zero for calculation.
  out[26] = static_cast<uint8_t>(laceCount);
  memcpy(out + 27, lacing, laceCount);

  size_t pos = headerBytes;
  for (uint8_t p = 0; p < packetCount; ++p) {
    memcpy(out + pos, packets[p], lengths[p]);
    pos += lengths[p];
  }

  const uint32_t crc = vsDoOggCrc(out, totalBytes);
  vsDoPutLe32(out + 22, crc);

  if (!vsDirectOpusLockSd()) return false;
  const size_t written = vsDoFile.write(out, totalBytes);
  vsDirectOpusUnlockSd();
  return written == totalBytes;
}

static bool vsDoWriteSinglePacketPage(const uint8_t* packet,
                                      uint16_t len,
                                      uint8_t flags,
                                      int64_t granule) {
  const uint8_t* packets[1] = {packet};
  const uint16_t lengths[1] = {len};
  return vsDoWriteOggPage(packets, lengths, 1, flags, granule);
}

static bool vsDoWriteHeaders() {
  uint8_t head[19] = {0};
  memcpy(head, "OpusHead", 8);
  head[8] = 1;   // version
  head[9] = 1;   // mono
  vsDoPutLe16(head + 10, VS_DO_PRE_SKIP);
  vsDoPutLe32(head + 12, VS_DO_RATE);
  vsDoPutLe16(head + 16, 0); // output gain Q7.8
  head[18] = 0;              // channel mapping family 0

  if (!vsDoWriteSinglePacketPage(head, sizeof(head), 0x02, 0)) return false;

  static const char vendor[] = "VisiteScribe-M5";
  const uint32_t vendorLen = sizeof(vendor) - 1;
  uint8_t tags[8 + 4 + sizeof(vendor) - 1 + 4] = {0};
  memcpy(tags, "OpusTags", 8);
  vsDoPutLe32(tags + 8, vendorLen);
  memcpy(tags + 12, vendor, vendorLen);
  vsDoPutLe32(tags + 12 + vendorLen, 0); // zero user comments

  return vsDoWriteSinglePacketPage(tags, sizeof(tags), 0x00, 0);
}

static bool vsDoFlushAudioPage(bool eos) {
  if (vsDoPagePacketCount == 0) return true;

  const uint8_t* packets[VS_DO_PACKETS_PER_PAGE] = {nullptr};
  for (uint8_t i = 0; i < vsDoPagePacketCount; ++i) {
    packets[i] = vsDoPagePacketData[i];
  }
  const bool ok = vsDoWriteOggPage(
      packets,
      vsDoPagePacketLen,
      vsDoPagePacketCount,
      eos ? 0x04 : 0x00,
      static_cast<int64_t>(vsDoPageGranule));
  vsDoPagePacketCount = 0;
  return ok;
}

static bool vsDoOpenEncoder() {
  if (vsDoEncoder) return true;

  esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
  cfg.sample_rate = VS_DO_RATE;
  cfg.channel = VS_DO_CHANNELS;
  cfg.bits_per_sample = VS_DO_BITS;
  cfg.bitrate = VS_DO_BITRATE;
  cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
  cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
  cfg.complexity = 0;
  cfg.enable_fec = false;
  cfg.enable_dtx = false;
  cfg.enable_vbr = true;

  const esp_audio_err_t rc =
      esp_opus_enc_open(&cfg, sizeof(cfg), &vsDoEncoder);
  if (rc != ESP_AUDIO_ERR_OK || !vsDoEncoder) {
    Serial.printf("DIRECT OPUS: encoder open FAIL rc=%d\n", (int)rc);
    vsDoEncoder = nullptr;
    return false;
  }

  int inBytes = 0;
  int outBytes = 0;
  const esp_audio_err_t sizeRc =
      esp_opus_enc_get_frame_size(vsDoEncoder, &inBytes, &outBytes);
  if (sizeRc != ESP_AUDIO_ERR_OK ||
      inBytes != static_cast<int>(VS_DO_FRAME_SAMPLES * sizeof(int16_t)) ||
      outBytes <= 0 ||
      outBytes > static_cast<int>(VS_DO_MAX_PACKET)) {
    Serial.printf(
        "DIRECT OPUS: frame shape FAIL rc=%d in=%d out=%d expected=%u\n",
        (int)sizeRc, inBytes, outBytes,
        (unsigned)(VS_DO_FRAME_SAMPLES * sizeof(int16_t)));
    esp_opus_enc_close(vsDoEncoder);
    vsDoEncoder = nullptr;
    return false;
  }

  return true;
}

static void vsDoCloseEncoder() {
  if (vsDoEncoder) {
    esp_opus_enc_close(vsDoEncoder);
    vsDoEncoder = nullptr;
  }
}

static void vsDoAppendChunkMeta(uint32_t sequence,
                                const char* finalPath,
                                uint32_t durationMs,
                                uint32_t bytes) {
  char metaPath[96];
  snprintf(metaPath, sizeof(metaPath),
           "/visitescribe/s%05u_opus.csv", vsDoSeenSessionId);

  if (!vsDirectOpusLockSd()) return;
  const bool exists = SD.exists(metaPath);
  File meta = SD.open(metaPath, FILE_APPEND);
  if (meta) {
    if (!exists || meta.size() == 0) {
      meta.println("sequence,path,duration_ms,bytes");
    }
    meta.printf("%lu,%s,%lu,%lu\n",
                (unsigned long)sequence,
                finalPath,
                (unsigned long)durationMs,
                (unsigned long)bytes);
    meta.close();
  }
  vsDirectOpusUnlockSd();
}

static bool vsDoEncodeTailPacket();

static bool vsDoOpenChunk() {
  ++vsDoChunkSequence;
  vsDoOggSerial =
      0x56530000U ^
      (static_cast<uint32_t>(vsDoSeenSessionId) << 8) ^
      vsDoChunkSequence;
  vsDoOggPageSequence = 0;
  vsDoChunkFrames = 0;
  vsDoPagePacketCount = 0;
  vsDoPageGranule = 0;

  snprintf(vsDoFinalPath, sizeof(vsDoFinalPath),
           "/visitescribe/s%05u_chunk_%06lu.opus",
           vsDoSeenSessionId,
           (unsigned long)vsDoChunkSequence);
  snprintf(vsDoTmpPath, sizeof(vsDoTmpPath), "%s.tmp", vsDoFinalPath);

  if (!vsDirectOpusLockSd()) return false;
  if (SD.exists(vsDoTmpPath)) SD.remove(vsDoTmpPath);
  vsDoFile = SD.open(vsDoTmpPath, FILE_WRITE);
  vsDirectOpusUnlockSd();
  if (!vsDoFile) return false;

  if (!vsDoOpenEncoder()) {
    if (vsDirectOpusLockSd()) {
      vsDoFile.close();
      SD.remove(vsDoTmpPath);
      vsDirectOpusUnlockSd();
    }
    return false;
  }

  if (!vsDoWriteHeaders()) {
    vsDoCloseEncoder();
    if (vsDirectOpusLockSd()) {
      vsDoFile.close();
      SD.remove(vsDoTmpPath);
      vsDirectOpusUnlockSd();
    }
    return false;
  }

  // Existing event CSV has an audio_file column. Keep it useful for diagnostics.
  strncpy(wavFinalPath, vsDoFinalPath, sizeof(wavFinalPath) - 1);
  wavFinalPath[sizeof(wavFinalPath) - 1] = '\0';

  Serial.printf(
      "DIRECT OPUS: chunk %lu OPEN %s rate=16000 mono bitrate=24000 vbr=1 frame=20ms\n",
      (unsigned long)vsDoChunkSequence, vsDoFinalPath);
  return true;
}

static bool vsDoFinalizeChunk() {
  if (!vsDoFile) {
    vsDoCloseEncoder();
    return true;
  }

  bool ok = true;
  if (vsDoChunkFrames > 0) {
    if (!vsDoEncodeTailPacket()) ok = false;
    if (ok) ok = vsDoFlushAudioPage(true);
  }

  uint32_t physical = 0;
  if (vsDirectOpusLockSd()) {
    vsDoFile.flush();
    physical = static_cast<uint32_t>(vsDoFile.size());
    vsDoFile.close();

    if (vsDoChunkFrames == 0) {
      SD.remove(vsDoTmpPath);
    } else {
      if (SD.exists(vsDoFinalPath)) SD.remove(vsDoFinalPath);
      if (!SD.rename(vsDoTmpPath, vsDoFinalPath)) ok = false;
    }
    vsDirectOpusUnlockSd();
  } else {
    ok = false;
  }

  if (vsDoChunkFrames > 0) {
    const uint32_t durationMs = vsDoChunkFrames * VS_DO_FRAME_MS;
    // Never advertise a chunk in the sync sidecar unless the complete Ogg file
    // was closed and atomically renamed successfully. A failed .tmp remains on
    // the card for diagnosis/recovery instead of becoming a bogus upload row.
    if (ok) {
      vsDoAppendChunkMeta(
          vsDoChunkSequence, vsDoFinalPath, durationMs, physical);
    }
    Serial.printf(
        "DIRECT OPUS: chunk %lu CLOSE frames=%lu duration=%lums bytes=%lu ok=%d\n",
        (unsigned long)vsDoChunkSequence,
        (unsigned long)vsDoChunkFrames,
        (unsigned long)durationMs,
        (unsigned long)physical,
        ok ? 1 : 0);
  }

  vsDoCloseEncoder();
  vsDoChunkFrames = 0;
  vsDoPagePacketCount = 0;
  return ok;
}

static bool vsDoEncodeTailPacket() {
  if (!vsDoEncoder || vsDoChunkFrames == 0) return true;

  static const int16_t silence[VS_DO_FRAME_SAMPLES] = {0};
  uint8_t packet[VS_DO_MAX_PACKET] = {0};

  esp_audio_enc_in_frame_t input = {};
  input.buffer = reinterpret_cast<uint8_t*>(
      const_cast<int16_t*>(silence));
  input.len = VS_DO_FRAME_SAMPLES * sizeof(int16_t);

  esp_audio_enc_out_frame_t output = {};
  output.buffer = packet;
  output.len = sizeof(packet);
  output.encoded_bytes = 0;

  const esp_audio_err_t rc =
      esp_opus_enc_process(vsDoEncoder, &input, &output);
  if (rc != ESP_AUDIO_ERR_OK ||
      output.encoded_bytes <= 0 ||
      output.encoded_bytes > static_cast<int>(VS_DO_MAX_PACKET)) {
    Serial.printf("DIRECT OPUS: tail encode FAIL rc=%d bytes=%d\n",
                  (int)rc, output.encoded_bytes);
    return false;
  }

  if (vsDoPagePacketCount >= VS_DO_PACKETS_PER_PAGE) {
    if (!vsDoFlushAudioPage(false)) return false;
  }

  const uint8_t slot = vsDoPagePacketCount++;
  memcpy(vsDoPagePacketData[slot], packet, output.encoded_bytes);
  vsDoPagePacketLen[slot] = static_cast<uint16_t>(output.encoded_bytes);
  vsDoEncodedBytes += static_cast<uint32_t>(output.encoded_bytes);

  // This is an encoder-drain packet, not additional source audio. The final
  // Ogg granule trims its tail so decoded duration remains exactly the number
  // of real 20 ms input frames.
  vsDoPageGranule =
      VS_DO_PRE_SKIP + vsDoChunkFrames * VS_DO_GRANULE_PER_FRAME;
  return true;
}

static bool vsDoEncodeFrame(const int16_t pcm[VS_DO_FRAME_SAMPLES]) {
  if (!vsDoFile && !vsDoOpenChunk()) return false;
  if (!vsDoEncoder) return false;

  uint8_t packet[VS_DO_MAX_PACKET] = {0};
  esp_audio_enc_in_frame_t input = {};
  input.buffer = reinterpret_cast<uint8_t*>(const_cast<int16_t*>(pcm));
  input.len = VS_DO_FRAME_SAMPLES * sizeof(int16_t);

  esp_audio_enc_out_frame_t output = {};
  output.buffer = packet;
  output.len = sizeof(packet);
  output.encoded_bytes = 0;

  const uint32_t startedUs = micros();
  const esp_audio_err_t rc =
      esp_opus_enc_process(vsDoEncoder, &input, &output);
  const uint32_t encodeUs = micros() - startedUs;

  if (rc != ESP_AUDIO_ERR_OK ||
      output.encoded_bytes <= 0 ||
      output.encoded_bytes > static_cast<int>(VS_DO_MAX_PACKET)) {
    Serial.printf(
        "DIRECT OPUS: encode FAIL rc=%d bytes=%d frame=%lu\n",
        (int)rc, output.encoded_bytes,
        (unsigned long)vsDoEncodedFrames);
    return false;
  }

  // If the current Ogg page is already full, flush it before the new packet.
  // This guarantees that only the actual final page receives EOS.
  if (vsDoPagePacketCount >= VS_DO_PACKETS_PER_PAGE) {
    if (!vsDoFlushAudioPage(false)) return false;
  }

  const uint8_t slot = vsDoPagePacketCount++;
  memcpy(vsDoPagePacketData[slot], packet, output.encoded_bytes);
  vsDoPagePacketLen[slot] = static_cast<uint16_t>(output.encoded_bytes);

  ++vsDoChunkFrames;
  ++vsDoEncodedFrames;
  vsDoEncodedBytes += static_cast<uint32_t>(output.encoded_bytes);
  vsDoPageGranule = vsDoChunkFrames * VS_DO_GRANULE_PER_FRAME;

  if ((vsDoEncodedFrames % 500U) == 0) {
    Serial.printf(
        "DIRECT OPUS: frames=%lu raw_opus=%luB last_encode=%luus queue=%u dropped=%lu\n",
        (unsigned long)vsDoEncodedFrames,
        (unsigned long)vsDoEncodedBytes,
        (unsigned long)encodeUs,
        (unsigned)(vsDoQueue ? uxQueueMessagesWaiting(vsDoQueue) : 0),
        (unsigned long)vsDoDroppedFrames);
  }

  if (vsDoChunkFrames >= VS_DO_FRAMES_PER_CHUNK) {
    // Exactly 30 seconds. The next incoming frame will open a fresh encoder
    // and fresh self-contained Ogg stream.
    if (!vsDoFinalizeChunk()) return false;
  }
  return true;
}

static void vsDoWorker(void*) {
  vsDoWorkerDone = false;
  vsDoWorkerFailed = false;

  // Open the first chunk and encoder before capture starts. That keeps encoder
  // initialization latency out of the first live audio frame.
  if (!vsDoOpenChunk()) {
    vsDoWorkerFailed = true;
    vsDoWorkerReady = true;
    vsDoWorkerDone = true;
    vsDoWorkerHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  vsDoWorkerReady = true;
  Serial.printf(
      "DIRECT OPUS: worker READY core=%d priority=%u stack_hwm=%u\n",
      (int)xPortGetCoreID(),
      (unsigned)uxTaskPriorityGet(nullptr),
      (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  VsDoQueueItem item;
  for (;;) {
    if (xQueueReceive(vsDoQueue, &item, portMAX_DELAY) != pdTRUE) continue;
    if (item.kind == 1) break;
    if (!vsDoEncodeFrame(item.pcm)) {
      vsDoWorkerFailed = true;
      break;
    }
  }

  if (!vsDoFinalizeChunk()) vsDoWorkerFailed = true;

  Serial.printf(
      "DIRECT OPUS: worker DONE failed=%d frames=%lu bytes=%lu dropped=%lu stack_hwm=%u\n",
      vsDoWorkerFailed ? 1 : 0,
      (unsigned long)vsDoEncodedFrames,
      (unsigned long)vsDoEncodedBytes,
      (unsigned long)vsDoDroppedFrames,
      (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  vsDoWorkerReady = false;
  vsDoWorkerDone = true;
  vsDoWorkerHandle = nullptr;
  vTaskDelete(nullptr);
}

static void vsDirectOpusResetSession(uint16_t newSessionId) {
  vsDoSeenSessionId = newSessionId;
  vsDoChunkSequence = 0;
  vsDoDroppedFrames = 0;
  vsDoEncodedFrames = 0;
  vsDoEncodedBytes = 0;
  vsDoDownsampleSum = 0;
  vsDoDownsampleFrames = 0;
  vsDoPcmFill = 0;

  char metaPath[96];
  snprintf(metaPath, sizeof(metaPath),
           "/visitescribe/s%05u_opus.csv", newSessionId);
  if (vsDirectOpusLockSd()) {
    if (SD.exists(metaPath)) SD.remove(metaPath);
    vsDirectOpusUnlockSd();
  }
}

static bool vsDirectOpusBeginSegment() {
  if (!vsDoSdMutex) {
    vsDoSdMutex = xSemaphoreCreateMutex();
    if (!vsDoSdMutex) return false;
  }
  if (!vsDoQueue) {
    vsDoQueue = xQueueCreate(VS_DO_QUEUE_FRAMES, sizeof(VsDoQueueItem));
    if (!vsDoQueue) return false;
  }
  xQueueReset(vsDoQueue);

  vsDoDownsampleSum = 0;
  vsDoDownsampleFrames = 0;
  vsDoPcmFill = 0;
  vsDoProducerFailed = false;
  vsDoWorkerReady = false;
  vsDoWorkerDone = false;
  vsDoWorkerFailed = false;

  const BaseType_t created = xTaskCreatePinnedToCore(
      vsDoWorker,
      "direct_opus",
      VS_DO_WORKER_STACK,
      nullptr,
      VS_DO_WORKER_PRIORITY,
      &vsDoWorkerHandle,
      VS_DO_WORKER_CORE);
  if (created != pdPASS) {
    vsDoWorkerHandle = nullptr;
    vsDoWorkerDone = true;
    return false;
  }

  const uint32_t deadline = millis() + 5000;
  while (!vsDoWorkerReady && !vsDoWorkerDone &&
         static_cast<int32_t>(deadline - millis()) > 0) {
    delay(5);
  }

  if (!vsDoWorkerReady || vsDoWorkerFailed) return false;
  return true;
}

static bool vsDirectOpusReady() {
  return vsDoWorkerReady && !vsDoWorkerFailed;
}

static bool vsDoQueuePcmFrame() {
  if (!vsDoQueue || !vsDoWorkerReady || vsDoWorkerFailed || vsDoWorkerDone) {
    return false;
  }

  VsDoQueueItem item;
  item.kind = 0;
  memcpy(item.pcm, vsDoPcmFrame, sizeof(item.pcm));
  // Allow one frame period for the worker to drain a full queue at rollover.
  // Never block indefinitely if the encoder or SD has failed.
  if (xQueueSend(vsDoQueue, &item, pdMS_TO_TICKS(VS_DO_FRAME_MS)) != pdTRUE) {
    ++vsDoDroppedFrames;
    return false;
  }
  return true;
}

static bool vsDirectOpusConsumeStereo(const int16_t* samples, size_t count) {
  if (vsDoProducerFailed) return false;
  if (!samples || count < 2) return true;
  if (vsDoPcmFill >= VS_DO_FRAME_SAMPLES) {
    vsDoPcmFill = 0;
    vsDoProducerFailed = true;
    return false;
  }

  const size_t frames = count / 2;
  for (size_t i = 0; i < frames; ++i) {
    const int16_t left = samples[i * 2];
    const int16_t right = samples[i * 2 + 1];

    vsDoDownsampleSum += static_cast<int32_t>(left) +
                         static_cast<int32_t>(right);
    ++vsDoDownsampleFrames;

    if (vsDoDownsampleFrames == 3) {
      vsDoPcmFrame[vsDoPcmFill++] =
          static_cast<int16_t>(vsDoDownsampleSum / 6);
      vsDoDownsampleSum = 0;
      vsDoDownsampleFrames = 0;

      if (vsDoPcmFill == VS_DO_FRAME_SAMPLES) {
        // Reset before any failure return: a subsequent microphone callback
        // must never write sample 320 beyond vsDoPcmFrame.
        vsDoPcmFill = 0;
        if (!vsDoQueuePcmFrame()) {
          vsDoProducerFailed = true;
          return false;
        }
      }
    }
  }
  return true;
}

static bool vsDirectOpusFinishSegment() {
  if (!vsDoQueue) return true;

  // Deliberately discard a partial (<20 ms) PCM frame. Every stored Opus file
  // therefore represents an exact whole number of 20 ms input frames.
  vsDoPcmFill = 0;
  vsDoDownsampleSum = 0;
  vsDoDownsampleFrames = 0;

  if (!vsDoWorkerDone) {
    VsDoQueueItem stop;
    stop.kind = 1;
    if (xQueueSend(vsDoQueue, &stop, pdMS_TO_TICKS(3000)) != pdTRUE) {
      Serial.println("DIRECT OPUS: stop sentinel queue timeout");
      return false;
    }

    const uint32_t deadline = millis() + 8000;
    while (!vsDoWorkerDone &&
           static_cast<int32_t>(deadline - millis()) > 0) {
      delay(5);
    }
  }

  return vsDoWorkerDone && !vsDoWorkerFailed && vsDoDroppedFrames == 0;
}

static uint32_t vsDirectOpusDroppedFrames() {
  return vsDoDroppedFrames;
}

// Explicit user-requested false-start cleanup. This is only called after the
// encoder worker has stopped, so no task or File handle can still reference
// the session. Every file with this exact session prefix is removed; unrelated
// recordings are untouched.
static bool vsDirectOpusDiscardSession(uint16_t discardSessionId) {
  char prefixBuf[16];
  snprintf(prefixBuf, sizeof(prefixBuf), "s%05u", discardSessionId);
  const String prefix(prefixBuf);

  std::vector<String> removePaths;
  File dir = SD.open("/visitescribe");
  if (!dir) return false;

  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      String name = f.name() ? String(f.name()) : String();
      const int slash = name.lastIndexOf('/');
      const String base = slash >= 0 ? name.substring(slash + 1) : name;
      if (base.startsWith(prefix + "_")) {
        removePaths.push_back(String("/visitescribe/") + base);
      }
    }
    f.close();
  }
  dir.close();

  bool ok = true;
  if (!vsDirectOpusLockSd(pdMS_TO_TICKS(3000))) return false;
  for (const auto& path : removePaths) {
    if (SD.exists(path) && !SD.remove(path)) {
      ok = false;
      Serial.printf("DIRECT OPUS: false-start remove FAIL %s\n", path.c_str());
    } else {
      Serial.printf("DIRECT OPUS: false-start removed %s\n", path.c_str());
    }
  }
  vsDirectOpusUnlockSd();
  return ok;
}

#endif  // VISITESCRIBE_DIRECT_OPUS
