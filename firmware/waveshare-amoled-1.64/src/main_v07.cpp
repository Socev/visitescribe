// VisiteScribe MINI v0.7 display-power overlay.
//
// Keep the proven v0.6 application/UI in main.cpp intact, but replace only the
// Arduino setup()/loop() entrypoints here. This makes the power-policy change
// small and reversible while the recorder/audio architecture is still evolving.
// PlatformIO excludes main.cpp as a standalone translation unit and compiles
// this file instead; main.cpp is included once below.

#define setup visitescribe_setup_v06
#define loop visitescribe_loop_v06
#include "main.cpp"
#undef setup
#undef loop

// Stage 2 of the AMOLED idle policy:
//   0..12 s : normal brightness
//   12..60 s: dimmed (implemented by v0.6 serviceDisplayPower())
//   >60 s   : panel emission completely off
//
// A tap while merely DIMMED wakes and acts in the same tap.
// A tap while fully OFF only wakes the screen. The user must then tap the now
// visible control. This deliberately prevents an invisible STOP/PRIVACY/
// VOLGENDE action.
static constexpr uint32_t DISPLAY_IDLE_OFF_MS = 60000;
static bool displayFullyOff = false;

static bool syncNeedsVisibleFeedback() {
  return state == AppState::SYNC &&
         (syncPhase == SyncPhase::CONNECTING_1 ||
          syncPhase == SyncPhase::CONNECTING_2);
}

static void wakeDisplayOnly() {
  if (!displayFullyOff) return;

  panel->displayOn();
  panel->setBrightness(DISPLAY_BRIGHTNESS_ACTIVE);
  displayFullyOff = false;
  displayDimmed = false;
  lastUserActivityMs = millis();

  // Repaint from application state rather than relying on panel RAM surviving
  // display-off. This also guarantees current timer/patient/battery information.
  screenDirty = true;
  lastDisplayedSecond = 0xFFFFFFFFUL;
  lastFinishedCountdown = 0xFFFFFFFFUL;
  render(true);
}

static void serviceDisplayPowerV07() {
  // Keep the screen awake while Wi-Fi is actively associating. That operation
  // is explicitly user-requested and short, so visible feedback wins here.
  if (syncNeedsVisibleFeedback()) {
    lastUserActivityMs = millis();
    if (displayFullyOff) wakeDisplayOnly();
    if (displayDimmed) {
      panel->setBrightness(DISPLAY_BRIGHTNESS_ACTIVE);
      displayDimmed = false;
    }
    return;
  }

  // Existing first stage: dim after 12 seconds.
  serviceDisplayPower();

  if (displayFullyOff) return;
  if (millis() - lastUserActivityMs < DISPLAY_IDLE_OFF_MS) return;

  // CO5300 displayOff() stops AMOLED emission. Touch, recording, SD and the
  // ESP32 continue to run; only the display panel goes dark.
  panel->displayOff();
  displayFullyOff = true;
}

void setup() {
  visitescribe_setup_v06();
  Serial.println("VisiteScribe UI power policy v0.7 active.");
  Serial.println("Display: dim at 12 s, fully off at 60 s; first tap from OFF only wakes.");
}

void loop() {
  uint16_t x = 0, y = 0;
  bool touchDown = readTouch(x, y);

  if (touchDown && !touchWasDown) {
    if (displayFullyOff) {
      // Safety: the first tap on an invisible UI never triggers an action.
      wakeDisplayOnly();
    } else {
      // While merely dimmed, preserve the snappy one-tap wake + action policy.
      noteUserActivity();
      handleTouchPress(x, y);
    }
  } else if (touchDown && !displayFullyOff) {
    // Holding/touching keeps the panel awake without repeating the button.
    noteUserActivity();
  }
  touchWasDown = touchDown;

  // The physical BOOT button remains an emergency control. It also wakes the
  // display, but a long press still retains its existing STOP/home behaviour.
  bool bootDownNow = digitalRead(VISITESCRIBE_BOOT_GPIO) == LOW;
  if (bootDownNow && displayFullyOff) wakeDisplayOnly();
  pollBootButton();

  serviceSync();

  bool batteryChanged = updateBattery(false);
  if (batteryChanged && batteryPercent != lastBatteryUiPercent) {
    if (!displayFullyOff) {
      if (state == AppState::STATUS) screenDirty = true;
      else drawBatteryBadge();
    }
  }

  if (state == AppState::FINISHED &&
      millis() - finishedAtMs >= FINISHED_AUTO_HOME_MS) {
    goHome();
  }

  // Do not send pointless QSPI redraws while the AMOLED is off. Recording and
  // all other background state continue independently.
  if (!displayFullyOff) render();
  serviceDisplayPowerV07();
  delay(8);
}
