// VisiteScribe CoreS3-Lite v0.6.5
//
// Diagnostic wrapper for the remaining server-sync PREPARE failure.
// Keeps v0.6.4 behaviour unchanged and adds targeted serial diagnostics:
// - list /visitescribe immediately before a sync queue starts;
// - after "Audio chunk voorbereiden mislukt", inspect the current session WAVs;
// - report physical file size, WAV header fields, expected chunk sizes and
//   single-read versus chunked-read results using the already reserved PSRAM.
//
// This revision is intentionally diagnostic: no server/API protocol or audio
// data on SD is modified.

#define setup setup_v064
#define loop loop_v064
#include "main_v064.cpp"
#undef loop
#undef setup

static bool vs065QueueDumped = false;
static bool vs065PrepareFailureDumped = false;

static void vs065ListVisiteScribeDir() {
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

static void vs065InspectWavRead(const String& path) {
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

  const uint32_t wanted = h.byteRate * VS_SYNC_CHUNK_SECONDS;
  uint32_t dataBytes = h.dataSize < wanted ? h.dataSize : wanted;
  if (h.blockAlign) dataBytes -= dataBytes % h.blockAlign;
  const size_t plainNeeded = sizeof(WAVHeader) + wanted;
  const size_t firstChunkBytes = sizeof(WAVHeader) + dataBytes;
  const size_t physicalPayload = physicalBytes > sizeof(WAVHeader)
      ? physicalBytes - sizeof(WAVHeader) : 0;

  Serial.printf(
      "SERVER DEBUG: WAV path=%s physical=%u headerData=%u payloadPhysical=%u rate=%u channels=%u bits=%u byteRate=%u blockAlign=%u\n",
      path.c_str(),
      (unsigned)physicalBytes,
      (unsigned)h.dataSize,
      (unsigned)physicalPayload,
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

  // vsReadWavHeader() left the file position directly after the 44-byte header.
  size_t gotSingle = wav.read(vs064SharedSyncBuffer + sizeof(WAVHeader), dataBytes);
  Serial.printf("SERVER DEBUG: single-read requested=%u got=%u remainingAvailable=%u\n",
                (unsigned)dataBytes,
                (unsigned)gotSingle,
                (unsigned)wav.available());

  // Retry the same payload as modest reads. This distinguishes a large VFS/SD
  // short-read from malformed/truncated source audio without changing the file.
  if (!wav.seek(sizeof(WAVHeader))) {
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

static void vs065DumpPrepareFailure() {
  Serial.printf(
      "SERVER DEBUG: PREPARE failure session=%s error=%s psram_total=%u psram_free=%u psram_largest=%u shared=%p ready=%d\n",
      vsServerSessionPrefix.c_str(),
      vsServerError.c_str(),
      (unsigned)ESP.getPsramSize(),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
      vs064SharedSyncBuffer,
      vs064SharedReady ? 1 : 0);

  vs065ListVisiteScribeDir();

  if (!vsServerSessionPrefix.length()) {
    Serial.println("SERVER DEBUG: geen actuele sessieprefix");
    return;
  }

  auto wavs = vsCollectWavs(vsServerSessionPrefix);
  Serial.printf("SERVER DEBUG: sessie %s heeft %u WAV(s)\n",
                vsServerSessionPrefix.c_str(), (unsigned)wavs.size());
  for (const auto& path : wavs) {
    vs065InspectWavRead(path);
  }
  Serial.println("SERVER DEBUG: einde PREPARE failure dump");
}

void setup() {
  setup_v064();
  Serial.println("VisiteScribe CoreS3-Lite v0.6.5 sync diagnostics enabled");
}

void loop() {
  const bool aboutToStartQueue =
      state == AppState::SYNC &&
      syncPhase == SyncPhase::CONNECTED &&
      vsServerStage == VsServerStage::IDLE &&
      !vsServerSyncRunning;

  if (aboutToStartQueue && !vs065QueueDumped) {
    vs065QueueDumped = true;
    vs065PrepareFailureDumped = false;
    Serial.println("SERVER DEBUG: sync queue gaat starten");
    vs065ListVisiteScribeDir();
  }

  loop_v064();

  if (vsServerStage == VsServerStage::ERROR &&
      vsServerError.indexOf("Audio chunk voorbereiden mislukt") >= 0 &&
      !vs065PrepareFailureDumped) {
    vs065PrepareFailureDumped = true;
    vs065DumpPrepareFailure();
  }

  if (state != AppState::SYNC || syncPhase != SyncPhase::CONNECTED) {
    vs065QueueDumped = false;
  }
  if (vsServerStage != VsServerStage::ERROR) {
    vs065PrepareFailureDumped = false;
  }
}
