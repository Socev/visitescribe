// VisiteScribe CoreS3-Lite v0.8/0.9 -- Brian as a fleet device
//
// On top of the complete v0.6.4 recorder (itself v0.6.7 sync + direct Opus):
//
// * identity, token and Wi-Fi networks live in NVS instead of compiled-in
//   headers, so one image fits every Brian (fleet_store.h);
// * first boot without any Wi-Fi opens a setup hotspot with a QR code and a
//   phone-friendly setup page; later reachable via Details > Wifi instellen
//   (fleet_portal.h);
// * a new Brian enrols itself and shows a pairing code until an admin links
//   it; every sync sends a heartbeat, fetches config and applies Wi-Fi
//   changes queued on the server (fleet_link.h);
// * firmware updates over the air, only when idle and the battery is > 20 %
//   on a charger or > 80 % without one, SHA-256 checked before the new image
//   is made bootable, with automatic rollback if the new image never reaches
//   the server (fleet_link.h);
// * a logbook: every printed line kept in PSRAM and on SD, sent to the server
//   at every sync (fleet_log.h, vsFleetUploadLogs in fleet_link.h).
//
// Nothing about recording, the audio format, the upload engine or the USB/PC
// sync changes. The older layers got three small, optional hook points only.

#include <Arduino.h>
// Every library header that could mention `Serial` comes in before the
// logbook tee renames it (see fleet_log.h).
#include <M5Unified.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include "firmware_version.h"
#include "fleet_log.h"
#define Serial vsLogSerial

// Keep the marker in the image: the server reads the version from it.
extern "C" const char vs080FirmwareMarker[] __attribute__((used)) = VISITESCRIBE_FW_MARKER;

// ---------------------------------------------------------------------------
// 1. Capture the old compiled-in secrets (development builds only), then
//    replace the macros every older layer uses with NVS-backed values.
// ---------------------------------------------------------------------------
#if __has_include("server_secrets.h")
#include "server_secrets.h"
#endif
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif

// The compiled identity is adopted only by a build made for that one existing
// Brian (env cores3-lite-migrate). Wi-Fi networks from wifi_secrets.h are
// harmless to share and are copied by every development build.
#if !defined(VISITESCRIBE_RELEASE_BUILD) && defined(VISITESCRIBE_MIGRATE_LEGACY_IDENTITY) && \
    defined(VISITESCRIBE_DEVICE_ID) && defined(VISITESCRIBE_DEVICE_TOKEN)
static const char* const VS080_LEGACY_ID = VISITESCRIBE_DEVICE_ID;
static const char* const VS080_LEGACY_TOKEN = VISITESCRIBE_DEVICE_TOKEN;
#else
static const char* const VS080_LEGACY_ID = nullptr;
static const char* const VS080_LEGACY_TOKEN = nullptr;
#endif

#if !defined(VISITESCRIBE_RELEASE_BUILD) && defined(VISITESCRIBE_WIFI_SSID_1)
#ifndef VISITESCRIBE_WIFI_SSID_2
#define VISITESCRIBE_WIFI_SSID_2 ""
#define VISITESCRIBE_WIFI_PASSWORD_2 ""
#endif
#ifndef VISITESCRIBE_WIFI_SSID_3
#define VISITESCRIBE_WIFI_SSID_3 ""
#define VISITESCRIBE_WIFI_PASSWORD_3 ""
#endif
static const char* const VS080_LEGACY_SSIDS[3] = {
    VISITESCRIBE_WIFI_SSID_1, VISITESCRIBE_WIFI_SSID_2, VISITESCRIBE_WIFI_SSID_3};
static const char* const VS080_LEGACY_PASS[3] = {
    VISITESCRIBE_WIFI_PASSWORD_1, VISITESCRIBE_WIFI_PASSWORD_2, VISITESCRIBE_WIFI_PASSWORD_3};
static constexpr uint8_t VS080_LEGACY_WIFI_COUNT = 3;
#else
static const char* const VS080_LEGACY_SSIDS[1] = {nullptr};
static const char* const VS080_LEGACY_PASS[1] = {nullptr};
static constexpr uint8_t VS080_LEGACY_WIFI_COUNT = 0;
#endif

#undef VISITESCRIBE_DEVICE_ID
#undef VISITESCRIBE_DEVICE_TOKEN
#undef VISITESCRIBE_WIFI_SSID_1
#undef VISITESCRIBE_WIFI_PASSWORD_1
#undef VISITESCRIBE_WIFI_SSID_2
#undef VISITESCRIBE_WIFI_PASSWORD_2
#undef VISITESCRIBE_WIFI_SSID_3
#undef VISITESCRIBE_WIFI_PASSWORD_3

