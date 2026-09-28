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
static constexpr uint32_t PWR_DOUBLE_CLICK_MS = 350;
static bool pwrClickPendingV03 = false;
static uint32_t pwrFirstClickMsV03 = 0;
static uint32_t pwrWakeGuardUntilV03 = 0;

// Type choice is committed on release inside the same card. This prevents a
// finger already resting on the display from selecting a mode on screen-open.
static bool quickTouchArmedV03 = false;
static uint8_t quickTouchChoiceV03 = 0; // 1=patient, 2=meeting, 3=stop
static int quickTouchLastXV03 = 0;
static int quickTouchLastYV03 = 0;
static const Rect QUICK_PATIENT_TOUCH {4, 68, 312, 44};
static const Rect QUICK_MEETING_TOUCH {4, 114, 312, 44};
static const Rect QUICK_STOP_TOUCH {236, 160, 76, 24};

// Later sync layers may attach a synthetic upload benchmark here. Keeping this
// as a hook means older recorder layers still compile and simply fall back to
// normal SYNC if no test implementation is installed.
static void (*vsSyntheticTestHook)() = nullptr;
static const char* (*vsSyntheticTestStatusHook)() = nullptr;

// Local-network transport benchmark. A later firmware overlay installs the
// implementation; older builds simply omit the menu action.
static void (*vsLanBenchmarkHook)() = nullptr;
static const char* (*vsLanBenchmarkStatusHook)() = nullptr;

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
  uiErrorTitle = "Opnemen niet mogelijk";
  uiErrorDetail = "Opslag niet beschikbaar";
  state = AppState::ERROR;
  screenDirty = true;
}

static void drawMenuV03() {
  // The pocket build now shares the same three-item Brian menu everywhere.
  drawMenu();
}

static void handleMenuTouchV03(int x, int y) {
  if (THREE_TOP.contains(x, y)) {
    beginSync();
  } else if (THREE_MIDDLE.contains(x, y)) {
    refreshBattery();
    state = AppState::STATUS;
    lastUserActivityMs = millis();
    screenDirty = true;
  } else if (THREE_BOTTOM.contains(x, y)) {
    goHome();
  }
}

static void pocketOpenMenuV03() {
  if (state == AppState::SYNC) return;

  // FINISHED still owns the just-closed logical session until goHome().
  if (state == AppState::FINISHED) goHome();

  state = AppState::MENU;
  screenDirty = true;
}

static void pocketSinglePowerV03() {
  switch (state) {
    case AppState::CHARGE_SYNC:
      cancelChargeSync();
      return;
    case AppState::SYNC: {
      if (vsSyncTouchLockedHook && vsSyncTouchLockedHook()) return;
      const bool resultScreen =
          syncPhase == SyncPhase::FAILED ||
          syncPhase == SyncPhase::NO_CREDENTIALS ||
          (vsSyncDoneHook && vsSyncDoneHook()) ||
          (vsSyncRetryAllowedHook && vsSyncRetryAllowedHook());
      if (!resultScreen) return;
      wifiOff();
      syncPhase = SyncPhase::NOT_STARTED;
      goHome();
      return;
    }

    case AppState::SAVING:
      return;

    case AppState::RECORDING:
    case AppState::PAUSED:
      // During the chooser every physical PWR press is handled immediately in
      // serviceInputsV03() as a selection cycle. This fallback is only for an
      // already-queued single click.
      if (quickModeChoiceActive) {
        cycleQuickChoice();
        return;
      }
      stopSession();
      return;

    case AppState::MENU:
      goHome();
      return;

    case AppState::STATUS:
      state = AppState::MENU;
      lastUserActivityMs = millis();
      screenDirty = true;
      return;

    case AppState::DETAILS:
      state = AppState::STATUS;
      lastUserActivityMs = millis();
      screenDirty = true;
      return;

    case AppState::ERROR:
      if (!sdOk) {
        sdOk = ensureStorage();
        if (!sdOk) {
          showStorageStatus();
          return;
        }
      }
      goHome();
      return;

    case AppState::FINISHED:
      goHome();
      break;

    case AppState::HOME:
      break;

    case AppState::MODE_CONFIRM:
      goHome();
      break;
  }

  if (!sdOk) {
    showStorageStatus();
    return;
  }

  // The gesture is now unambiguously a single click. Show feedback before
  // synchronous SD/encoder/worker initialization; do not claim capture yet.
  drawHeader("");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);
  centeredText(104, "Opname starten...", C_NAVY, 2);
  if (!startQuickSession()) {
    state = AppState::ERROR;
    if (!uiErrorTitle.length()) uiErrorTitle = "Opnemen niet mogelijk";
    if (!uiErrorDetail.length()) uiErrorDetail = "Controleer Apparaatstatus";
    screenDirty = true;
    return;
  }

  // If a finger was already resting on the touchscreen before PWR started the
  // recording, consume that held contact. A mode can only be chosen after it
  // has been released and touched again.
  int tx=0, ty=0, rawX=0, rawY=0;
  touchWasDown = readTouchV03(tx, ty, rawX, rawY);
  quickTouchArmedV03 = false;
  quickTouchChoiceV03 = 0;

  lastUserActivityMs = millis();
  screenDirty = true;
}

