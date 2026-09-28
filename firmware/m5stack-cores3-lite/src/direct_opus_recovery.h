#pragma once
// Recovery runs only while capture/worker are stopped. Originals are preserved.
static uint32_t vsDoRecoveredOnBoot = 0;
static uint8_t vsDoRecoveryPage[8192];

struct VsDoOggScan {
  uint32_t bytes = 0, lastPage = 0, durationMs = 0;
  bool eos = false;
};

static uint32_t vsDoReadLe32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static VsDoOggScan vsDoScanOgg(const String& path) {
  VsDoOggScan result;
  File f = SD.open(path, FILE_READ);
  if (!f) return result;
  uint32_t sequence = 0, serial = 0;
  uint16_t preSkip = 0;
  while (f.available()) {
    const uint32_t offset = f.position();
    uint8_t* page = vsDoRecoveryPage;
    if (f.read(page, 27) != 27 || memcmp(page, "OggS", 4) || page[4] != 0) break;
    const size_t segments = page[26];
    if (!segments || (page[5] & 1) || f.read(page + 27, segments) != segments) break;
    size_t body = 0;
    for (size_t i = 0; i < segments; ++i) body += page[27 + i];
    const size_t size = 27 + segments + body;
    if (size > sizeof(vsDoRecoveryPage) || page[26 + segments] == 255 ||
        f.read(page + 27 + segments, body) != body) break;
    if (vsDoReadLe32(page + 18) != sequence) break;
    const uint32_t expectedCrc = vsDoReadLe32(page + 22);
    memset(page + 22, 0, 4);
    if (vsDoOggCrc(page, size) != expectedCrc) break;
    const uint8_t* packet = page + 27 + segments;
    if (sequence == 0) {
      if (!(page[5] & 2) || body != 19 || memcmp(packet, "OpusHead", 8) || packet[9] != 1) break;
      serial = vsDoReadLe32(page + 14);
      preSkip = uint16_t(packet[10]) | (uint16_t(packet[11]) << 8);
    } else if (vsDoReadLe32(page + 14) != serial) break;
    if (sequence == 1 && (body < 8 || memcmp(packet, "OpusTags", 8))) break;
    const uint64_t granule = uint64_t(vsDoReadLe32(page + 6)) |
                            (uint64_t(vsDoReadLe32(page + 10)) << 32);
    if (sequence >= 2) {
      if (granule <= preSkip || granule > uint64_t(30000) * 48 + preSkip) break;
      const uint32_t duration = (granule - preSkip) / 48;
      if (duration < result.durationMs) break;
      result.durationMs = duration;
    }
    result.lastPage = offset;
    result.bytes = f.position();
    result.eos = (page[5] & 4) && result.bytes == f.size();
    ++sequence;
    if (page[5] & 4) break;
  }
  f.close();
  return result;
}

// Recover only complete, CRC-valid pages; truncate torn writes in the COPY.
// The original .tmp is never modified, renamed or deleted.
static bool vsDoSalvageTmp(const String& tmp, const String& finalPath) {
  const VsDoOggScan scan = vsDoScanOgg(tmp);
  if (!scan.durationMs || !scan.bytes || SD.exists(finalPath)) return false;
  const String stage = finalPath + ".recovering";
  File input = SD.open(tmp, FILE_READ);
  File output = SD.open(stage, FILE_WRITE);
  if (!input || !output) return false;
  uint32_t copied = 0;
  bool ok = true;
  while (copied < scan.lastPage) {
    const size_t n = std::min(size_t(1024), size_t(scan.lastPage - copied));
    if (input.read(vsDoRecoveryPage, n) != n || output.write(vsDoRecoveryPage, n) != n) { ok = false; break; }
    copied += n;
  }
  const size_t lastSize = scan.bytes - scan.lastPage;
  if (ok && input.read(vsDoRecoveryPage, lastSize) == lastSize) {
    vsDoRecoveryPage[5] |= 4; // EOS at last durable packet; no invented audio.
    memset(vsDoRecoveryPage + 22, 0, 4);
    vsDoPutLe32(vsDoRecoveryPage + 22, vsDoOggCrc(vsDoRecoveryPage, lastSize));
    ok = output.write(vsDoRecoveryPage, lastSize) == lastSize;
  } else ok = false;
  output.flush(); output.close(); input.close();
  const VsDoOggScan check = vsDoScanOgg(stage);
  return ok && check.eos && check.durationMs && SD.rename(stage, finalPath);
}

static bool vsDoValidPrefix(const String& prefix) {
  if (prefix.length() != 6 || prefix[0] != 's') return false;
  for (int i = 1; i < 6; ++i) if (prefix[i] < '0' || prefix[i] > '9') return false;
  return true;
}

