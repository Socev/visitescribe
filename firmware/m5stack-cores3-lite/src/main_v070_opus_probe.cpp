// VisiteScribe CoreS3-Lite v0.7.0 experimental Opus encoder benchmark.
//
// This file is included ONLY when VISITESCRIBE_OPUS_EXPERIMENT is enabled by
// the dedicated PlatformIO environment. It does not alter recording, sync,
// crypto or server formats. Its only job is to prove encoder speed and output
// size on the exact CoreS3-Lite hardware before Opus becomes a production wire
// format.

#ifdef VISITESCRIBE_OPUS_EXPERIMENT

#include <opus.h>

static constexpr int VS070_RATE = 16000;
static constexpr int VS070_CHANNELS = 1;
static constexpr int VS070_FRAME_MS = 20;
static constexpr int VS070_FRAME_SAMPLES = VS070_RATE * VS070_FRAME_MS / 1000; // 320
static constexpr int VS070_BITRATE = 24000;
static constexpr int VS070_COMPLEXITY = 1;
static constexpr int VS070_SECONDS = 300;             // 5 minutes
static constexpr int VS070_BLOCK_SECONDS = 30;
static constexpr int VS070_FRAMES_PER_BLOCK = VS070_BLOCK_SECONDS * 1000 / VS070_FRAME_MS;
static constexpr int VS070_TOTAL_FRAMES = VS070_SECONDS * 1000 / VS070_FRAME_MS;
static constexpr int VS070_MAX_PACKET = 512;

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
  // Deterministic voiced-ish signal with a slowly varying envelope plus a
  // small unvoiced component. Content generation is timed separately from
  // opus_encode(), so it cannot make the codec benchmark look slower.
  const uint32_t envelopeStep = (frameIndex / 25U) % 10U;
  const int32_t envelope = 5000 + static_cast<int32_t>(envelopeStep) * 900;
  for (int i = 0; i < VS070_FRAME_SAMPLES; ++i) {
    phaseA += 5905580U;   // ~220 Hz in 32-bit phase space @ 16 kHz
    phaseB += 9126805U;   // ~340 Hz
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

static void vs070MenuOpusTest() {
  noteActivity();
  Serial.println("OPUS TEST: starting 5 min synthetic speech benchmark");
  Serial.printf("OPUS TEST: config rate=%d channels=%d frame=%dms bitrate=%d complexity=%d cbr=1\n",
                VS070_RATE, VS070_CHANNELS, VS070_FRAME_MS,
                VS070_BITRATE, VS070_COMPLEXITY);

  int error = OPUS_OK;
  OpusEncoder* encoder = opus_encoder_create(
      VS070_RATE, VS070_CHANNELS, OPUS_APPLICATION_VOIP, &error);
  if (!encoder || error != OPUS_OK) {
    Serial.printf("OPUS TEST: encoder create FAILED error=%d %s\n",
                  error, opus_strerror(error));
    if (encoder) opus_encoder_destroy(encoder);
    screenDirty = true;
    return;
  }

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
    screenDirty = true;
    return;
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
    uint64_t blockBytes = 0;
    uint64_t blockEncodeUs = 0;
    uint64_t blockGenerateUs = 0;
    vs070DrawProgress(block + 1, totalBlocks, "encoderen...");

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
        Serial.printf("OPUS TEST: encode FAILED frame=%lu error=%d %s\n",
                      (unsigned long)frameIndex, bytes, opus_strerror(bytes));
        opus_encoder_destroy(encoder);
        screenDirty = true;
        return;
      }

      blockEncodeUs += encUs;
      totalEncodeUs += encUs;
      blockBytes += static_cast<uint32_t>(bytes);
      totalBytes += static_cast<uint32_t>(bytes);
      if (static_cast<uint32_t>(bytes) < minPacket) minPacket = static_cast<uint32_t>(bytes);
      if (static_cast<uint32_t>(bytes) > maxPacket) maxPacket = static_cast<uint32_t>(bytes);
      ++completedFrames;
      if ((f & 127) == 0) delay(0);
    }

    const double blockEncodeMs = static_cast<double>(blockEncodeUs) / 1000.0;
    const double realtime = blockEncodeMs > 0.0
        ? (VS070_BLOCK_SECONDS * 1000.0) / blockEncodeMs : 0.0;
    const double kbps = (static_cast<double>(blockBytes) * 8.0) /
                        (VS070_BLOCK_SECONDS * 1000.0);
    Serial.printf(
        "OPUS TEST: block %d/%d audio=%ds bytes=%llu kbps=%.2f "
        "encode=%.1fms gen=%.1fms realtime=%.2fx\n",
        block + 1, totalBlocks, VS070_BLOCK_SECONDS,
        (unsigned long long)blockBytes, kbps,
        blockEncodeMs, static_cast<double>(blockGenerateUs) / 1000.0,
        realtime);

    char detail[64];
    snprintf(detail, sizeof(detail), "%.2fx  %llu bytes",
             realtime, (unsigned long long)blockBytes);
    vs070DrawProgress(block + 1, totalBlocks, detail);

    // Do not trap the user in a multi-minute benchmark if this particular
    // Arduino Opus build cannot keep up with realtime on the S3.
    if (blockEncodeUs > static_cast<uint64_t>(VS070_BLOCK_SECONDS) * 1000000ULL) {
      Serial.println("OPUS TEST: STOP early; encoder is slower than realtime");
      break;
    }
    delay(10);
  }

  const uint32_t wallMs = millis() - wallStarted;
  const double audioSeconds = completedFrames * (VS070_FRAME_MS / 1000.0);
  const double encodeMs = static_cast<double>(totalEncodeUs) / 1000.0;
  const double realtime = encodeMs > 0.0 ? (audioSeconds * 1000.0) / encodeMs : 0.0;
  const double kbps = audioSeconds > 0.0
      ? (static_cast<double>(totalBytes) * 8.0) / (audioSeconds * 1000.0) : 0.0;
  Serial.printf(
      "OPUS TEST: DONE audio=%.1fs frames=%d bytes=%llu kbps=%.2f "
      "encode=%.1fms gen=%.1fms wall=%lums realtime=%.2fx packet=%lu..%lu\n",
      audioSeconds, completedFrames, (unsigned long long)totalBytes, kbps,
      encodeMs, static_cast<double>(totalGenerateUs) / 1000.0,
      (unsigned long)wallMs, realtime,
      (unsigned long)(minPacket == UINT32_MAX ? 0 : minPacket),
      (unsigned long)maxPacket);

  opus_encoder_destroy(encoder);
  screenDirty = true;
}

struct Vs070HookInstaller {
  Vs070HookInstaller() { vsSyntheticTestHook = &vs070MenuOpusTest; }
};
static Vs070HookInstaller vs070HookInstaller;

#endif  // VISITESCRIBE_OPUS_EXPERIMENT