#include "fleet_store.h"

#define VISITESCRIBE_DEVICE_ID vsFleetDeviceId()
#define VISITESCRIBE_DEVICE_TOKEN vsFleetToken()
#define VISITESCRIBE_WIFI_CONFIGURED 1
#define VISITESCRIBE_WIFI_PROFILE_COUNT VS_FLEET_MAX_NETWORKS
#define VISITESCRIBE_WIFI_SSID_AT(i) vsFleetWifiSsidAt(i)
#define VISITESCRIBE_WIFI_PASS_AT(i) vsFleetWifiPassAt(i)
#define VISITESCRIBE_WIFI_SSID_1 vsFleetWifiSsidAt(0)
#define VISITESCRIBE_WIFI_PASSWORD_1 vsFleetWifiPassAt(0)
#define VISITESCRIBE_WIFI_SSID_2 vsFleetWifiSsidAt(1)
#define VISITESCRIBE_WIFI_PASSWORD_2 vsFleetWifiPassAt(1)
#define VISITESCRIBE_WIFI_SSID_3 vsFleetWifiSsidAt(2)
#define VISITESCRIBE_WIFI_PASSWORD_3 vsFleetWifiPassAt(2)

// ---------------------------------------------------------------------------
// 2. The complete recorder, with its entry points renamed.
// ---------------------------------------------------------------------------
#define setup setup_v067
#define loop loop_v067
#include "main_v064.cpp"
#undef setup
#undef loop

#include "fleet_portal.h"
#include "fleet_link.h"

// Keep a freshly installed image "pending verify" until it has reached the
// server (vsFleetConfirmImage). Arduino would otherwise confirm it at boot,
// before we know it works, and the bootloader's rollback would never trigger.
// C linkage: the weak default in esp32-hal-misc.c is a C symbol.
extern "C" bool verifyRollbackLater() { return true; }

// ---------------------------------------------------------------------------
// 3. Fleet modes that take over the screen and the loop.
// ---------------------------------------------------------------------------
enum class Vs080Mode : uint8_t { NONE, PORTAL, PAIRING, RESET_CONFIRM };
static Vs080Mode vs080Mode = Vs080Mode::NONE;
static uint32_t vs080ModeStarted = 0;
static uint32_t vs080LastPoll = 0;
static uint32_t vs080LastKeepAwake = 0;
static bool vs080RequestPortal = false;
static AppState vs080LoopState = AppState::HOME;   // state at the start of this loop pass
static bool vs080NeedsDraw = false;
static constexpr uint32_t VS080_PAIRING_POLL_MS = 15000;
static constexpr uint32_t VS080_PAIRING_TIMEOUT_MS = 15UL * 60UL * 1000UL;
static constexpr uint32_t VS080_PORTAL_TIMEOUT_MS = 30UL * 60UL * 1000UL;

// First-time setup: from the first boot until the server reports this Brian
// as linked, the hotspot and the pairing screen cannot be left with PWR and do
// not time out. Afterwards (hotspot opened from Details) they can.
static bool vs080Onboarding() { return !vsFleetLinked; }

static bool vs080PwrPressed() {
  return axp2101DirectOk && (M5.Power.Axp2101.getPekPress() & 0x02);
}

static void vs080KeepAwake() {
  if (millis() - vs080LastKeepAwake >= 2000) {
    vs080LastKeepAwake = millis();
    noteActivity();
  }
  serviceDisplayPower();
}

static void vs080DrawPortal() {
  drawHeader("Wifi instellen");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);
  const String qr = vsPortalQrText();
  M5.Display.qrcode(qr.c_str(), 8, HEADER_H + 8, 140, 3);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_NAVY);
  const int x = 160;
  M5.Display.drawString("1. Scan de QR-code of", x, 56);
  M5.Display.drawString("   kies dit netwerk:", x, 70);
  M5.Display.setTextColor(C_BLUE);
  M5.Display.drawString(vsPortalSsid.c_str(), x + 8, 88);
  M5.Display.setTextColor(C_NAVY);
  M5.Display.drawString("   wachtwoord:", x, 106);
  M5.Display.setTextColor(C_BLUE);
  M5.Display.drawString(vsPortalPass.c_str(), x + 8, 122);
  M5.Display.setTextColor(C_NAVY);
  M5.Display.drawString("2. Open 192.168.4.1", x, 144);
  M5.Display.setTextColor(C_GREY);
  char line[48];
  snprintf(line, sizeof(line), "Netwerken: %u   Telefoons: %d",
           (unsigned)vsFleetNetCount, WiFi.softAPgetStationNum());
  M5.Display.drawString(line, x, 166);
  M5.Display.setTextDatum(middle_center);
  if (!vs080Onboarding() && vsFleetNetCount) centeredText(214, "PWR = klaar", C_GREY, 1);
  else centeredText(214, "Kies daarna Opslaan en herstarten", C_GREY, 1);
}

