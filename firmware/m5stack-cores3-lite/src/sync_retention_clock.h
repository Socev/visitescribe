#pragma once
#include <atomic>
#include <time.h>
#include <esp_sntp.h>

// Build-time clock seeding is sufficient for initial TLS, never for deletion.
static std::atomic<bool> vsRetentionHasNetworkTime{false};
static void vsRetentionTimeReceived(struct timeval*) {
  vsRetentionHasNetworkTime.store(true);
}
static uint64_t vsRetentionNow() {
  const time_t now = time(nullptr);
  return vsRetentionHasNetworkTime.load() && now >= 1704067200
      ? static_cast<uint64_t>(now) : 0;
}
