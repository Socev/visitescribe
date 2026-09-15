// VisiteScribe CoreS3-Lite v0.3
//
// v0.2 proved the FT6336U itself is alive: a touch wakes/dedims the display.
// This wrapper keeps the complete v0.2 recorder implementation, but uses the
// direct CoreS3-Lite touch and AXP2101 power-key paths for input.
//
// IMPORTANT: UI navigation must never be gated by microSD availability. The
// previous build logged valid TOUCH/PWR events but only dispatched them when
// sdOk was true, making the whole UI appear dead if SD initialisation failed.
// Recording itself still remains safely gated inside startNewSession().

#define setup setup_v02
#define loop loop_v02
#include "main_v02.cpp"
#undef setup
#undef loop

static bool axp2101DirectOk = false;

static bool readTouchV03(int& x, int& y, int& rawX, int& rawY) {
  rawX = rawY = -1;
  if (!touchOk) return false;

  // The direct FT6336 read is already proven on the Lite.
  if (!readDirectTouch(rawX, rawY)) return false;

  // On the current CoreS3-Lite/M5GFX combination these values are already in
  // the 320x240 landscape coordinate system; convertRawXY is harmless and
  // keeps this correct if the panel driver starts returning native coordinates.
  lgfx::touch_point_t p;
  p.x = rawX;
  p.y = rawY;
  M5.Display.convertRawXY(&p, 1);

  x = p.x;
  y = p.y;
  return x >= 0 && x < M5.Display.width() && y >= 0 && y < M5.Display.height();
}

static void serviceInputsV03() {
  // PWR: poll the onboard AXP2101 directly. getPekPress() returns 2 for a
  // short click and clears that status bit.
  uint8_t pek = 0;
  if (axp2101DirectOk) {
    pek = M5.Power.Axp2101.getPekPress();
  }

  int tx = 0, ty = 0, rawX = 0, rawY = 0;
  bool touchDown = readTouchV03(tx, ty, rawX, rawY);

  if ((pek & 0x02) != 0) {
    AppState before = state;
    bool wakeOnly = wakeOnlyIfOff();
    if (!wakeOnly) {
      noteActivity();
      // Never gate UI dispatch on sdOk. startNewSession() itself refuses to
      // record without storage; navigation and diagnostics must still work.
      handlePowerButton();
    }
    Serial.printf("PWR short press state=%u sd=%d wakeOnly=%d app=%u->%u\n",
                  (unsigned)pek, sdOk ? 1 : 0, wakeOnly ? 1 : 0,
                  (unsigned)before, (unsigned)state);
  }

  if (touchDown && !touchWasDown) {
    AppState before = state;
    bool wakeOnly = wakeOnlyIfOff();
    if (!wakeOnly) {
      noteActivity();
      // Same fix as PWR: touch navigation remains active even if the SD card
      // is absent or failed to mount.
      handleTouch(tx, ty);
    }
    Serial.printf("TOUCH raw=%d,%d converted=%d,%d rot=%u size=%dx%d sd=%d wakeOnly=%d app=%u->%u\n",
                  rawX, rawY, tx, ty,
                  (unsigned)M5.Display.getRotation(),
                  M5.Display.width(), M5.Display.height(),
                  sdOk ? 1 : 0, wakeOnly ? 1 : 0,
                  (unsigned)before, (unsigned)state);
  }
  touchWasDown = touchDown;

  // Keep M5Unified housekeeping alive for battery/audio internals. v0.2 set
  // cfg.pmic_button=false, so M5.update() does not consume the PWR event.
  M5.update();
}

void setup() {
  setup_v02();

  // Do not gate PWR polling on M5.Power.getType(). The Lite is known hardware:
  // initialise the onboard AXP2101 directly on the internal I2C bus.
  axp2101DirectOk = M5.Power.Axp2101.begin();
  Serial.printf("VisiteScribe CoreS3-Lite v0.3b; board=%d pmic=%d axp2101_direct=%s touch=%s sd=%s display=%dx%d rot=%u\n",
                (int)M5.getBoard(),
                (int)M5.Power.getType(),
                axp2101DirectOk ? "OK" : "FAIL",
                touchOk ? "OK" : "FAIL",
                sdOk ? "OK" : "FAIL",
                M5.Display.width(), M5.Display.height(),
                (unsigned)M5.Display.getRotation());
}

void loop() {
  serviceInputsV03();
  serviceAudio();
  serviceSync();

  if (millis() - lastBatteryRefreshMs > 10000) {
    int old = batteryPct;
    refreshBattery();
    if (old != batteryPct && state == AppState::STATUS) screenDirty = true;
  }

  if (state == AppState::FINISHED && millis() - finishedAtMs >= FINISHED_AUTO_HOME_MS) {
    goHome();
  }

  render();
  serviceDisplayPower();
  delay(5);
}