static bool vsDoRecoverSession(const String& prefix) {
  if (!vsDoWorkerDone || captureRunning || !vsDoValidPrefix(prefix)) return false;
  const String base = String("/visitescribe/") + prefix;
  if (SD.exists(base + "_recovered.txt")) return true;
  // A sync UUID may already have an authenticated manifest. Never rewrite it.
  if (SD.exists(base + "_sync.txt") || !SD.exists(base + "_events.csv")) return false;
  std::vector<String> files;
  File dir = SD.open("/visitescribe");
  if (!dir) return false;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String name = f.name();
    name = name.substring(name.lastIndexOf('/') + 1);
    if (name.startsWith(prefix + "_chunk_") &&
        (name.endsWith(".opus") || name.endsWith(".opus.tmp"))) files.push_back(String("/visitescribe/") + name);
    f.close();
  }
  dir.close();
  for (const auto& path : files) {
    if (path.endsWith(".tmp")) {
      const String finalPath = path.substring(0, path.length() - 4);
      if (!SD.exists(finalPath) && vsDoSalvageTmp(path, finalPath)) {
        Serial.printf("RECOVERY: salvaged %s; original tmp retained\n", finalPath.c_str());
      }
    }
  }
  // Enumerate exact sequence names; gaps block publication instead of renumbering.
  uint32_t maxSequence = 0;
  for (const auto& path : files) {
    const int at = path.indexOf("_chunk_") + 7;
    maxSequence = std::max(maxSequence, uint32_t(path.substring(at, at + 6).toInt()));
  }
  std::vector<VsDirectOpusChunkInfo> rows;
  uint32_t duration = 0;
  for (uint32_t seq = 1; seq <= maxSequence; ++seq) {
    char suffix[40]; snprintf(suffix, sizeof(suffix), "_chunk_%06lu.opus", (unsigned long)seq);
    const String path = base + suffix;
    if (!SD.exists(path)) {
      if (seq != maxSequence) return false; // allow an unrecoverable final tmp
      break;
    }
    const VsDoOggScan scan = vsDoScanOgg(path);
    if (!scan.eos || !scan.durationMs) return false;
    VsDirectOpusChunkInfo row;
    row.sequence = seq; row.path = path; row.durationMs = scan.durationMs; row.bytes = scan.bytes;
    rows.push_back(row); duration += scan.durationMs;
  }
  if (rows.empty()) return false;
  const String meta = base + "_opus.csv";
  const String stage = meta + ".recovering";
  File out = SD.open(stage, FILE_WRITE);
  if (!out) return false;
  bool ok = out.println("sequence,path,duration_ms,bytes") > 0;
  for (const auto& row : rows) {
    if (!out.printf("%lu,%s,%lu,%lu\n", (unsigned long)row.sequence, row.path.c_str(),
                    (unsigned long)row.durationMs, (unsigned long)row.bytes)) ok = false;
  }
  out.flush(); out.close();
  if (!ok) return false;
  if (SD.exists(meta)) {
    const String backup = meta + ".before-recovery";
    if (SD.exists(backup)) {
      // Retry after marker-write/reset failure: reuse byte-identical metadata.
      File a = SD.open(meta, FILE_READ), b = SD.open(stage, FILE_READ);
      bool same = a && b && a.size() == b.size();
      while (same && a.available()) if (a.read() != b.read()) same = false;
      a.close(); b.close();
      if (!same) return false; // never overwrite evidence or an altered manifest
      SD.remove(stage); // only our redundant generated staging file
    } else if (!SD.rename(meta, backup)) return false;
  }
  if (SD.exists(stage) && !SD.rename(stage, meta)) return false;
  File marker = SD.open(base + "_recovered.txt.recovering", FILE_WRITE);
  if (!marker) return false;
  ok = marker.printf("status=recovered\nchunks=%u\nduration_ms=%lu\nreset_reason=%d\noriginals_retained=1\n",
                     (unsigned)rows.size(), (unsigned long)duration, (int)esp_reset_reason()) > 0;
  marker.flush(); marker.close();
  if (!ok || !SD.rename(base + "_recovered.txt.recovering", base + "_recovered.txt")) return false;
  ++vsDoRecoveredOnBoot;
  Serial.printf("RECOVERY: %s ready for sync chunks=%u duration=%lums; interrupted recording\n",
                prefix.c_str(), (unsigned)rows.size(), (unsigned long)duration);
  return true;
}

static bool vsDoWriteActiveJournal() {
  char path[80]; snprintf(path, sizeof(path), "/visitescribe/s%05u_active.txt", vsDoSeenSessionId);
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  const bool ok = f.println("direct_opus_active=1") > 0;
  f.flush(); f.close();
  return ok;
}

static void vsDoRecoverActiveSessions() {
  if (!vsDoWorkerDone || captureRunning) return;
  std::vector<String> prefixes;
  File dir = SD.open("/visitescribe");
  if (!dir) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String name = f.name(); name = name.substring(name.lastIndexOf('/') + 1);
    if (name.endsWith("_active.txt") && vsDoValidPrefix(name.substring(0, 6))) prefixes.push_back(name.substring(0, 6));
    f.close();
  }
  dir.close();
  for (const auto& prefix : prefixes) {
    vsDoRecoverSession(prefix);
  }
}

static void vsDoClearActiveJournal() {
  char path[80]; snprintf(path, sizeof(path), "/visitescribe/s%05u_active.txt", vsDoSeenSessionId);
  if (SD.exists(path)) SD.remove(path);
}
