#pragma once
// Automatic sync (firmware 0.13.0-test1), both boards.
//
// While Brian rests with recordings waiting, he sends them by himself:
//
//   * when: every 5 minutes, and once right after a recording ends (as soon
//     as the screen has gone dark);
//   * only when truly at rest: HOME, screen off, not recording, nothing else
//     using Wi-Fi or the storage, battery >= 15 % (or on a charger), and at
//     least one recording waiting;
//   * first a short Wi-Fi scan (~1.5 s, nothing sent). Only when a known
//     network is in range does Brian connect; otherwise he waits for the next
//     turn. Home visits therefore cost a scan, not a failing connect;
//   * quietly: the screen stays off and the sync does not count as activity
//     (so the 2-hour switch-off and the screen timer are not reset);
//   * a failed connect or sync waits longer: 10, then 20, then 30 minutes;
//   * no firmware update during an automatic sync; updates stay with a sync
//     you start yourself or the charger sync.
//
// Any button press stops it at once. During the scan and the connect that is
// immediate; during the upload at the next chunk boundary (usually well under
// a second; on a stalling network at most the 15 s request timeout of an
// automatic sync). Nothing is lost: the server
// keeps only complete chunks and Brian deletes only what the server confirmed.
// The press then wakes Brian as usual (the wake-up animation), Wi-Fi is off,
// and the next press starts a recording.
//
// Every attempt leaves one AUTOSYNC line in the logbook; together with the
// POWER lines (idle_power.h) that gives the real battery cost.

static constexpr uint32_t VS_AUTO_INTERVAL_MS = 5UL * 60UL * 1000UL;
static constexpr uint32_t VS_AUTO_BACKOFF_MS[3] = {
    10UL * 60UL * 1000UL, 20UL * 60UL * 1000UL, 30UL * 60UL * 1000UL};
static constexpr uint32_t VS_AUTO_SCAN_TIMEOUT_MS = 10000UL;
static constexpr uint32_t VS_AUTO_ATTEMPT_TIMEOUT_MS = 4UL * 60UL * 1000UL;
static constexpr int VS_AUTO_MIN_BATTERY = 15;
static constexpr uint32_t VS_AUTO_HTTP_TIMEOUT_MS = 15000UL;

enum class VsAutoPhase : uint8_t { IDLE, SCANNING, SYNCING };
enum class VsAutoResult : uint8_t { OK, NOTHING, NO_NETWORK, NO_CONNECT, FAILED,
                                    ABORTED, TIMEOUT, TAKEN_OVER };

static VsAutoPhase vsAutoPhase = VsAutoPhase::IDLE;
static uint32_t vsAutoNextAt = 0;          // millis(); 0 = as soon as possible
static uint32_t vsAutoStartedAt = 0;
static uint32_t vsAutoSavedActivity = 0;
static uint8_t vsAutoFailures = 0;
static bool vsAutoAbort = false;
static bool vsAutoPlanned = false;         // vs080BeforeWifi keeps our scan's order
static bool vsAutoWasRecording = false;
static uint32_t vsAutoLastCheck = 0;

#if VS_STICK
// The upload blocks the loop, so a short click would never be seen by
// M5.update(). An edge interrupt on both buttons latches it instead.
static volatile bool vsAutoButtonIrq = false;
static void IRAM_ATTR vsAutoButtonIsr() { vsAutoButtonIrq = true; }
#endif

static void vsAutoArmButtons() {
  vsAutoAbort = false;
#if VS_STICK
  vsAutoButtonIrq = false;
  attachInterrupt(11, vsAutoButtonIsr, FALLING);
  attachInterrupt(12, vsAutoButtonIsr, FALLING);
#else
  if (axp2101DirectOk) M5.Power.Axp2101.getPekPress();   // drop a stale latch
#endif
}

static void vsAutoDisarmButtons() {
#if VS_STICK
  detachInterrupt(11);
  detachInterrupt(12);
#endif
}

// Any press since the attempt began. The CoreS3's AXP2101 latches PWR presses,
// the StickS3 uses the interrupt above. A screen that came on means some other
// layer already took a press (it only ever wakes on input).
static bool vsAutoPressed() {
  if (vsAutoAbort) return true;
#if VS_STICK
  if (vsAutoButtonIrq) vsAutoAbort = true;
#else
  if (axp2101DirectOk && M5.Power.Axp2101.getPekPress() != 0) vsAutoAbort = true;
#endif
  if (displayPower != DisplayPower::OFF) vsAutoAbort = true;
  return vsAutoAbort;
}

// The sync engine's abort hook (main_v066.cpp): only an automatic sync stops.
static bool vsAutoAbortRequested() {
  return vsAutoPhase != VsAutoPhase::IDLE && vsAutoPressed();
}

