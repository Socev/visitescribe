#pragma once
#include <stdint.h>
struct VsSyncResultTimer {
  bool armed = false;
  uint32_t since = 0;
  bool update(bool successfulAndIdle, uint32_t now) {
    if (!successfulAndIdle) { armed = false; return false; }
    if (!armed) { armed = true; since = now; }
    return uint32_t(now - since) >= 10000;
  }
};
