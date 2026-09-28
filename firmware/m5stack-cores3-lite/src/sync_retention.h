#pragma once
#include "sync_directory.h"
// Called only with recorder/worker stopped; all paths stay in /visitescribe.
static bool vsRetentionValidPrefix(const String& prefix) {
  if (prefix.length() != 6 || prefix[0] != 's') return false;
  for (size_t i = 1; i < 6; ++i) if (prefix[i] < '0' || prefix[i] > '9') return false;
  return true;
}

static uint64_t vsRetentionReadStamp(const String& prefix, const String& uuid) {
  File f = SD.open(String("/visitescribe/") + prefix + "_synced_at.txt", FILE_READ);
  if (!f) return 0;
  String savedUuid, text;
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    if (line.startsWith("uuid=")) savedUuid = line.substring(5);
    if (line.startsWith("confirmed_at=")) text = line.substring(13);
  }
  f.close();
  if (savedUuid != uuid || text.length() != 10) return 0;
  uint64_t value = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    if (text[i] < '0' || text[i] > '9') return 0;
    value = value * 10 + (text[i] - '0');
  }
  return value >= 1704067200ULL ? value : 0;
}

static void vsRetentionRecordConfirmation(const String& prefix, const String& uuid) {
  const uint64_t now = vsRetentionNow();
  if (!now || !vsRetentionValidPrefix(prefix) || uuid.length() != 36 ||
      vsRetentionReadStamp(prefix, uuid)) return;
  const String path = String("/visitescribe/") + prefix + "_synced_at.txt";
  const String stage = path + ".writing";
  char epoch[24]; snprintf(epoch, sizeof(epoch), "%llu", (unsigned long long)now);
  const String body = String("uuid=") + uuid + "\nconfirmed_at=" + epoch + "\n";
  File f = SD.open(stage, FILE_WRITE);
  if (!f) return;
  const bool ok = f.print(body) == body.length();
  f.flush(); f.close();
  if (!ok) return;
  // Missing/corrupt timestamps get a NEW full 72h grace period, never an estimate.
  if (SD.exists(path) && !SD.remove(path)) return;
  if (!SD.rename(stage, path)) return;
  Serial.printf("RETENTION: %s 72h grace period started\n", prefix.c_str());
}

static void vsRetentionCleanup(const std::vector<String>* snapshot = nullptr) {
  const uint64_t now = vsRetentionNow();
  if (!sdOk || !now || captureRunning || sessionOpen) return;
#ifdef VISITESCRIBE_DIRECT_OPUS
  if (!vsDoWorkerDone) return;
#endif
  std::vector<String> ownedFiles, prefixes;
  if (!snapshot) {
    if (!vsReadSessionDirectory(ownedFiles)) return;
    snapshot = &ownedFiles;
  }
  const auto& files = *snapshot;
  for (const auto& name : files) {
    if (name == name.substring(0, 6) + "_sync.txt" &&
        vsRetentionValidPrefix(name.substring(0, 6))) prefixes.push_back(name.substring(0, 6));
  }
  for (const auto& prefix : prefixes) {
    String uuid, syncState;
    if (!vsReadSyncMeta(prefix, uuid, syncState) || syncState != "ingested") continue;
    const uint64_t confirmed = vsRetentionReadStamp(prefix, uuid);
    if (!confirmed) {
      // Includes old firmware and USB confirmations made without network time.
      vsRetentionRecordConfirmation(prefix, uuid);
      continue;
    }
    if (!vsRetentionDue(true, confirmed, now)) continue;
    const String syncName = prefix + "_sync.txt";
    const String eventsName = prefix + "_events.csv";
    const String stampName = prefix + "_synced_at.txt";
    bool ok = true;
    for (const auto& name : files) {
      if (!name.startsWith(prefix + "_") || name == syncName ||
          name == eventsName || name == stampName) continue;
      const String path = String("/visitescribe/") + name;
      if (!SD.remove(path)) { ok = false; break; }
    }
    // Preserve receipt until EVERYTHING else is gone. Interrupted/failed cleanup
    // stays excluded from upload; findNextSessionId reserves this prefix too.
    if (ok) {
      const String events = String("/visitescribe/") + eventsName;
      ok = !SD.exists(events) || SD.remove(events);
    }
    if (ok) ok = SD.remove(String("/visitescribe/") + stampName);
    if (ok) ok = SD.remove(String("/visitescribe/") + syncName);
    Serial.printf("RETENTION: %s %s\n", prefix.c_str(),
                  ok ? "removed after 72h confirmed sync" : "cleanup incomplete; receipt retained");
  }
}
