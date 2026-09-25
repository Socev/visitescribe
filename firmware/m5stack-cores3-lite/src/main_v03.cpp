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

// Preserve any outer wrapper's setup/loop aliases while v0.2 is included.
// GCC/xtensa supports push_macro/pop_macro; without this, the old plain
// #undef setup/#undef loop sequence could erase a later firmware layer's
// aliases and produce duplicate Arduino entrypoints.
#pragma push_macro("setup")
#pragma push_macro("loop")
#undef setup
#undef loop
#define setup setup_v02
#define loop loop_v02
#include "main_v02.cpp"
#undef setup
#undef loop
#pragma pop_macro("loop")
#pragma pop_macro("setup")

static bool axp2101DirectOk = false;

// Software double-click recogniser. We delay the single-click action briefly so
// the first click can never start/stop a recording before we know whether a
// second click follows.
static constexpr uint32_t PWR_DOUBLE_CLICK_MS = 360;
static bool pwrClickPendingV03 = false;
static uint32_t pwrFirstClickMsV03 = 0;

// Later sync layers may attach a synthetic upload benchmark here. Keeping this
// as a hook means older recorder layers still compile and simply fall back to
// normal SYNC if no test implementation is installed.
static void (*vsSyntheticTestHook)() = nullptr;
static const char* (*vsSyntheticTestStatusHook)() = nullptr;

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

static void drawMenuV03() {
  drawHeader("MENU");
#ifdef VISITESCRIBE_DEMO_UI
  zone(THREE_TOP, "STATUS", C_BLUE, C_WHITE);
  zone(THREE_MIDDLE, "SYNC", C_TEAL, C_WHITE, "opnames naar server");
  zone(THREE_BOTTOM, "TERUG", C_NAVY, C_WHITE);
#else
  // Four 50 px rows deliberately reuse the home-screen rectangles so no new
  // touch geometry is introduced into the proven base UI.
  zone(HOME_VISIT, "STATUS", C_BLUE, C_WHITE);
  zone(HOME_ROUND, "SYNC", C_TEAL, C_WHITE, "echte opnames uploaden");
#ifdef VISITESCRIBE_OPUS_EXPERIMENT
  const char* opusStatus = vsSyntheticTestStatusHook ? vsSyntheticTestStatusHook() : "tik om te starten";
  zone(HOME_MEETING, "OPUS TEST", C_AMBER, C_NAVY, opusStatus);
#else
  zone(HOME_MEETING, "SYNC TEST", C_AMBER, C_NAVY, "10 dummy chunks");
#endif
  zone(HOME_MENU, "TERUG", C_NAVY, C_WHITE);
#endif
}

static void handleMenuTouchV03(int x, int y) {
#ifdef VISITESCRIBE_DEMO_UI
  if (THREE_TOP.contains(x, y)) {
    refreshBattery();
    state = AppState::STATUS;
    screenDirty = true;
  } else if (THREE_MIDDLE.contains(x, y)) {
    beginSync();
  } else if (THREE_BOTTOM.contains(x, y)) {
    goHome();
  }
#else
  if (HOME_VISIT.contains(x, y)) {
    refreshBattery();
    state = AppState::STATUS;
    screenDirty = true;
  } else if (HOME_ROUND.contains(x, y)) {
    beginSync();
  } else if (HOME_MEETING.contains(x, y)) {
    if (vsSyntheticTestHook) vsSyntheticTestHook();
    else beginSync();
  } else if (HOME_MENU.contains(x, y)) {
    goHome();
  }
#endif
}

static void pocketOpenMenuV03() {
  if (state == AppState::SYNC) return;

  // FINISHED still owns the just-closed logical session until goHome().
  if (state == AppState::FINISHED) goHome();

  state = AppState::MENU;
  screenDirty = true;
}

static void pocketSinglePowerV03() {
  if (state == AppState::SYNC) return;

  if (state == AppState::RECORDING || state == AppState::PAUSED) {
    // A session stopped before the 10 s choice expires is a VISITE by default.
    if (quickModeChoiceActive) selectQuickMode(Mode::VISIT);
    stopSession();
    return;
  }

  if (!sdOk) {
    showStorageStatus();
    return;
  }

  if (state == AppState::FINISHED) goHome();

  if (!startQuickSession()) {
    showStorageStatus();
    return;
  }

  lastUserActivityMs = millis();
  screenDirty = true;
}