static bool vsAutoExternalPower() {
  if (M5.Power.getVBUSVoltage() >= 4000) return true;
  return vsChargeDetector.latched;          // CoreS3 on the Bottom3 dock
}

static bool vsAutoRestingNow() {
  if (vs080Mode != Vs080Mode::NONE || vs080RequestPortal || !vsFleetLinked) return false;
  if (state != AppState::HOME || displayPower != DisplayPower::OFF) return false;
  if (sessionOpen || captureRunning || vsServerSyncRunning || vsUsbSyncActive || !sdOk)
    return false;
#ifdef VISITESCRIBE_DIRECT_OPUS
  if (!vsDoWorkerDone) return false;
#endif
  if (pwrClickPendingV03 || vsPowerProbeActive) return false;
  if (WiFi.getMode() != WIFI_OFF) return false;
#if VS_STICK
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed() || vsStickAClick || vsStickBClick)
    return false;
#endif
  return true;
}

static const char* vsAutoResultName(VsAutoResult r) {
  switch (r) {
    case VsAutoResult::OK: return "ok";
    case VsAutoResult::NOTHING: return "nothing";
    case VsAutoResult::NO_NETWORK: return "no_known_network";
    case VsAutoResult::NO_CONNECT: return "no_connect";
    case VsAutoResult::FAILED: return "failed";
    case VsAutoResult::ABORTED: return "stopped_by_button";
    case VsAutoResult::TIMEOUT: return "timeout";
    case VsAutoResult::TAKEN_OVER: return "taken_over";
  }
  return "?";
}

static void vsAutoFinish(VsAutoResult result) {
  const uint32_t now = millis();
  vsAutoDisarmButtons();
  WiFi.scanDelete();
  if (WiFi.getMode() != WIFI_OFF) wifiOff();
  const bool ownedScreenState = state == AppState::SYNC && vs080Mode == Vs080Mode::NONE;
  if (ownedScreenState) {
    syncPhase = SyncPhase::NOT_STARTED;
    vsServerStage = VsServerStage::IDLE;
    vsServerSessionPrefix = "";
    vsServerError = "";
    goHome();
  }
  lastUserActivityMs = vsAutoSavedActivity;   // a sync by itself is not activity
  vsQuietActivity = false;
  vsHttpTimeoutOverrideMs = 0;
  vsAutoPlanned = false;
  vsAutoPhase = VsAutoPhase::IDLE;

  uint32_t wait = VS_AUTO_INTERVAL_MS;
  switch (result) {
    case VsAutoResult::OK:
    case VsAutoResult::NOTHING:
      vsAutoFailures = 0;
      break;
    case VsAutoResult::NO_CONNECT:
    case VsAutoResult::FAILED:
    case VsAutoResult::TIMEOUT:
      if (vsAutoFailures < 3) ++vsAutoFailures;
      wait = VS_AUTO_BACKOFF_MS[vsAutoFailures - 1];
      break;
    default:
      break;
  }
  vsAutoNextAt = now + wait;
  Serial.printf("AUTOSYNC: result=%s took=%lums sent=%lu next_in=%lus battery=%d%% external=%d\n",
                vsAutoResultName(result), (unsigned long)(now - vsAutoStartedAt),
                (unsigned long)vsServerSessionsDone, (unsigned long)(wait / 1000),
                (int)M5.Power.getBatteryLevel(), vsAutoExternalPower() ? 1 : 0);

  if (result == VsAutoResult::ABORTED) {
    // The press that stopped the sync wakes Brian, exactly as on a dark screen.
    if (displayPower == DisplayPower::OFF && wakeOnlyIfOff()) {
      vsPlayWakeAnimation(false);
    } else {
      noteActivity();
    }
    pwrClickPendingV03 = false;
    pwrFirstClickMsV03 = 0;
    pwrWakeGuardUntilV03 = millis() + PWR_DOUBLE_CLICK_MS;
  }
  screenDirty = true;
}

static void vsAutoBegin() {
  vsAutoStartedAt = millis();
  vsAutoSavedActivity = lastUserActivityMs;
  vsQuietActivity = true;
  vsHttpTimeoutOverrideMs = VS_AUTO_HTTP_TIMEOUT_MS;
  vsAutoArmButtons();
  vsServerSessionsDone = 0;
  vsAutoPhase = VsAutoPhase::SCANNING;
  WiFi.mode(WIFI_STA);
  delay(50);
  // Asynchronous active scan (~120 ms per channel): the loop keeps running, so
  // a press during the scan stops it straight away.
  if (WiFi.scanNetworks(true, true, false, 120) == WIFI_SCAN_FAILED) {
    Serial.println("AUTOSYNC: scan could not start");
    vsAutoFinish(VsAutoResult::NO_CONNECT);
  }
}

