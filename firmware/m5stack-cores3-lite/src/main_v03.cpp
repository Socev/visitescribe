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

// Allow a later firmware revision to include this implementation and rename
// only v0.3's public setup()/loop() entry points. The nested v0.2 include is
// always renamed independently, avoiding macro collisions in wrapper builds.
#ifndef VISITESCRIBE_V03_SETUP_NAME
#define VISITESCRIBE_V03_SETUP_NAME setup
#define VISITESCRIBE_V03_SETUP_NAME_LOCAL 1
#endif
#ifndef VISITESCRIBE_V03_LOOP_NAME
#define VISITESCRIBE_V03_LOOP_NAME loop
#define VISITESCRIBE_V03_LOOP_NAME_LOCAL 1
#endif

#define setup setup_v02
#define loop loop_v02
#include "main_v02.cpp"
#undef setup
#undef loop

static bool axp2101DirectOk = false;

static bool readTouchV03(int& x, int& y, int& rawX, int& rawY) {
  rawX = rawY = -1;
  if (!touchOk) return false;

  if (!readDirectTouch(rawX, rawY)) return false;

  lgfx::touch_point_t p;
  p.x = rawX;
  p.y = rawY;
  M5.Display.convertRawXY(&p, 1);

  x = p.x;
  y = p.y;
  return x >= 0 && x < M5.Display.width() && y >= 0 && y < M5.Display.height();
}

static void showStorageStatus() {
  refreshBattery();
  state = AppState::STATUS;
  screenDirty = true;
}

static void serviceInputsV03() {
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
      if (!sdOk && (state == AppState::HOME || state == AppState::MODE_CONFIRM)) {
        // A quick-record request cannot succeed without storage. Show the
        // reason instead of silently doing nothing.
        showStorageStatus();
      } else {
        handlePowerButton();
      }
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
      // Navigation is always allowed. Only the actual START action is
      // redirected to STATUS when storage is unavailable.
      if (!sdOk && state == AppState::MODE_CONFIRM && TWO_TOP.contains(tx, ty)) {
        showStorageStatus();
      } else {
        handleTouch(tx, ty);
      }
    }
    Serial.printf("TOUCH raw=%d,%d converted=%d,%d rot=%u size=%dx%d sd=%d wakeOnly=%d app=%u->%u\n",
                  rawX, rawY, tx, ty,
                  (unsigned)M5.Display.getRotation(),
                  M5.Display.width(), M5.Display.height(),
                  sdOk ? 1 : 0, wakeOnly ? 1 : 0,
                  (unsigned)before, (unsigned)state);
  }
  touchWasDown = touchDown;

  M5.update();
}

void VISITESCRIBE_V03_SETUP_NAME() {
  setup_v02();

  axp2101DirectOk = M5.Power.Axp2101.begin();

  // setup_v02 historically replaced the whole UI with a static SD error page
  // when mounting failed. Keep the recorder UI accessible instead; MENU >
  // STATUS will clearly show MICROSD FOUT, and START/PWR will route there too.
  if (!sdOk) {
    state = AppState::HOME;
    screenDirty = true;
    render(true);
  }

  Serial.printf("VisiteScribe CoreS3-Lite v0.3c; board=%d pmic=%d axp2101_direct=%s touch=%s sd=%s display=%dx%d rot=%u\n",
                (int)M5.getBoard(),
                (int)M5.Power.getType(),
                axp2101DirectOk ? "OK" : "FAIL",
                touchOk ? "OK" : "FAIL",
                sdOk ? "OK" : "FAIL",
                M5.Display.width(), M5.Display.height(),
                (unsigned)M5.Display.getRotation());
}

void VISITESCRIBE_V03_LOOP_NAME() {
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

#ifdef VISITESCRIBE_V03_SETUP_NAME_LOCAL
#undef VISITESCRIBE_V03_SETUP_NAME_LOCAL
#undef VISITESCRIBE_V03_SETUP_NAME
#endif
#ifdef VISITESCRIBE_V03_LOOP_NAME_LOCAL
#undef VISITESCRIBE_V03_LOOP_NAME_LOCAL
#undef VISITESCRIBE_V03_LOOP_NAME
#endif
