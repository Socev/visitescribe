#pragma once
// Idle power (firmware 0.12.0), both boards.
//
// At rest Brian used to spin its main loop every 5 ms at 240 MHz with the
// screen off, which is nearly all of its idle consumption. Now, when it is
// truly idle, it light-sleeps between loop passes:
//
//   * truly idle = HOME (or an error screen), screen off, on battery, nothing
//     recording, saving, syncing, updating, in USB sync, hotspot or pairing,
//     Wi-Fi off, and no button gesture under way;
//   * CoreS3-Lite: the PWR key cannot wake the ESP32 (it is wired to the
//     AXP2101 only), but the AXP latches a press, so Brian wakes every 200 ms,
//     reads it, and sleeps on. No press is lost; a reply is at most 0.2 s late;
//   * StickS3: both buttons wake it at once; otherwise once a second;
//   * every wake runs one normal loop pass, so charge detection, the hourly
//     clean-up, the logbook and onboarding keep working;
//   * RAM, PSRAM (the logbook ring) and all state survive: waking is instant.
//
// After 2 hours idle on battery Brian switches off for real: the CoreS3 via
// the AXP2101 (a PWR press starts it again), the StickS3 in deep sleep (either
// button starts it again). Recordings are on storage and are safe.
//
// With USB or a charger (or the Bottom3 dock) nothing sleeps: charging,
// charge auto-sync and USB sync stay exactly as they were.
//
// Every 10 minutes a POWER line goes to the logbook (battery, time asleep),
// so the effect can be measured in the admin.

#include <esp_sleep.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>

static constexpr uint32_t VS_IDLE_POWER_OFF_MS = 2UL * 60UL * 60UL * 1000UL;
static constexpr uint32_t VS_IDLE_LOG_MS = 10UL * 60UL * 1000UL;

static uint64_t vsIdleSleptUs = 0;       // since the last POWER line
static uint32_t vsIdleLogAt = 0;
static bool vsIdleTouchAsleep = false;   // CoreS3: FT6336 hibernating

static bool vsOnExternalPower() {
  if (M5.Power.getVBUSVoltage() >= 4000) return true;
  return vsChargeDetector.latched;        // CoreS3 on the Bottom3 dock
}

// Cheap checks first; the voltage read over I2C only when all else says idle.
static bool vsIdleEligible() {
  if (vs080Mode != Vs080Mode::NONE || vs080RequestPortal) return false;
  if (state != AppState::HOME && state != AppState::ERROR) return false;
  if (displayPower != DisplayPower::OFF) return false;
  if (sessionOpen || captureRunning || vsServerSyncRunning || vsUsbSyncActive) return false;
#ifdef VISITESCRIBE_DIRECT_OPUS
  if (!vsDoWorkerDone) return false;
#endif
  if (WiFi.getMode() != WIFI_OFF) return false;
  if (pwrClickPendingV03) return false;
#if VS_STICK
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed() || vsStickAClick || vsStickBClick) return false;
#endif
  return !vsOnExternalPower();
}

#if !VS_STICK
// FT6336: hibernate while Brian sleeps (touch is not used with the screen
// off). Coming back needs a reset pulse on AW9523 P0_0, which also brings it
// back when it no longer answers at all.
static void vsIdleTouchHibernate() {
  if (vsIdleTouchAsleep || !touchOk) return;
  M5.In_I2C.writeRegister8(FT6336_ADDR, 0xA5, 0x03, I2C_HZ);
  vsIdleTouchAsleep = true;
}
static void vsIdleTouchWake() {
  if (!vsIdleTouchAsleep) return;
  vsIdleTouchAsleep = false;
  M5.In_I2C.bitOff(AW9523_ADDR, 0x04, 0x01, I2C_HZ);   // P0_0 = output
  M5.In_I2C.bitOff(AW9523_ADDR, 0x02, 0x01, I2C_HZ);   // reset low
  delay(10);
  M5.In_I2C.bitOn(AW9523_ADDR, 0x02, 0x01, I2C_HZ);    // reset high
  delay(120);
  touchOk = M5.In_I2C.scanID(FT6336_ADDR, 100000);
}
#endif

static void vsIdleLightSleep() {
#if VS_STICK
  gpio_wakeup_enable(GPIO_NUM_11, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(GPIO_NUM_12, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(1000000ULL);
#else
  vsIdleTouchHibernate();
  esp_sleep_enable_timer_wakeup(200000ULL);
#endif
  const int64_t t0 = esp_timer_get_time();
  esp_light_sleep_start();
  vsIdleSleptUs += (uint64_t)(esp_timer_get_time() - t0);
#if VS_STICK
  gpio_wakeup_disable(GPIO_NUM_11);
  gpio_wakeup_disable(GPIO_NUM_12);
#endif
}

static void vsIdlePowerOff() {
  Serial.println("POWER: 2 h unused on battery; switching off (a press starts Brian again)");
  if (sdOk) vsLogFlushToSd();
  Serial.flush();
  delay(50);
#if VS_STICK
  M5.Display.sleep();
  const uint64_t mask = (1ULL << GPIO_NUM_11) | (1ULL << GPIO_NUM_12);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  rtc_gpio_pullup_en(GPIO_NUM_11);
  rtc_gpio_pulldown_dis(GPIO_NUM_11);
  rtc_gpio_pullup_en(GPIO_NUM_12);
  rtc_gpio_pulldown_dis(GPIO_NUM_12);
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
#else
  M5.Power.powerOff();
#endif
}

// Called once per main-loop pass (main_v080 loop()).
static void vsServiceIdlePower() {
  const uint32_t now = millis();

#if !VS_STICK
  if (vsIdleTouchAsleep && displayPower != DisplayPower::OFF) vsIdleTouchWake();
#endif

  if (vsIdleLogAt == 0) vsIdleLogAt = now;
  if (now - vsIdleLogAt >= VS_IDLE_LOG_MS) {
    const uint32_t span = (now - vsIdleLogAt) / 1000;
    const bool external = vsOnExternalPower();
    Serial.printf("POWER: battery=%dmV %d%% external=%d asleep=%lus of %lus state=%u\n",
                  (int)M5.Power.getBatteryVoltage(), (int)M5.Power.getBatteryLevel(),
                  external ? 1 : 0, (unsigned long)(vsIdleSleptUs / 1000000ULL),
                  (unsigned long)span, (unsigned)state);
    vsIdleSleptUs = 0;
    vsIdleLogAt = now;
  }

  if (!vsIdleEligible()) return;
  if (now - lastUserActivityMs >= VS_IDLE_POWER_OFF_MS) vsIdlePowerOff();
  vsIdleLightSleep();
}
