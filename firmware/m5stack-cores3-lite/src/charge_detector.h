#pragma once
#include <stdint.h>

// Voltage is a heuristic, NOT a hardware charger-present indication.
// Caller samples at 1 Hz under a known, idle load; no recording/network activity.
struct VsChargeDetector {
  enum Event { NONE, USB_POWER, VOLTAGE_RISE };
  bool usbKnown=false, usb=false, latched=false, usbLatch=false;
  bool tracking=false, candidate=false, falling=false;
  int context=0, reference=0, filtered=0, peak=0, count=0, sum=0;
  uint32_t contextAt=0, candidateAt=0, fallingAt=0;
  void resetTracking() {
    tracking=false; candidate=false; falling=false;
    reference=filtered=peak=count=sum=0;
  }
  void suspend() { resetTracking(); usbKnown=false; }
  Event sample(uint32_t now, int mv, bool eligible, int loadContext, bool vbus) {
    bool attached=false;
    if (!usbKnown) { usbKnown=true; usb=vbus; if (vbus) { latched=true; usbLatch=true; } else if (usbLatch) { latched=false; usbLatch=false; } }
    else if (usb != vbus) {
      attached=vbus; usb=vbus; resetTracking();
      latched=vbus; usbLatch=vbus;
    }
    if (!eligible || mv<3000 || mv>4400) { resetTracking(); return NONE; }
    if (attached) return USB_POWER;
    if (vbus) { resetTracking(); return NONE; }
    if (!tracking || context != loadContext) {
      resetTracking(); tracking=true; context=loadContext; contextAt=now;
      return NONE;
    }
    // Let a known load transition settle, then form a five-sample baseline.
    if (uint32_t(now-contextAt)<3000) return NONE;
    if (count<5) {
      sum+=mv; ++count;
      if (count==5) reference=filtered=peak=sum/5;
      return NONE;
    }
    filtered=(filtered*3+mv)/4;
    if (latched) {
      if (filtered>peak) peak=filtered;
      if (filtered<=peak-25) {
        if (!falling) { falling=true; fallingAt=now; }
        if (uint32_t(now-fallingAt)>=10000) {
          latched=false; usbLatch=false; reference=filtered; candidate=false;
          falling=false; peak=filtered;
        }
      } else falling=false;
      return NONE;
    }
    if (candidate) {
      if (mv<reference+15) { candidate=false; return NONE; }
      if (uint32_t(now-candidateAt)>=10000 && filtered>=reference+20) {
        latched=true; usbLatch=false; candidate=false; peak=filtered;
        return VOLTAGE_RISE;
      }
      if (uint32_t(now-candidateAt)>=30000) { candidate=false; reference=filtered; }
      return NONE;
    }
    if (filtered<reference) reference=filtered;
    if (mv>=reference+25) { candidate=true; candidateAt=now; }
    return NONE;
  }
};