static void vs080EnterPortal() {
  Serial.println("FLEET: entering setup hotspot");
  if (!vsPortalStart()) {
    uiErrorTitle = "Hotspot mislukt";
    uiErrorDetail = "Probeer het opnieuw";
    state = AppState::ERROR;
    screenDirty = true;
    return;
  }
  vs080Mode = Vs080Mode::PORTAL;
  vs080ModeStarted = millis();
  noteActivity();
  vs080DrawPortal();
}

static void vs080LeaveToHome() {
  wifiOff();
  syncPhase = SyncPhase::NOT_STARTED;
  vsServerStage = VsServerStage::IDLE;
  vsServerSessionPrefix = "";
  vs080Mode = Vs080Mode::NONE;
  goHome();
  pwrClickPendingV03 = false;
  pwrFirstClickMsV03 = 0;
  pwrWakeGuardUntilV03 = millis() + PWR_DOUBLE_CLICK_MS;
  screenDirty = true;
}

static void vs080ServicePortal() {
  if (vsPortalService()) {
    vsPortalStop();
    Serial.println("FLEET: setup finished; restarting");
    if (sdOk) vsLogFlushToSd();
    Serial.flush();
    delay(200);
    ESP.restart();
  }
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck >= 1000) {
    lastCheck = millis();
    const int clients = WiFi.softAPgetStationNum();
    if (clients != vsPortalLastClients || vsFleetNetCount != vsPortalLastNets) {
      vsPortalLastClients = clients;
      vsPortalLastNets = vsFleetNetCount;
      vs080DrawPortal();
    }
  }
  const bool locked = vs080Onboarding() || !vsFleetHasNetworks();
  if (locked) {
    if (vs080PwrPressed()) {
      Serial.println("FLEET: PWR ignored; first-time setup must be finished");
      vs080DrawPortal();
      centeredText(196, "Eerst wifi instellen en opslaan", C_AMBER, 1);
    }
    return;
  }
  const bool timeout = millis() - vs080ModeStarted >= VS080_PORTAL_TIMEOUT_MS;
  if (vs080PwrPressed() || timeout) {
    Serial.printf("FLEET: leaving setup hotspot (%s)\n", timeout ? "timeout" : "PWR");
    const bool changed = vsPortalChanged;
    vsPortalStop();
    vs080LeaveToHome();
    // New networks and no token yet: go and enrol right away.
    if (vsFleetHasNetworks() && (changed || !vsFleetHasToken())) beginSync();
  }
}

static void vs080EnterPairing() {
  vs080Mode = Vs080Mode::PAIRING;
  vs080ModeStarted = millis();
  vs080LastPoll = millis();
  noteActivity();
  // Drawn on the first loop: the sync engine still paints its own result
  // screen after the hook that switched us into pairing returns.
  vs080NeedsDraw = true;
}

