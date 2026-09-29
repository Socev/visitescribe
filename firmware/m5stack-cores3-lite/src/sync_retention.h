#pragma once
#include "sync_directory.h"
#include "action_marker.h"
// Deleting recordings the server has confirmed (firmware 0.10.0).
//
// Until 0.9.x a confirmed recording stayed on the SD card for 72 hours. Now a
// successful sync means delete: as soon as the server has confirmed ingest
// (the session's _sync.txt says state=ingested) the recording goes. Only that
// state counts -- queued, failed and quarantined recordings are never touched.
//
// Order matters for a reset half-way: audio first, then the events file, and
// the _sync.txt receipt last. While the receipt exists the prefix is still
// known as "ingested", so it is never uploaded again and the next pass simply
// finishes the job. The _busy_del marker makes the next boot schedule that pass.
//
// Called only with the recorder/worker stopped; all paths stay in /visitescribe.

static bool vsRetentionValidPrefix(const String& prefix) {
  if (prefix.length() != 6 || prefix[0] != 's') return false;
  for (size_t i = 1; i < 6; ++i) if (prefix[i] < '0' || prefix[i] > '9') return false;
  return true;
}

// Kept as the hook v0.6.6 calls when it writes state=ingested. Deleting right
// there would pull files out from under the upload that is still finishing,
// so it only asks for a pass once the sync is over.
static bool vsPurgeDue = false;
static void vsRetentionRecordConfirmation(const String& prefix, const String& uuid) {
  (void)uuid;
  if (vsRetentionValidPrefix(prefix)) vsPurgeDue = true;
}

// Returns the number of recordings removed.
static uint32_t vsRetentionCleanup(const std::vector<String>* snapshot = nullptr) {
  if (!sdOk || captureRunning || sessionOpen) return 0;
#ifdef VISITESCRIBE_DIRECT_OPUS
  if (!vsDoWorkerDone) return 0;
#endif
  std::vector<String> ownedFiles, prefixes;
  if (!snapshot) {
    if (!vsReadSessionDirectory(ownedFiles)) return 0;
    snapshot = &ownedFiles;
  }
  const auto& files = *snapshot;
  for (const auto& name : files) {
    if (name == name.substring(0, 6) + "_sync.txt" &&
        vsRetentionValidPrefix(name.substring(0, 6))) {
      String uuid, syncState;
      if (vsReadSyncMeta(name.substring(0, 6), uuid, syncState) && syncState == "ingested")
        prefixes.push_back(name.substring(0, 6));
    }
  }
  vsPurgeDue = false;
  if (prefixes.empty()) {
    vsMarkerClear("del");        // a cut-off pass that had already finished
    return 0;
  }

  vsMarkerSet("del", prefixes.front());
  uint32_t removed = 0;
  bool allOk = true;
  for (const auto& prefix : prefixes) {
    const String syncName = prefix + "_sync.txt";
    const String eventsName = prefix + "_events.csv";
    const String stampName = prefix + "_synced_at.txt";      // written by 0.9.x
    bool ok = true;
    for (const auto& name : files) {
      if (!name.startsWith(prefix + "_") || name == syncName ||
          name == eventsName || name == stampName) continue;
      const String path = String("/visitescribe/") + name;
      if (SD.exists(path) && !SD.remove(path)) { ok = false; break; }
    }
    // Preserve the receipt until EVERYTHING else is gone. An interrupted or
    // failed delete stays excluded from upload; findNextSessionId reserves
    // this prefix too.
    if (ok) {
      const String events = String("/visitescribe/") + eventsName;
      ok = !SD.exists(events) || SD.remove(events);
    }
    if (ok) {
      const String stamp = String("/visitescribe/") + stampName;
      ok = !SD.exists(stamp) || SD.remove(stamp);
    }
    if (ok) ok = SD.remove(String("/visitescribe/") + syncName);
    Serial.printf("PURGE: %s %s\n", prefix.c_str(),
                  ok ? "removed after confirmed sync" : "delete incomplete; receipt kept, retried later");
    if (ok) ++removed; else allOk = false;
  }
  if (allOk) vsMarkerClear("del");
  else vsPurgeDue = true;
  return removed;
}
