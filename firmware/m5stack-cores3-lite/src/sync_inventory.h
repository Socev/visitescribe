#pragma once
#include <map>
#include "sync_directory.h"

struct VsInventoryOrder {
  bool operator()(const String& a, const String& b) const { return a.compareTo(b) < 0; }
};
struct VsInventoryEntry {
  bool events = false;
  std::vector<String> wavs;
};

// One directory walk per inventory. No per-session WAV directory rescans.
// Prepared sessions are scoped to this sync pass, never cached across recording,
// USB recovery or retention. The uploader still checks each chunk on read.
static std::vector<String> vsPendingPrefixes(std::vector<VsLocalSession>* prepared = nullptr,
                                              const std::vector<String>* snapshot = nullptr) {
  const uint32_t started = millis();
  if (prepared) prepared->clear();
  std::vector<String> prefixes;
  std::map<String, VsInventoryEntry, VsInventoryOrder> entries;
  std::vector<String> ownedFiles;
  const bool shared = snapshot != nullptr;
  if (!snapshot) {
    if (!vsReadSessionDirectory(ownedFiles)) return prefixes;
    snapshot = &ownedFiles;
  }
  for (const auto& name : *snapshot) {
    const String prefix = name.substring(0, 6);
    if (name.length() > 7 && name[6] == '_' && vsRetentionValidPrefix(prefix)) {
      if (name == prefix + "_events.csv") entries[prefix].events = true;
      else if (name.endsWith(".wav")) entries[prefix].wavs.push_back(String("/visitescribe/") + name);
    }
  }
  for (auto& entry : entries) {
    if (!entry.second.events) continue;
    const String& prefix = entry.first;
    String uuid, st;
    if (vsReadSyncMeta(prefix, uuid, st) && vsLocalSyncStateTerminal(st)) continue;
    VsLocalSession local;
    local.prefix = prefix;
    local.eventsPath = String("/visitescribe/") + prefix + "_events.csv";
    if (!vsEventsShowComplete(local.eventsPath)) continue;
#ifdef VISITESCRIBE_DIRECT_OPUS
    local.opus = vsDirectOpusChunksForPrefix(prefix);
    local.directOpus = !local.opus.empty();
    if (local.directOpus) {
      if (prepared) local.mode = vsDirectOpusModeFromEvents(local.eventsPath);
    } else
#endif
    {
      local.wavs = std::move(entry.second.wavs);
      if (local.wavs.empty()) continue;
      std::sort(local.wavs.begin(), local.wavs.end(), VsInventoryOrder());
      if (prepared) local.mode = vsModeFromWavs(local.wavs);
    }
    prefixes.push_back(prefix);
    if (prepared) prepared->push_back(std::move(local));
  }
  Serial.printf("SERVER: inventory directory_passes=%u files=%lu pending=%u elapsed=%lums\n",
                shared ? 0 : 1, (unsigned long)snapshot->size(), (unsigned)prefixes.size(), (unsigned long)(millis() - started));
  return prefixes;
}