static void vs080ServicePairing() {
  if (vs080NeedsDraw) {
    vs080NeedsDraw = false;
    vsFleetDrawPairing();
  }
  if (vs080PwrPressed()) {
    if (vs080Onboarding()) {
      Serial.println("FLEET: PWR ignored; waiting to be linked");
    } else {
      Serial.println("FLEET: pairing postponed by PWR");
      vs080LeaveToHome();
      return;
    }
  }
  if (!vs080Onboarding() && millis() - vs080ModeStarted >= VS080_PAIRING_TIMEOUT_MS) {
    Serial.println("FLEET: pairing wait timed out; will retry on next sync");
    vs080LeaveToHome();
    return;
  }
  if (millis() - vs080LastPoll < VS080_PAIRING_POLL_MS) return;
  vs080LastPoll = millis();
  if (WiFi.status() != WL_CONNECTED) {
    vsFleetDrawPairing("Wifi weg - opnieuw proberen...");
    WiFi.reconnect();
    return;
  }
  const String before = vsFleetPairingCode;
  bool wifiChanged = false;
  if (!vsFleetFetchConfig(wifiChanged)) {
    vsFleetDrawPairing("Server niet bereikbaar - opnieuw...");
    return;
  }
  if (wifiChanged) vsFleetHeartbeat();
  if (!vsFleetPending) {
    Serial.println("FLEET: linked by admin");
    drawHeader("Koppelen");
    M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);
    centeredText(104, "Gekoppeld", C_GREEN, 3);
    centeredText(146, "Opnames worden nu verzonden", C_GREY, 1);
    delay(2500);
    // Back into the running sync: stage IDLE with Wi-Fi still connected makes
    // the v0.6.7 engine start the upload queue on the next loop.
    vs080Mode = Vs080Mode::NONE;
    vsServerStage = VsServerStage::IDLE;
    vsServerSessionPrefix = "";
    noteActivity();
    return;
  }
  if (vsFleetPairingCode != before) vsFleetDrawPairing();
}

// ---------------------------------------------------------------------------
// 4. Hooks into the older layers.
// ---------------------------------------------------------------------------
static bool vs080BeforeWifi() {
  // The dock gives no hardware charging signal; its voltage-rise detection is
  // trusted only for a sync that the charge auto-sync itself started.
  vsFleetDockSync = vs080LoopState == AppState::CHARGE_SYNC;
  if (!vsFleetHasNetworks()) return false;
  vsFleetPlanWifi();
  return true;
}

static bool vs080BeforeUpload() {
  vsFleetUpdate = VsFleetUpdate();
  if (!vsFleetHasToken()) {
    vsSetStage(VsServerStage::FETCH_KEY, "Aanmelden bij server");
    if (!vsFleetEnroll()) {
      vsFail(vsFleetLastError);
      return false;
    }
  }
  if (!vsFleetHeartbeat() && vsFleetLastHttp == 401 &&
      vsFleetErrorCode(vsFleetLastBody) == "INVALID_DEVICE") {
    // The server itself says it does not know this token (a pending device was
    // refused, or the device was deleted). Try to enrol again, without the old
    // token on the request. The old token is kept -- in NVS and in memory --
    // unless enrolment succeeds, so a proxy hiccup cannot cost the device its
    // credentials.
    const String oldToken = vsFleetTokenValue;
    vsFleetTokenValue = "";
    if (!vsFleetEnroll()) {
      vsFleetTokenValue = oldToken;
      vsFail(vsFleetLastError);
      return false;
    }
    vsFleetHeartbeat();
  }
  vsFleetReportRollback();
  bool wifiChanged = false;
  if (vsFleetFetchConfig(wifiChanged) && wifiChanged) vsFleetHeartbeat();
  vsFleetUploadLogs();
  if (vsFleetPending) {
    vsSetStage(VsServerStage::NOTHING, "Wacht op koppeling");
    vs080EnterPairing();
    return false;
  }
  // Config unreachable on an older server: carry on uploading as before.
  return true;
}

static void vs080AfterUpload() {
  if (vs080Mode != Vs080Mode::NONE) return;
  if (WiFi.status() != WL_CONNECTED) return;       // the sync lost Wi-Fi
  if (vsFleetImagePendingVerify()) {
    // Never overwrite the only known-good slot while this image is unproven.
    Serial.println("FLEET: running image not confirmed yet; no update now");
    return;
  }
  vsFleetMaybeUpdate();   // restarts on success
}

static void vs080DetailsAction() {
  if (captureRunning || sessionOpen) return;
  vs080RequestPortal = true;
}

// ---------------------------------------------------------------------------
// Factory reset, from Details. Confirmed by tapping 1, 2, 3 in order on three
// buttons shown in a shuffled order, so a pocket touch cannot do it.
// ---------------------------------------------------------------------------
static uint8_t vs080ResetOrder[3] = {1, 2, 3};   // label shown on left/middle/right
static uint8_t vs080ResetNext = 1;
static bool vs080ResetTouchWasDown = false;
static uint32_t vs080ResetStarted = 0;

static Rect vs080ResetButton(uint8_t slot) {
  return Rect{16 + slot * 98, 146, 92, 56};
}