static void pocketDoublePowerV03() {
  if (state == AppState::SYNC) return;

  if (state == AppState::RECORDING || state == AppState::PAUSED) {
    // A patient boundary before an explicit type choice unambiguously makes
    // this a VISITE. Lock that choice first so filenames/events stay coherent.
    if (quickModeChoiceActive) selectQuickMode(Mode::VISIT);
    addMarkerOrNext();
    lastUserActivityMs = millis();
    screenDirty = true;
    return;
  }

  pocketOpenMenuV03();
}

static void serviceInputsV03() {
  uint8_t pek = 0;
  if (axp2101DirectOk) {
    pek = M5.Power.Axp2101.getPekPress();
  }

  // A physical PWR short press is authoritative. If the LCD is asleep, this
  // press is consumed exclusively by wake-up, exactly as requested.
  if ((pek & 0x02) != 0) {
    const AppState before = state;

    if (wakeOnlyIfOff()) {
      pwrClickPendingV03 = false;
      pwrFirstClickMsV03 = 0;
      Serial.printf("PWR wake-only app=%u\n", (unsigned)before);
    } else {
      noteActivity();
      const uint32_t now = millis();

      if (pwrClickPendingV03 &&
          now - pwrFirstClickMsV03 <= PWR_DOUBLE_CLICK_MS) {
        pwrClickPendingV03 = false;
        pwrFirstClickMsV03 = 0;
        pocketDoublePowerV03();
        Serial.printf("PWR double app=%u->%u\n",
                      (unsigned)before, (unsigned)state);
      } else {
        pwrClickPendingV03 = true;
        pwrFirstClickMsV03 = now;
        Serial.printf("PWR first-click pending app=%u\n", (unsigned)before);
      }
    }
  }

  // Fire a single-click action only once the double-click window has expired.
  if (pwrClickPendingV03 &&
      millis() - pwrFirstClickMsV03 > PWR_DOUBLE_CLICK_MS) {
    const AppState before = state;
    pwrClickPendingV03 = false;
    pwrFirstClickMsV03 = 0;
    pocketSinglePowerV03();
    Serial.printf("PWR single app=%u->%u\n",
                  (unsigned)before, (unsigned)state);
  }

  serviceQuickModeChoiceTimeout();

  // Touch is deliberately not even polled on HOME, during a locked recording,
  // or while the LCD sleeps. This prevents pocket touches and removes the old
  // ~200 I2C touch reads/second power cost.
  const bool touchAllowed =
      displayPower != DisplayPower::OFF &&
      (quickModeChoiceActive ||
       state == AppState::MENU ||
       state == AppState::STATUS ||
       state == AppState::SYNC);

  int tx = 0, ty = 0, rawX = 0, rawY = 0;
  const bool touchDown =
      touchAllowed && readTouchV03(tx, ty, rawX, rawY);

  if (touchDown && !touchWasDown) {
    const AppState before = state;
    noteActivity();

    if (quickModeChoiceActive) {
      if (TWO_TOP.contains(tx, ty)) {
        selectQuickMode(Mode::VISIT);
      } else if (TWO_BOTTOM.contains(tx, ty)) {
        selectQuickMode(Mode::MEETING);
      }
    } else if (state == AppState::MENU) {
      handleMenuTouchV03(tx, ty);
    } else if (state == AppState::STATUS) {
      if (STATUS_BACK.contains(tx, ty)) {
        state = AppState::MENU;
        screenDirty = true;
      }
    } else if (state == AppState::SYNC) {
      handleTouch(tx, ty);
    }

    Serial.printf(
        "TOUCH pocket raw=%d,%d converted=%d,%d allowed=%d app=%u->%u\n",
        rawX, rawY, tx, ty, touchAllowed ? 1 : 0,
        (unsigned)before, (unsigned)state);
  }

  touchWasDown = touchAllowed ? touchDown : false;

  if (state == AppState::MENU && screenDirty) {
    screenDirty = false;
    drawMenuV03();
  }

  // Keep M5Unified housekeeping alive for PMIC/audio internals. Long-press PEK
  // events are intentionally ignored by the application; there is no privacy
  // action on PWR.
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

  if (state == AppState::MENU && screenDirty) {
    screenDirty = false;
    drawMenuV03();
  } else {
    render();
  }
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
