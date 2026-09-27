// VisiteScribe CoreS3-Lite v0.6.1
//
// TLS certificate verification on ESP32 depends on a sane system clock.
// v0.6 could reach HTTPS immediately after Wi-Fi association while the clock
// was still near the Unix epoch, causing X509 verification to fail before the
// request ever reached the API.
//
// Keep the v0.6.1 clock/NTP fix, routes the production sync leaf
// to v0.6.7, attaches the temporary v0.6.9 synthetic transport probe, and can
// optionally attach isolated Opus experiments.
//
// The build-time seed is not the long-term time source; SNTP is. It merely
// prevents the first TLS handshake from running with an obviously invalid date.

#include <Arduino.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

static int vsMonthIndex(const char* mon) {
  static const char* months[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };
  for (int i = 0; i < 12; ++i) {
    if (strncmp(mon, months[i], 3) == 0) return i;
  }
  return 0;
}

static time_t vsBuildEpoch() {
  // __DATE__ = "Mmm dd yyyy", __TIME__ = "hh:mm:ss".
  char mon[4] = {__DATE__[0], __DATE__[1], __DATE__[2], 0};
  struct tm t = {};
  t.tm_mon = vsMonthIndex(mon);
  t.tm_mday = atoi(__DATE__ + 4);
  t.tm_year = atoi(__DATE__ + 7) - 1900;
  t.tm_hour = atoi(__TIME__);
  t.tm_min = atoi(__TIME__ + 3);
  t.tm_sec = atoi(__TIME__ + 6);
  t.tm_isdst = 0;

  // Interpret the compile timestamp as UTC. A small timezone error would not
  // matter for certificate validity, but keeping this deterministic does.
  setenv("TZ", "UTC0", 1);
  tzset();
  return mktime(&t);
}

static void vsSeedClockFromBuild() {
  const time_t build = vsBuildEpoch();
  const time_t now = time(nullptr);
  // Never move a plausible clock backwards. Only repair clearly stale time.
  if (build > 0 && now + 300 < build) {
    struct timeval tv = {};
    tv.tv_sec = build;
    settimeofday(&tv, nullptr);
  }
}

void initVariant() {
  vsSeedClockFromBuild();

  // Starting SNTP before Wi-Fi is fine: it will obtain time once networking
  // becomes available. Three independent servers keep this usable on normal
  // home, practice and hotspot networks.
  configTime(0, 0,
             "time.cloudflare.com",
             "pool.ntp.org",
             "time.google.com");
}

#include "main_v067.cpp"
#include "main_v069_probe.cpp"

// The old Arduino Opus benchmark and the Espressif load probe intentionally
// share the existing OPUS TEST menu label, but never compile into the same
// build. Production has neither macro and therefore neither dependency.
#if defined(VISITESCRIBE_OPUS_EXPERIMENT) && !defined(VISITESCRIBE_OPUS_ESP_LOAD_EXPERIMENT)
#include "main_v070_opus_probe.cpp"
#endif
#ifdef VISITESCRIBE_OPUS_ESP_LOAD_EXPERIMENT
#include "main_v071_opus_esp_load.cpp"
#endif