static void vs080DrawReset(const char* note = nullptr, uint16_t noteColor = C_GREY) {
  drawHeader("Fabrieksreset");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);
  centeredText(62, "Wist wifi en de koppeling met de server.", C_NAVY, 1);
  centeredText(80, "Opnames op de SD-kaart blijven bewaard.", C_GREY, 1);
  centeredText(98, "Daarna opnieuw instellen en koppelen.", C_GREY, 1);
  centeredText(126, note ? note : "Tik 1, 2 en 3 in de goede volgorde", noteColor, 1);
  for (uint8_t slot = 0; slot < 3; ++slot) {
    const Rect r = vs080ResetButton(slot);
    const bool done = vs080ResetOrder[slot] < vs080ResetNext;
    M5.Display.fillRoundRect(r.x, r.y, r.w, r.h, 10, done ? C_NAVY : C_WHITE);
    M5.Display.drawRoundRect(r.x, r.y, r.w, r.h, 10, done ? C_NAVY : C_LINE);
    char label[4];
    snprintf(label, sizeof(label), "%u", (unsigned)vs080ResetOrder[slot]);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(done ? C_WHITE : C_NAVY);
    M5.Display.drawString(label, r.x + r.w / 2, r.y + r.h / 2);
  }
  centeredText(222, "PWR = annuleren", C_GREY, 1);
}

static void vs080EnterReset() {
  // A shuffled order, never the plain 1-2-3 left to right.
  do {
    for (uint8_t i = 0; i < 3; ++i) vs080ResetOrder[i] = i + 1;
    for (int i = 2; i > 0; --i) {
      const int j = esp_random() % (i + 1);
      std::swap(vs080ResetOrder[i], vs080ResetOrder[j]);
    }
  } while (vs080ResetOrder[0] == 1 && vs080ResetOrder[1] == 2);
  vs080ResetNext = 1;
  vs080ResetTouchWasDown = true;   // the tap that opened this screen is still down
  vs080ResetStarted = millis();
  vs080Mode = Vs080Mode::RESET_CONFIRM;
  noteActivity();
  vs080DrawReset();
}

static void vs080CancelReset(const char* why) {
  Serial.printf("FLEET: factory reset cancelled (%s)\n", why);
  vs080DrawReset(why, C_AMBER);
  delay(1500);
  vs080Mode = Vs080Mode::NONE;
  state = AppState::DETAILS;
  lastUserActivityMs = millis();
  screenDirty = true;
  pwrClickPendingV03 = false;
  pwrFirstClickMsV03 = 0;
  pwrWakeGuardUntilV03 = millis() + PWR_DOUBLE_CLICK_MS;
}

static void vs080ServiceReset() {
  if (vs080PwrPressed()) { vs080CancelReset("Geannuleerd"); return; }
  if (millis() - vs080ResetStarted > 30000UL) { vs080CancelReset("Geannuleerd (tijd verstreken)"); return; }
  int x = 0, y = 0, rx = 0, ry = 0;
  const bool down = readTouchV03(x, y, rx, ry);
  const bool pressed = down && !vs080ResetTouchWasDown;
  vs080ResetTouchWasDown = down;
  if (!pressed) return;
  noteActivity();
  for (uint8_t slot = 0; slot < 3; ++slot) {
    if (!vs080ResetButton(slot).contains(x, y)) continue;
    if (vs080ResetOrder[slot] != vs080ResetNext) {
      vs080CancelReset("Verkeerde volgorde - geannuleerd");
      return;
    }
    ++vs080ResetNext;
    vs080DrawReset();
    if (vs080ResetNext > 3) {
      vs080DrawReset("Terugzetten... Brian herstart", C_NAVY);
      vsFleetFactoryReset();
      if (sdOk) vsLogFlushToSd();
      Serial.flush();
      delay(1200);
      ESP.restart();
    }
    return;
  }
}

static void vs080DetailsReset() {
  if (captureRunning || sessionOpen) return;
  vs080EnterReset();
}

static const char* vs080StatusAccount(uint16_t& color) {
  if (!vsFleetLinked) {
    color = C_AMBER;
    return "niet gekoppeld";
  }
  if (!vsFleetOwnerEmail.length()) {
    color = C_GREY;
    return "geen account";
  }
  // Green: signed in to OurMind, recordings can be delivered. Amber: the
  // account is known but its OurMind login has lapsed.
  color = vsFleetOwnerOurMind ? C_GREEN : C_AMBER;
  return vsFleetOwnerEmail.c_str();
}

