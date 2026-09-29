// VisiteScribe CoreS3-Lite v0.8.0 -- Brian as a fleet device
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
//   the server (fleet_link.h).
//
// Nothing about recording, the audio format, the upload engine or the USB/PC
// sync changes. The older layers got three small, optional hook points only.

#include <Arduino.h>
#include "firmware_version.h"

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
enum class Vs080Mode : uint8_t { NONE, PORTAL, PAIRING };
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
  centeredText(214, vsFleetNetCount ? "PWR = klaar" : "PWR = later instellen", C_GREY, 1);
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
    if (!vsFleetHasNetworks()) {
      // Restarting would only open the hotspot again; let Brian record.
      Serial.println("FLEET: setup finished without networks; back to recorder");
      vs080LeaveToHome();
      return;
    }
    Serial.println("FLEET: setup finished; restarting");
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
    Serial.println("FLEET: pairing postponed by PWR");
    vs080LeaveToHome();
    return;
  }
  if (millis() - vs080ModeStarted >= VS080_PAIRING_TIMEOUT_MS) {
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

static const char* vs080DetailsInfo() {
  static char line[64];
  snprintf(line, sizeof(line), "%s  v%s", vsFleetDeviceId(), VISITESCRIBE_FW_VERSION);
  return line;
}

// ---------------------------------------------------------------------------
// 5. Entry points.
// ---------------------------------------------------------------------------
void setup() {
  vsFleetBegin(VS080_LEGACY_ID, VS080_LEGACY_TOKEN, VS080_LEGACY_SSIDS, VS080_LEGACY_PASS,
               VS080_LEGACY_WIFI_COUNT);
  vsBeforeWifiHook = vs080BeforeWifi;
  vsPreUploadHook = vs080BeforeUpload;
  vsPostUploadHook = vs080AfterUpload;
  vsDetailsActionHook = vs080DetailsAction;
  vsDetailsActionLabel = "Wifi instellen";
  vsDetailsInfoHook = vs080DetailsInfo;

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

void loop() {
  vs080LoopState = state;
  switch (vs080Mode) {
    case Vs080Mode::PORTAL:
      vs080ServicePortal();
      vs080KeepAwake();
      delay(2);
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
  loop_v067();
}