static void pocketDoublePowerV03() {
  switch (state) {
    case AppState::RECORDING:
    case AppState::PAUSED:
      // Before type is established, double PWR deliberately has no meaning.
      // It must never secretly turn into "next patient".
      if (quickModeChoiceActive) return;
      addMarkerOrNext();
      lastUserActivityMs = millis();
      screenDirty = true;
      return;

    case AppState::HOME:
    case AppState::FINISHED:
      pocketOpenMenuV03();
      return;

    default:
      // Double-click is intentionally inert in menu/status/sync/error/saving.
      return;
  }
}

static void serviceInputsV03() {
  uint8_t pek = 0;
  if (axp2101DirectOk) {
    pek = M5.Power.Axp2101.getPekPress();
  }

  if ((pek & 0x02) != 0) {
    const AppState before = state;
    const uint32_t now = millis();

    if (state == AppState::CHARGE_SYNC) {
      cancelChargeSync();
      pwrClickPendingV03 = false;
      pwrFirstClickMsV03 = 0;
      pwrWakeGuardUntilV03 = now + PWR_DOUBLE_CLICK_MS;
    } else if (quickModeChoiceActive && state == AppState::RECORDING) {
      // While the ten-second chooser is visible, PWR is not interpreted as a
      // single/double-click gesture. Every physical press advances exactly one
      // visible choice: Patient -> Vergadering -> STOP -> Patient.
      pwrClickPendingV03 = false;
      pwrFirstClickMsV03 = 0;
      pwrWakeGuardUntilV03 = 0;
      noteActivity();
      cycleQuickChoice();
      Serial.printf(
          "PWR chooser-cycle choice=%u\n",
          (unsigned)quickChoice);
    } else if (displayPower == DisplayPower::OFF && wakeOnlyIfOff()) {
      pwrClickPendingV03 = false;
      pwrFirstClickMsV03 = 0;
      pwrWakeGuardUntilV03 = now + PWR_DOUBLE_CLICK_MS;
      Serial.printf("PWR wake-only app=%u\n", (unsigned)before);
    } else if (static_cast<int32_t>(pwrWakeGuardUntilV03 - now) > 0) {
      // Consume every short click belonging to the same wake gesture and move
      // the quiet window forward. Only a fresh gesture after silence can act.
      pwrClickPendingV03 = false;
      pwrFirstClickMsV03 = 0;
      pwrWakeGuardUntilV03 = now + PWR_DOUBLE_CLICK_MS;
      Serial.printf("PWR wake-guard consumed app=%u\n", (unsigned)before);
    } else {
      noteActivity();

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
        if (state == AppState::HOME) {
          M5.Display.fillRect(0, 147, SCREEN_W, 27, C_WHITE);
          centeredText(160, "PWR ontvangen", C_BLUE, 1);
          Serial.printf("PWR feedback latency=%lums\n", (unsigned long)(millis() - now));
        }
        Serial.printf("PWR first-click pending app=%u\n", (unsigned)before);
      }
    }
  }

  if (pwrClickPendingV03 &&
      millis() - pwrFirstClickMsV03 > PWR_DOUBLE_CLICK_MS) {
    const AppState before = state;
    Serial.printf("PWR single wait=%lums\n", (unsigned long)(millis() - pwrFirstClickMsV03));
    pwrClickPendingV03 = false;
    pwrFirstClickMsV03 = 0;
    pocketSinglePowerV03();
    Serial.printf("PWR single app=%u->%u\n",
                  (unsigned)before, (unsigned)state);
  }

  serviceQuickModeChoiceTimeout();

  const bool syncTouchAllowed =
      state == AppState::SYNC &&
      !(vsSyncTouchLockedHook && vsSyncTouchLockedHook());
  const bool touchAllowed =
      displayPower != DisplayPower::OFF &&
      (quickModeChoiceActive ||
       state == AppState::MENU ||
       state == AppState::STATUS ||
       state == AppState::DETAILS ||
       state == AppState::CHARGE_SYNC ||
       syncTouchAllowed ||
       state == AppState::ERROR);

  int tx = 0, ty = 0, rawX = 0, rawY = 0;
  const bool touchDown =
      touchAllowed && readTouchV03(tx, ty, rawX, rawY);

  if (touchDown) {
    quickTouchLastXV03 = tx;
    quickTouchLastYV03 = ty;
  }

  if (touchDown && !touchWasDown) {
    const AppState before = state;
    noteActivity();

    if (quickModeChoiceActive) {
      quickTouchArmedV03 = true;
      quickTouchChoiceV03 =
          QUICK_PATIENT_TOUCH.contains(tx, ty) ? 1 :
          (QUICK_MEETING_TOUCH.contains(tx, ty) ? 2 :
           (QUICK_STOP_TOUCH.contains(tx, ty) ? 3 : 0));
      if (!quickTouchChoiceV03) quickTouchArmedV03 = false;
    } else if (state == AppState::MENU) {
      handleMenuTouchV03(tx, ty);
    } else if (state == AppState::STATUS ||
               state == AppState::DETAILS ||
               state == AppState::SYNC ||
               state == AppState::CHARGE_SYNC ||
               state == AppState::ERROR) {
      handleTouch(tx, ty);
    }

    Serial.printf(
        "TOUCH pocket raw=%d,%d converted=%d,%d allowed=%d app=%u->%u\n",
        rawX, rawY, tx, ty, touchAllowed ? 1 : 0,
        (unsigned)before, (unsigned)state);
  }

  if (!touchDown && touchWasDown && quickTouchArmedV03) {
    const bool patientCommit =
        quickTouchChoiceV03 == 1 &&
        QUICK_PATIENT_TOUCH.contains(
            quickTouchLastXV03, quickTouchLastYV03);
    const bool meetingCommit =
        quickTouchChoiceV03 == 2 &&
        QUICK_MEETING_TOUCH.contains(
            quickTouchLastXV03, quickTouchLastYV03);
    const bool stopCommit =
        quickTouchChoiceV03 == 3 &&
        QUICK_STOP_TOUCH.contains(
            quickTouchLastXV03, quickTouchLastYV03);

    quickTouchArmedV03 = false;
    quickTouchChoiceV03 = 0;

    if (quickModeChoiceActive) {
      if (patientCommit) selectQuickMode(Mode::VISIT);
      else if (meetingCommit) selectQuickMode(Mode::MEETING);
      else if (stopCommit) touchQuickStop();
    }
  }

  if (!quickModeChoiceActive) {
    quickTouchArmedV03 = false;
    quickTouchChoiceV03 = 0;
  }

  touchWasDown = touchAllowed ? touchDown : false;

  if (state == AppState::MENU && screenDirty) {
    screenDirty = false;
    drawMenuV03();
  }

  M5.update();
}

void VISITESCRIBE_V03_SETUP_NAME() {
  setup_v02();

  axp2101DirectOk = M5.Power.Axp2101.begin();

  // This firmware reads FT6336 directly only while touch is actually useful.
  // Disable M5Unified's own Touch.update() path so M5.update() no longer polls
  // the controller on every ~5 ms loop iteration in a pocket/recording state.
  M5.Touch.end();

  // Never present "Klaar voor opname" when local recording cannot be made.
  // Startup storage failure is a blocking, truthful error state.
  if (!sdOk) {
    uiErrorTitle = "Opnemen niet mogelijk";
    uiErrorDetail = "Opslag niet beschikbaar";
    state = AppState::ERROR;
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