// HOME, top-left: the OurMind account instead of "Brian", once known. Long
// addresses are shortened so they never run into the battery figure.
static const char* vs080HomeName(uint16_t& color) {
  if (!vsFleetLinked || !vsFleetOwnerEmail.length()) return nullptr;
  static char shown[36];
  const size_t max = 32;
  if (vsFleetOwnerEmail.length() <= max) {
    snprintf(shown, sizeof(shown), "%s", vsFleetOwnerEmail.c_str());
  } else {
    snprintf(shown, sizeof(shown), "%.*s..", (int)(max - 2), vsFleetOwnerEmail.c_str());
  }
  color = vsFleetOwnerOurMind ? C_NAVY : C_AMBER;
  return shown;
}

static const char* vs080DetailsInfo() {
  static char line[64];
  snprintf(line, sizeof(line), "%s  v%s", vsFleetDeviceId(), VISITESCRIBE_FW_VERSION);
  return line;
}

// ---------------------------------------------------------------------------
// 5. Entry points.
// ---------------------------------------------------------------------------
void setup() {
  vsLogBegin();   // first: from here on every printed line is kept
  vsLogMuteHook = []() { return vsUsbSyncActive; };
  vsFleetBegin(VS080_LEGACY_ID, VS080_LEGACY_TOKEN, VS080_LEGACY_SSIDS, VS080_LEGACY_PASS,
               VS080_LEGACY_WIFI_COUNT);
  vsBeforeWifiHook = vs080BeforeWifi;
  vsPreUploadHook = vs080BeforeUpload;
  vsPostUploadHook = vs080AfterUpload;
  vsDetailsActionHook = vs080DetailsAction;
  vsDetailsActionLabel = "Wifi";
  vsDetailsAction2Hook = vs080DetailsReset;
  vsDetailsAction2Label = "Reset";
  vsDetailsInfoHook = vs080DetailsInfo;
  vsStatusAccountHook = vs080StatusAccount;
  vsHomeNameHook = vs080HomeName;

  setup_v067();

  const bool imagePending = vsFleetImagePendingVerify();
  vsFleetCheckRollback();
  Serial.printf("VisiteScribe CoreS3-Lite v%s (%s); device=%s image=%s\n",
                VISITESCRIBE_FW_VERSION, vs080FirmwareMarker, vsFleetDeviceId(),
                imagePending ? "pending-verify" : "valid");

  if (!vsFleetHasNetworks()) {
    vs080EnterPortal();
  } else if (!vsFleetHasToken() || imagePending || vsFleetOtaPending().length()) {
    // Enrol, or let a freshly updated image check in (and confirm itself).
    beginSync();
  }
}

// Write the logbook to SD every minute, but only while nothing else can be
// using the card: not recording, not syncing, not in USB maintenance.
static void vs080ServiceLogFlush() {
  static uint32_t last = 0;
  if (millis() - last < 60000UL) return;
  const bool busy = captureRunning || sessionOpen || vsServerSyncRunning ||
                    vsUsbSyncActive || !sdOk
#ifdef VISITESCRIBE_DIRECT_OPUS
                    || !vsDoWorkerDone
#endif
      ;
  if (busy) return;
  last = millis();
  vsLogFlushToSd();
}

void loop() {
  vs080LoopState = state;
  vs080ServiceLogFlush();
  switch (vs080Mode) {
    case Vs080Mode::PORTAL:
      vs080ServicePortal();
      vs080KeepAwake();
      delay(2);
      return;
    case Vs080Mode::RESET_CONFIRM:
      vs080ServiceReset();
      vs080KeepAwake();
      delay(10);
      return;
    case Vs080Mode::PAIRING:
      vs080ServicePairing();
      vs080KeepAwake();
      delay(10);
      return;
    case Vs080Mode::NONE:
      break;
  }
  if (vs080RequestPortal) {
    vs080RequestPortal = false;
    vs080EnterPortal();
    return;
  }
  // First-time setup not finished (no Wi-Fi reached, server unreachable): try
  // again by itself every 30 s while Brian sits idle at HOME.
  static uint32_t lastOnboardingTry = 0;
  if (vs080Onboarding() && state == AppState::HOME && vsFleetHasNetworks() &&
      !captureRunning && !sessionOpen && !vsServerSyncRunning &&
      millis() - lastOnboardingTry >= 30000UL) {
    lastOnboardingTry = millis();
    Serial.println("FLEET: first-time setup not finished; connecting again");
    beginSync();
  }
  loop_v067();
}
