#pragma once
// Explicit USB maintenance diagnostic: bounded to 60s; no charger settings change.
static bool vsPowerProbeActive = false;
static uint32_t vsPowerProbeStarted = 0, vsPowerProbeLast = 0;
static bool vsStartPowerProbe() {
  if (!sdOk || captureRunning || sessionOpen || vsServerSyncRunning) return false;
  File f = SD.open("/visitescribe/power_probe.csv", FILE_WRITE);
  if (!f) return false;
  const char* header = "elapsed_ms,axp00,axp01,vbus_mv,battery_mv,charge_status\n";
  const bool ok = f.print(header) == strlen(header);
  f.flush(); f.close();
  vsPowerProbeActive = ok;
  vsPowerProbeStarted = vsPowerProbeLast = millis();
  return ok;
}
static void vsServicePowerProbe() {
  if (!vsPowerProbeActive) return;
  const uint32_t now = millis();
  if (now - vsPowerProbeStarted > 60000) { vsPowerProbeActive = false; return; }
  if (now - vsPowerProbeLast < 1000 || captureRunning || sessionOpen || vsServerSyncRunning) return;
#ifdef VISITESCRIBE_DIRECT_OPUS
  if (!vsDoWorkerDone) return;
#endif
  vsPowerProbeLast = now;
  const unsigned a00 = M5.Power.Axp2101.readRegister8(0x00);
  const unsigned a01 = M5.Power.Axp2101.readRegister8(0x01);
  const int vbus = M5.Power.getVBUSVoltage();
  const int battery = M5.Power.getBatteryVoltage();
  const int charge = M5.Power.Axp2101.getChargeStatus();
  File f = SD.open("/visitescribe/power_probe.csv", FILE_APPEND);
  if (!f) { vsPowerProbeActive = false; return; }
  f.printf("%lu,%u,%u,%d,%d,%d\n", (unsigned long)(now - vsPowerProbeStarted),
           a00, a01, vbus, battery, charge);
  f.flush(); f.close();
}
