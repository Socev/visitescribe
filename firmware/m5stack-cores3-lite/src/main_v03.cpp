// VisiteScribe CoreS3-Lite v0.3
//
// v0.2 already proved that the FT6336U itself is alive: a touch wakes/dedims
// the display. The remaining UI problem was that v0.2 fed the FT6336 raw
// coordinates straight into a landscape (rotation=1) UI. This wrapper keeps
// the complete v0.2 recorder implementation, but replaces the main loop so
// raw touch points are converted through M5GFX before hit-testing.
//
// The power key is also polled directly from the AXP2101 without depending on
// M5Unified board/PMIC classification. This matters on CoreS3-Lite builds that
// use the generic esp32-s3-devkitc-1 PlatformIO target.

#define setup setup_v02
#define loop loop_v02
#include "main_v02.cpp"
#undef setup
#undef loop

static bool axp2101DirectOk = false;

static bool readTouchV03(int& x, int& y, int& rawX, int& rawY) {
  rawX = rawY = -1;
  if (!touchOk) return false;

  // Use the already-working direct FT6336 read from v0.2. It deliberately
  // bypasses synthesized M5Unified touch events.
  if (!readDirectTouch(rawX, rawY)) return false;

  // Critical v0.3 fix: FT6336 reports native panel coordinates. The UI is
  // drawn after M5.Display.setRotation(1), so convert the raw point with the
  // exact same M5GFX panel transform before hit-testing.
  lgfx::touch_point_t p;
  p.x = rawX;
  p.y = rawY;
  M5.Display.convertRawXY(&p, 1);

  x = p.x;
  y = p.y;
  return x >= 0 && x < M5.Display.width() && y >= 0 && y < M5.Display.height();
}

static void serviceInputsV03() {
  // PWR: CoreS3-Lite's left key is wired to AXP2101 PWRON. Poll the PMIC
  // directly. getPekPress() returns 2 for a short click and clears the status.
  uint8_t pek = 0;
  if (axp2101DirectOk) {
    pek = M5.Power.Axp2101.getPekPress();
  }

  int tx = 0, ty = 0, rawX = 0, rawY = 0;
  bool touchDown = readTouchV03(tx, ty, rawX, rawY);

  if ((pek & 0x02) != 0) {
    Serial.printf("PWR short press state=%u\n", (unsigned)pek);
    if (!wakeOnlyIfOff()) {
      noteActivity();
      if (sdOk) handlePowerButton();
    }
  }

  if (touchDown && !touchWasDown) {
    Serial.printf("TOUCH raw=%d,%d converted=%d,%d rot=%u size=%dx%d\n",
                  rawX, rawY, tx, ty,
                  (unsigned)M5.Display.getRotation(),
                  M5.Display.width(), M5.Display.height());
    if (!wakeOnlyIfOff()) {
      noteActivity();
      if (sdOk) handleTouch(tx, ty);
    }
  }
  touchWasDown = touchDown;

  // Keep M5Unified housekeeping alive for battery/audio internals. v0.2 set
  // cfg.pmic_button=false, so M5.update() will not consume the PWR event.
  M5.update();
}

void setup() {
  setup_v02();

  // Do not gate PWR polling on M5.Power.getType(). The Lite is known hardware:
  // initialise the onboard AXP2101 directly on the internal I2C bus.
  axp2101DirectOk = M5.Power.Axp2101.begin();
  Serial.printf("VisiteScribe CoreS3-Lite v0.3; board=%d pmic=%d axp2101_direct=%s touch=%s display=%dx%d rot=%u\n",
                (int)M5.getBoard(),
                (int)M5.Power.getType(),
                axp2101DirectOk ? "OK" : "FAIL",
                touchOk ? "OK" : "FAIL",
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