// Scan done: order the known networks by signal, as vsFleetPlanWifi() does,
// and connect only if at least one of them is in range.
static void vsAutoScanDone(int found) {
  int32_t best[VS_FLEET_MAX_NETWORKS];
  for (uint8_t i = 0; i < vsFleetNetCount; ++i) best[i] = INT32_MIN;
  for (int s = 0; s < found; ++s) {
    const int at = vsFleetFindNetwork(WiFi.SSID(s));
    if (at >= 0 && WiFi.RSSI(s) > best[at]) best[at] = WiFi.RSSI(s);
  }
  WiFi.scanDelete();
  // Hand the radio back off before connecting (see vsFleetPlanWifi: connecting
  // straight after a scan gave AUTH_EXPIRE on the first hardware test).
  WiFi.mode(WIFI_OFF);
  delay(100);

  vsFleetResetOrder();
  uint8_t count = 0;
  for (uint8_t i = 0; i < vsFleetNetCount; ++i) {
    if (best[i] != INT32_MIN) vsFleetOrder[count++] = i;
  }
  if (!count) {
    vsFleetResetOrder();
    vsAutoFinish(VsAutoResult::NO_NETWORK);
    return;
  }
  std::sort(vsFleetOrder, vsFleetOrder + count,
            [&](uint8_t a, uint8_t b) { return best[a] > best[b]; });
  vsFleetOrderCount = count;   // only what is in range: no futile attempts
  Serial.printf("AUTOSYNC: %u known network(s) in range; best %s (%d dBm)\n",
                (unsigned)count, vsFleetNets[vsFleetOrder[0]].ssid.c_str(),
                (int)best[vsFleetOrder[0]]);

  vsAutoPlanned = true;
  vsAutoPhase = VsAutoPhase::SYNCING;
  pwrClickPendingV03 = false;
  pwrFirstClickMsV03 = 0;
  vsServerStage = VsServerStage::IDLE;
  beginSync();
}

// Called every loop pass, before the older layers see the buttons.
static void vsServiceAutoSync() {
  const uint32_t now = millis();

  if (vsAutoPhase == VsAutoPhase::SCANNING) {
    if (vsAutoPressed()) { vsAutoFinish(VsAutoResult::ABORTED); return; }
    const int16_t found = WiFi.scanComplete();
    if (found == WIFI_SCAN_RUNNING) {
      if (now - vsAutoStartedAt > VS_AUTO_SCAN_TIMEOUT_MS) vsAutoFinish(VsAutoResult::NO_CONNECT);
      return;
    }
    if (found < 0) { vsAutoFinish(VsAutoResult::NO_CONNECT); return; }
    vsAutoScanDone(found);
    return;
  }

  if (vsAutoPhase == VsAutoPhase::SYNCING) {
    if (vsServerSyncRunning) return;               // cannot be: the upload blocks
    if (vsAutoPressed()) { vsAutoFinish(VsAutoResult::ABORTED); return; }
    if (vs080Mode != Vs080Mode::NONE || state != AppState::SYNC) {
      vsAutoFinish(VsAutoResult::TAKEN_OVER);
      return;
    }
    if (syncPhase == SyncPhase::FAILED || syncPhase == SyncPhase::NO_CREDENTIALS) {
      vsAutoFinish(VsAutoResult::NO_CONNECT);
      return;
    }
    if (vsServerStage == VsServerStage::DONE) { vsAutoFinish(VsAutoResult::OK); return; }
    if (vsServerStage == VsServerStage::NOTHING) { vsAutoFinish(VsAutoResult::NOTHING); return; }
    if (vsServerStage == VsServerStage::ERROR) {
      vsAutoFinish(vsAutoAbort ? VsAutoResult::ABORTED : VsAutoResult::FAILED);
      return;
    }
    if (now - vsAutoStartedAt > VS_AUTO_ATTEMPT_TIMEOUT_MS) vsAutoFinish(VsAutoResult::TIMEOUT);
    return;
  }

  // IDLE: a recording that just ended makes the next turn due straight away.
  const bool recordingNow = sessionOpen || captureRunning;
  if (vsAutoWasRecording && !recordingNow) vsAutoNextAt = now;
  vsAutoWasRecording = recordingNow;

  if ((int32_t)(now - vsAutoNextAt) < 0) return;
  if (!vsAutoRestingNow()) return;
  if (now - vsAutoLastCheck < 5000UL) return;      // the checks below touch I2C and storage
  vsAutoLastCheck = now;

  if (!vsAutoExternalPower() && M5.Power.getBatteryLevel() < VS_AUTO_MIN_BATTERY) {
    vsAutoNextAt = now + VS_AUTO_INTERVAL_MS;
    Serial.println("AUTOSYNC: battery low; skipped");
    return;
  }
  if (pendingCountUi() == 0) {
    vsAutoNextAt = now + VS_AUTO_INTERVAL_MS;
    return;
  }
  vsAutoBegin();
}
