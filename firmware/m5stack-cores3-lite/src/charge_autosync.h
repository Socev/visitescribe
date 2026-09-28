#pragma once
#include "charge_detector.h"

static VsChargeDetector vsChargeDetector;
static uint32_t vsChargeLastSample = 0;
static uint32_t vsChargeLastSecond = 0;
static bool vsChargeFromUsb = false;

static void vsChargeLog(const char* event) {
  // Called only while idle: never contend with the recording writer.
  Serial.printf("CHARGE: %s source=%s battery=%dmV reference=%dmV\n", event,
      vsChargeFromUsb ? "USB" : "voltage", M5.Power.getBatteryVoltage(),
      vsChargeDetector.reference);
  if (!sdOk || captureRunning || sessionOpen || vsServerSyncRunning) return;
  File f = SD.open("/visitescribe/charge_sync_log.csv", FILE_APPEND);
  if (f) {
    f.printf("%lu,%s,%s,%d,%d\n", (unsigned long)millis(), event,
        vsChargeFromUsb ? "USB" : "voltage", M5.Power.getBatteryVoltage(),
        vsChargeDetector.reference);
    f.flush(); f.close();
  }
}
static void vsChargeCancelled() { vsChargeLog("cancelled"); }
static void vsSuspendChargeAutoSync() {
  vsChargeDetector.suspend();
  chargeRiseBeingConfirmed = false;
  // USB maintenance owns Serial and the UI; EXIT restores HOME.
  chargeSyncStartedMs = 0;
}
static void vsServiceChargeAutoSync() {
  if (vsPowerProbeActive) { vsSuspendChargeAutoSync(); return; }
  const uint32_t now = millis();
  if (state == AppState::CHARGE_SYNC) {
    if (vsChargeFromUsb && M5.Power.getVBUSVoltage() < 4000) {
      cancelChargeSync(); return;
    }
    const uint32_t second = (now - chargeSyncStartedMs) / 1000;
    if (second != vsChargeLastSecond) { vsChargeLastSecond = second; screenDirty = true; }
    if (now - chargeSyncStartedMs >= CHARGE_SYNC_DELAY_MS) {
      vsChargeLog("sync_start");
      chargeSyncStartedMs = 0;
      pwrClickPendingV03 = false; pwrFirstClickMsV03 = 0;
      vsServerStage = VsServerStage::IDLE;
      beginSync();
    }
    return;
  }
  bool eligible = state == AppState::HOME && sdOk && !captureRunning &&
      !sessionOpen && !vsServerSyncRunning && WiFi.getMode() == WIFI_OFF;
#ifdef VISITESCRIBE_DIRECT_OPUS
  eligible = eligible && vsDoWorkerDone;
#endif
  if (!eligible) chargeRiseBeingConfirmed = false;
  if (now - vsChargeLastSample < 1000) return;
  vsChargeLastSample = now;
  const auto event = vsChargeDetector.sample(now, M5.Power.getBatteryVoltage(),
      eligible, (int)displayPower, M5.Power.getVBUSVoltage() >= 4000);
  chargeRiseBeingConfirmed = eligible && vsChargeDetector.candidate;
  if (event == VsChargeDetector::NONE) return;
  vsChargeFromUsb = event == VsChargeDetector::USB_POWER;
  vsChargeLog("countdown");
  vsChargeCancelHook = vsChargeCancelled;
  chargeRiseBeingConfirmed = false;
  noteActivity();
  state = AppState::CHARGE_SYNC;
  chargeSyncStartedMs = millis(); vsChargeLastSecond = 0;
  pwrClickPendingV03 = false; pwrFirstClickMsV03 = 0;
  screenDirty = true;
}
