#pragma once
#include <stdint.h>

static constexpr uint64_t VS_RETENTION_SECONDS = 72ULL * 60 * 60;
static constexpr bool vsRetentionDue(bool ingested, uint64_t confirmed, uint64_t now) {
  return ingested && confirmed >= 1704067200ULL && now >= confirmed &&
         now - confirmed >= VS_RETENTION_SECONDS;
}
static constexpr uint32_t vsSyncCurrentOrdinal(uint32_t handled, uint32_t total) {
  return handled < total ? handled + 1 : total;
}

// Compile-time boundary/regression checks run in every firmware build.
static_assert(!vsRetentionDue(true, 1800000000, 1800259199), "Keep until full 72h");
static_assert(vsRetentionDue(true, 1800000000, 1800259200), "Eligible at 72h");
static_assert(!vsRetentionDue(false, 1800000000, 1800300000), "Never purge queued/quarantined");
static_assert(!vsRetentionDue(true, 0, 1800300000), "Unknown date is not old");
static_assert(!vsRetentionDue(true, 1800000000, 0), "No trustworthy clock");
static_assert(!vsRetentionDue(true, 1800000000, 1799999999), "Clock rollback keeps audio");
static_assert(vsSyncCurrentOrdinal(2, 4) == 3, "Third stays third through confirmation");
static_assert(vsSyncCurrentOrdinal(3, 4) == 4, "Advance after confirmation");
static_assert(vsSyncCurrentOrdinal(4, 4) == 4, "Never show five of four");
static_assert(vsSyncCurrentOrdinal(0, 0) == 0, "Empty queue");
