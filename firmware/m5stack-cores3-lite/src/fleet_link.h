#pragma once
// Brian <-> server housekeeping around every sync: enrol, heartbeat, config,
// Wi-Fi changes, pairing, and firmware over the air.
//
// Runs inside the existing sync, on the connection the sync already opened:
//
//   Wi-Fi up -> vsFleetBeforeUpload()  enrol if needed, heartbeat, config,
//                                      apply Wi-Fi changes, pairing check
//            -> upload recordings      (unchanged v0.6.7 engine)
//            -> vsFleetAfterUpload()   install a firmware update if one is set
//                                      out and the battery/charger rules allow
//
// Included from main_v080.cpp after the whole recorder, so every helper of the
// older layers (vsBeginHttp, vsSetStage, drawHeader, ...) is in scope.

#include <ArduinoJson.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include "firmware_version.h"
#include "fleet_store.h"

struct VsFleetUpdate {
  bool present = false;
  String releaseId;
  String version;
  String sha256;
  String url;
  uint32_t size = 0;
  int minCharging = 20;
  int minUnplugged = 80;
};

static VsFleetUpdate vsFleetUpdate;
static bool vsFleetPending = false;          // enrolled, not yet linked by an admin
static String vsFleetPairingCode;
static String vsFleetLastError;
static int vsFleetLastHttp = 0;
static bool vsFleetConfigSeen = false;
static String vsFleetLastBody;
static bool vsFleetDockSync = false;         // this sync was started by the charge auto-sync
static String vsFleetRolledBackRelease;     // to report at the next successful contact
static uint32_t vsFleetLogRequest = 0;      // admin asked for the whole logbook

// Absolute floor, whatever the server says: never flash below 20 % battery.
static constexpr int VS_OTA_BATTERY_FLOOR = 20;

// --------------------------------------------------------------------------
// HTTP
// --------------------------------------------------------------------------

static int vsFleetRequest(const char* method, const String& path, const String& body,
                          String& response, const String& proofToken = String()) {
  HTTPClient http;
  const String url = String(VISITESCRIBE_SERVER_BASE_URL) + path;
  if (!vsBeginHttp(http, url)) {
    vsFleetLastHttp = -1;
    return -1;
  }
  if (proofToken.length()) http.addHeader("Authorization", String("Bearer ") + proofToken);
  int code;
  if (strcmp(method, "GET") == 0) {
    code = http.GET();
  } else {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body);
  }
  response = code > 0 ? http.getString() : String();
  http.end();
  vsFleetLastHttp = code;
  vsFleetLastBody = response;
  Serial.printf("FLEET: %s %s -> %d\n", method, path.c_str(), code);
  return code;
}

static String vsFleetErrorCode(const String& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return String();
  return String((const char*)(doc["error"]["code"] | ""));
}

// --------------------------------------------------------------------------
// state reported to the server
// --------------------------------------------------------------------------

static bool vsFleetOnCharger() {
  // USB-C is measured. The Bottom3 dock charges through its own TP4057 and
  // gives no hardware signal, so the dock counts as "on the charger" only via
  // the voltage-rise detector the charge auto-sync already uses.
  // The latch is only trusted for a sync the charge auto-sync started: lifted
  // off the dock, it stays latched until the detector samples again at HOME.
  const int vbus = M5.Power.getVBUSVoltage();
  return vbus >= 4000 || (vsFleetDockSync && vsChargeDetector.latched);
}

static void vsFleetHardware(JsonObject hw) {
  hw["mac"] = vsFleetMacPretty();
  hw["board"] = VISITESCRIBE_FW_BOARD;
  hw["chip_rev"] = ESP.getChipRevision();
  hw["flash_mb"] = ESP.getFlashChipSize() / (1024 * 1024);
#ifdef VISITESCRIBE_DIRECT_OPUS
  hw["audio"] = "opus16k";
#else
  hw["audio"] = "wav48k";
#endif
}

// A freshly installed image stays "pending verify" until it has proved it can
// reach the server. If it reboots before that, the bootloader goes back to the
// previous image by itself (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
static void vsFleetConfirmImage() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
      st == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.printf("FLEET: new image %s confirmed; rollback cancelled\n",
                  VISITESCRIBE_FW_VERSION);
  }
  String target;
  if (vsFleetOtaPending(&target).length() && target == VISITESCRIBE_FW_VERSION) {
    vsFleetSetOtaPending(String());
  }
}

// At boot: an update was written, but the version now running is not the one
// it installed. The bootloader went back to the previous image, i.e. the new
// one never reached the server. Remember to tell the server, or it would keep
// offering the same image in a loop.
static void vsFleetCheckRollback() {
  String target;
  const String release = vsFleetOtaPending(&target);
  if (!release.length() || target == VISITESCRIBE_FW_VERSION) return;
  vsFleetRolledBackRelease = release;
  vsFleetSetOtaPending(String());
  Serial.printf("FLEET: update %s (%s) was rolled back; still running %s\n",
                release.c_str(), target.c_str(), VISITESCRIBE_FW_VERSION);
}

static bool vsFleetImagePendingVerify() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  return running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
         st == ESP_OTA_IMG_PENDING_VERIFY;
}

static bool vsFleetHeartbeat() {
  refreshBattery();
  JsonDocument doc;
  doc["device_id"] = vsFleetDeviceId();
  doc["software_version"] = VISITESCRIBE_FW_VERSION;
  if (batteryPct >= 0) doc["battery_percent"] = batteryPct;
  doc["charging"] = vsFleetOnCharger();
  if (WiFi.status() == WL_CONNECTED) {
    doc["network_state"] = WiFi.SSID() + " (" + String(WiFi.RSSI()) + " dBm)";
  }
  JsonArray nets = doc["wifi_networks"].to<JsonArray>();
  for (uint8_t i = 0; i < vsFleetNetCount; ++i) nets.add(vsFleetNets[i].ssid);
  doc["wifi_ops_applied"] = vsFleetWifiApplied;
  vsFleetHardware(doc["hardware"].to<JsonObject>());
  String body, resp;
  serializeJson(doc, body);
  const int code = vsFleetRequest("POST", "/v1/device/heartbeat", body, resp);
  if (code >= 200 && code < 300) {
    vsFleetConfirmImage();
    return true;
  }
  return false;
}

// --------------------------------------------------------------------------
// enrolment
// --------------------------------------------------------------------------

static bool vsFleetEnroll() {
  JsonDocument doc;
  doc["device_id"] = vsFleetDeviceId();
  doc["software_version"] = VISITESCRIBE_FW_VERSION;
  vsFleetHardware(doc["hardware"].to<JsonObject>());
  String body, resp;
  serializeJson(doc, body);
  // No token yet: vsBeginHttp sends X-Device-ID only.
  // After a factory reset: the previous token proves this is the same box.
  const int code = vsFleetRequest("POST", "/v1/device/enroll", body, resp,
                                  vsFleetTokenValue.length() ? String() : vsFleetPrevToken);
  if (code == 201 || code == 200) {
    JsonDocument r;
    if (deserializeJson(r, resp)) {
      vsFleetLastError = "Aanmeldantwoord onleesbaar";
      return false;
    }
    const String token = r["token"] | "";
    if (!token.length() || !vsFleetSaveIdentity(vsFleetDeviceId(), token)) {
      vsFleetLastError = "Token opslaan mislukt";
      return false;
    }
    vsFleetClearPrevToken();
    vsFleetPending = String((const char*)(r["enrolment"]["state"] | "active")) == "pending";
    vsFleetPairingCode = r["enrolment"]["pairing_code"] | "";
    Serial.printf("FLEET: enrolled as %s (%s)\n", vsFleetDeviceId(),
                  vsFleetPending ? "pending" : "active");
    return true;
  }
  const String err = vsFleetErrorCode(resp);
  if (err == "DEVICE_EXISTS") {
    vsFleetLastError = "Al geregistreerd - beheerder: opnieuw laten aanmelden";
  } else if (err == "ENROLMENT_CLOSED") {
    vsFleetLastError = "Aanmelden staat uit op de server";
  } else if (code == 429) {
    vsFleetLastError = "Te veel aanmeldingen - later opnieuw";
  } else {
    vsFleetLastError = String("Aanmelden mislukt (") + code + ")";
  }
  return false;
}

// --------------------------------------------------------------------------
// config: enrolment state, Wi-Fi changes, firmware update
// --------------------------------------------------------------------------

// Returns true when Wi-Fi changes were applied (so the caller confirms them
// with an immediate heartbeat and the server can wipe the passwords).
//
// Removals go first: the server keeps at most one waiting change per SSID, so
// order between different SSIDs does not matter, and a full list can then be
// made room for in the same pass. An add that still does not fit is left
// unconfirmed (and everything after it), so its password is not wiped and the
// website keeps showing it as waiting.
static bool vsFleetApplyWifiOps(JsonArrayConst ops) {
  uint32_t highest = vsFleetWifiApplied;
  uint32_t blockedAt = 0;          // id of the first add that did not fit
  bool changed = false;
  for (int pass = 0; pass < 2; ++pass) {
    for (JsonObjectConst op : ops) {
      const uint32_t id = op["id"] | 0;
      if (!id || id <= vsFleetWifiApplied) continue;
      const String kind = op["op"] | "";
      const String ssid = op["ssid"] | "";
      if (pass == 0 && kind == "remove") {
        vsFleetRemoveNetwork(ssid, false);
        changed = true;
        Serial.printf("FLEET: wifi op %lu remove %s\n", (unsigned long)id, ssid.c_str());
      } else if (pass == 1 && kind == "add") {
        if (blockedAt && id > blockedAt) continue;
        if (!vsFleetUpsertNetwork(ssid, op["password"] | "", false)) {
          Serial.printf("FLEET: wifi op %lu add %s does not fit; left waiting\n",
                        (unsigned long)id, ssid.c_str());
          if (!blockedAt || id < blockedAt) blockedAt = id;
          continue;
        }
        changed = true;
        Serial.printf("FLEET: wifi op %lu add %s\n", (unsigned long)id, ssid.c_str());
      } else {
        continue;
      }
    }
  }
  for (JsonObjectConst op : ops) {
    const uint32_t id = op["id"] | 0;
    if (id > vsFleetWifiApplied && (!blockedAt || id < blockedAt) && id > highest) highest = id;
  }
  if (changed) vsFleetSaveNetworks();
  if (highest != vsFleetWifiApplied) {
    vsFleetSaveWifiApplied(highest);
    changed = true;
  }
  return changed;
}

static void vsFleetReportFor(const String& releaseId, const char* state, const String& detail);

static bool vsFleetFetchConfig(bool& wifiChanged) {
  wifiChanged = false;
  String resp;
  const int code = vsFleetRequest("GET", "/v1/device/config", String(), resp);
  if (code < 200 || code >= 300) return false;
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return false;
  vsFleetConfigSeen = true;

  // An older server has no enrolment block: treat as active.
  JsonObjectConst enrolment = doc["enrolment"];
  vsFleetPending = !enrolment.isNull() &&
                   String((const char*)(enrolment["state"] | "active")) == "pending";
  vsFleetPairingCode = enrolment["pairing_code"] | "";
  if (!vsFleetPending) vsFleetSetLinked(true);
  // Only a server that sends the block (1.9.2+) may change what is shown.
  if (!enrolment.isNull() && enrolment["owner"].is<JsonObjectConst>()) {
    vsFleetSetOwner(String((const char*)(enrolment["owner"]["email"] | "")),
                    enrolment["owner"]["ourmind"] | false);
  } else if (!enrolment.isNull() && enrolment["linked"].is<bool>() &&
             !enrolment["linked"].as<bool>()) {
    vsFleetSetOwner(String(), false);             // bound to nobody
  }

  JsonArrayConst ops = doc["wifi_ops"];
  if (!ops.isNull()) wifiChanged = vsFleetApplyWifiOps(ops);

  vsFleetLogRequest = doc["log_request"]["id"] | 0;

  vsFleetUpdate = VsFleetUpdate();
  JsonObjectConst fw = doc["firmware_update"];
  if (!fw.isNull()) {
    vsFleetUpdate.present = true;
    vsFleetUpdate.releaseId = fw["release_id"] | "";
    vsFleetUpdate.version = fw["version"] | "";
    vsFleetUpdate.sha256 = fw["sha256"] | "";
    vsFleetUpdate.sha256.toLowerCase();
    vsFleetUpdate.url = fw["url"] | "";
    vsFleetUpdate.size = fw["size"] | 0;
    vsFleetUpdate.minCharging = max(VS_OTA_BATTERY_FLOOR, (int)(fw["min_battery_charging"] | 20));
    vsFleetUpdate.minUnplugged = max(VS_OTA_BATTERY_FLOOR, (int)(fw["min_battery_unplugged"] | 80));
    if (!vsFleetUpdate.releaseId.length() || !vsFleetUpdate.url.startsWith("/v1/") ||
        vsFleetUpdate.sha256.length() != 64 || !vsFleetUpdate.size) {
      Serial.println("FLEET: firmware_update block incomplete; ignored");
      vsFleetUpdate.present = false;
    }
    // An image built for another Brian (CoreS3 vs StickS3) would not even fit
    // this flash layout. The server checks this too; never rely on one side.
    const String board = fw["board"] | "";
    if (vsFleetUpdate.present && board.length() && board != VISITESCRIBE_FW_BOARD) {
      static String refused;     // report once, not at every config fetch
      if (refused != vsFleetUpdate.releaseId) {
        refused = vsFleetUpdate.releaseId;
        Serial.printf("FLEET: update %s is for board %s, this is %s; refused\n",
                      vsFleetUpdate.version.c_str(), board.c_str(), VISITESCRIBE_FW_BOARD);
        vsFleetReportFor(vsFleetUpdate.releaseId, "failed",
                         String("image voor ") + board + ", dit is " + VISITESCRIBE_FW_BOARD);
      }
      vsFleetUpdate.present = false;
    }
  }
  return true;
}

static void vsFleetReportFor(const String& releaseId, const char* state, const String& detail) {
  if (!releaseId.length()) return;
  JsonDocument doc;
  doc["release_id"] = releaseId;
  doc["state"] = state;
  doc["detail"] = detail;
  String body, resp;
  serializeJson(doc, body);
  vsFleetRequest("POST", "/v1/device/firmware/report", body, resp);
}

static void vsFleetReport(const char* state, const String& detail) {
  vsFleetReportFor(vsFleetUpdate.releaseId, state, detail);
}

static void vsFleetReportRollback() {
  if (!vsFleetRolledBackRelease.length()) return;
  vsFleetReportFor(vsFleetRolledBackRelease, "failed",
                   String("teruggezet: nieuwe versie bereikte de server niet; draait ") +
                   VISITESCRIBE_FW_VERSION);
  vsFleetRolledBackRelease = "";
}

// --------------------------------------------------------------------------
// logbook upload
// --------------------------------------------------------------------------

// Sends the logbook lines added since the last successful upload (or, when
// the admin asked, everything still kept), in chunks of up to 64 KiB. The
// high-water mark (boot, line) is stored in NVS only after the server said
// 200, so a lost reply just means a resend, which the server ignores.
static void vsFleetUploadLogs() {
  Preferences p;
  uint32_t sentBoot = 0, sentLine = 0;
  if (p.begin("vslog", true)) {
    sentBoot = p.getUInt("sent_b", 0);
    sentLine = p.getUInt("sent_l", 0);
    p.end();
  }
  const uint32_t request = vsFleetLogRequest;
  if (sdOk) vsLogFlushToSd();   // this runs inside the sync: the card is ours
  static constexpr size_t CHUNK = 64 * 1024;
  char* buf = static_cast<char*>(heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buf) return;
  uint32_t b = request ? 0 : sentBoot, l = request ? 0 : sentLine;
  const int rounds = request ? 40 : 8;   // at most ~512 KiB per sync, 2.5 MiB on request
  size_t total = 0;
  for (int round = 0; round < rounds; ++round) {
    size_t n = 0;
    if (sdOk) {
      n = vsLogReadSd(b, l, buf, CHUNK);
    } else if (b < vsLogBoot || (b == vsLogBoot)) {
      uint32_t last = 0;
      n = vsLogRingCopy(b == vsLogBoot ? l : 0, buf, CHUNK, last);
      if (n) {
        b = vsLogBoot;
        l = last;
      }
    }
    const bool finished = n < CHUNK - 512;
    if (!n && !request) break;
    HTTPClient http;
    if (!vsBeginHttp(http, String(VISITESCRIBE_SERVER_BASE_URL) + "/v1/device/logs")) break;
    http.addHeader("Content-Type", "text/plain; charset=utf-8");
    if (request && finished) http.addHeader("X-Log-Request", String(request));
    const int code = http.POST(reinterpret_cast<uint8_t*>(buf), n);
    http.end();
    if (code != 200) {
      Serial.printf("FLEET: log upload failed (%d) after %u bytes\n", code, (unsigned)total);
      break;
    }
    total += n;
    if (b > sentBoot || (b == sentBoot && l > sentLine)) {
      sentBoot = b;
      sentLine = l;
      if (p.begin("vslog", false)) {
        p.putUInt("sent_b", sentBoot);
        p.putUInt("sent_l", sentLine);
        p.end();
      }
    }
    if (finished) {
      if (request) vsFleetLogRequest = 0;
      break;
    }
  }
  free(buf);
  if (total || request) {
    Serial.printf("FLEET: logbook sent %u bytes%s\n", (unsigned)total,
                  request ? " (full, on request)" : "");
  }
}

// --------------------------------------------------------------------------
// pairing screen (the loop in main_v080 runs it while pending)
// --------------------------------------------------------------------------

static void vsFleetDrawPairing(const char* note = nullptr) {
#if VS_STICK
  drawHeader("");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);
  centeredText(HEADER_H + 11, "Koppelen", C_NAVY, 2);
  stickWrapped(46, "Koppelcode voor de beheerder", C_GREY, 1, 2);
  {
    String shown = vsFleetPairingCode;
    if (shown.length() == 6) shown = shown.substring(0, 3) + " " + shown.substring(3);
    centeredText(98, shown.length() ? shown.c_str() : "------", C_BLUE, 3);
  }
  stickWrapped(122, vsFleetDeviceId(), C_GREY, 1, 2);
  stickWrapped(148, note ? note : "Opnames blijven bewaard tot koppeling", C_GREY, 1, 3);
  if (vsFleetLinked) drawPwrHints("Knop: later koppelen");
  return;
#endif
  drawHeader("Koppelen");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);
  centeredText(66, "Koppelcode voor de beheerder", C_GREY, 1);
  String shown = vsFleetPairingCode;
  if (shown.length() == 6) shown = shown.substring(0, 3) + " " + shown.substring(3);
  centeredText(110, shown.length() ? shown.c_str() : "------", C_BLUE, 4);
  centeredText(152, vsFleetDeviceId(), C_GREY, 1);
  centeredText(174, note ? note : "Opnames blijven bewaard tot koppeling", C_GREY, 1);
  if (vsFleetLinked) centeredText(214, "PWR = later koppelen", C_GREY, 1);
}

// --------------------------------------------------------------------------
// firmware over the air
// --------------------------------------------------------------------------

static void vsFleetDrawUpdate(const char* line, uint32_t done, uint32_t total, uint16_t color = C_NAVY) {
#if VS_STICK
  drawHeader("");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);
  centeredText(HEADER_H + 11, "Software-update", C_NAVY, 1);
  {
    const String v = String(VISITESCRIBE_FW_VERSION) + " -> " + vsFleetUpdate.version;
    centeredText(48, v.c_str(), C_GREY, 1);
  }
  int y = stickWrapped(62, line, color, 2, 2);
  if (total) {
    const int x = 8, w = SCREEN_W - 16, h = 12;
    y += 6;
    M5.Display.drawRoundRect(x, y, w, h, 5, C_LINE);
    const int fill = (int)((uint64_t)(w - 4) * done / total);
    M5.Display.fillRoundRect(x + 2, y + 2, fill, h - 4, 3, C_BLUE);
    char pct[16];
    snprintf(pct, sizeof(pct), "%u%%", (unsigned)((uint64_t)done * 100 / total));
    centeredText(y + 24, pct, C_GREY, 1);
  }
  stickWrapped(172, "Niet uitzetten - opnames blijven bewaard", C_GREY, 1, 3);
  return;
#endif
  drawHeader("Software-update");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);
  const String v = String("Versie ") + VISITESCRIBE_FW_VERSION + " -> " + vsFleetUpdate.version;
  centeredText(70, v.c_str(), C_GREY, 1);
  centeredText(104, line, color, 2);
  if (total) {
    const int x = 30, y = 140, w = SCREEN_W - 60, h = 16;
    M5.Display.drawRoundRect(x, y, w, h, 6, C_LINE);
    const int fill = (int)((uint64_t)(w - 4) * done / total);
    M5.Display.fillRoundRect(x + 2, y + 2, fill, h - 4, 4, C_BLUE);
    char pct[16];
    snprintf(pct, sizeof(pct), "%u%%", (unsigned)((uint64_t)done * 100 / total));
    centeredText(172, pct, C_GREY, 1);
  }
  centeredText(214, "Niet uitzetten - opnames blijven bewaard", C_GREY, 1);
}

static String vsFleetHex(const uint8_t* d, size_t n) {
  static const char* hx = "0123456789abcdef";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    s += hx[d[i] >> 4];
    s += hx[d[i] & 15];
  }
  return s;
}

// Receives the download body from HTTPClient::writeToStream(), which handles
// both Content-Length and chunked transfer (a proxy in front of the server may
// re-chunk the response). Every byte is hashed and written to the OTA slot.
class VsOtaSink : public Stream {
 public:
  uint32_t total = 0;
  uint32_t expected = 0;
  uint32_t lastDraw = 0;
  bool failed = false;
  String error;
  mbedtls_sha256_context sha;

  VsOtaSink() { mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha, 0); }
  ~VsOtaSink() { mbedtls_sha256_free(&sha); }

  size_t write(const uint8_t* data, size_t len) override {
    if (failed) return 0;
    if (total + len > expected) {
      failed = true;
      error = "Meer data dan aangekondigd";
      return 0;
    }
    mbedtls_sha256_update(&sha, data, len);
    if (Update.write(const_cast<uint8_t*>(data), len) != len) {
      failed = true;
      error = String("Schrijven: ") + Update.errorString();
      return 0;
    }
    total += len;
    if (total - lastDraw >= 64 * 1024 || total == expected) {
      lastDraw = total;
      noteActivity();
      vsFleetDrawUpdate("Downloaden...", total, expected);
    }
    return len;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// Download into the inactive OTA slot, hashing as it streams. The slot is only
// made bootable after size and SHA-256 both match what the server announced.
static bool vsFleetInstall(String& error) {
  const uint32_t expected = vsFleetUpdate.size;
  vsFleetDrawUpdate("Downloaden...", 0, expected);
  HTTPClient http;
  if (!vsBeginHttp(http, String(VISITESCRIBE_SERVER_BASE_URL) + vsFleetUpdate.url)) {
    error = "HTTP start mislukt";
    return false;
  }
  http.setTimeout(20000);
  const int code = http.GET();
  if (code != 200) {
    http.end();
    error = String("Download HTTP ") + code;
    return false;
  }
  const int declared = http.getSize();   // -1 when chunked
  if (declared >= 0 && (uint32_t)declared != expected) {
    http.end();
    error = String("Onverwachte grootte ") + declared;
    return false;
  }
  if (!Update.begin(expected, U_FLASH)) {
    http.end();
    error = String("Update.begin: ") + Update.errorString();
    return false;
  }
  VsOtaSink sink;
  sink.expected = expected;
  int written = 0;
  if (declared >= 0) {
    // Read it ourselves: HTTPClient::writeToStream has no inactivity timeout,
    // and a stalled connection would keep Brian on this screen indefinitely.
    WiFiClient* stream = http.getStreamPtr();
    static uint8_t buf[1460];
    uint32_t lastData = millis();
    while (sink.total < expected && !sink.failed) {
      const int avail = stream->available();
      if (avail > 0) {
        size_t want = (size_t)avail < sizeof(buf) ? (size_t)avail : sizeof(buf);
        if (want > expected - sink.total) want = expected - sink.total;
        const int got = stream->read(buf, want);
        if (got > 0) {
          sink.write(buf, (size_t)got);
          lastData = millis();
        }
      } else if (!stream->connected() || millis() - lastData > 20000) {
        break;
      } else {
        delay(2);
      }
    }
    written = (int)sink.total;
  } else {
    written = http.writeToStream(&sink);   // chunked: HTTPClient decodes it
  }
  http.end();
  uint8_t digest[32];
  mbedtls_sha256_finish(&sink.sha, digest);
  if (sink.failed || written < 0 || sink.total != expected) {
    Update.abort();
    error = sink.failed ? sink.error
                        : (written < 0 ? String("Download afgebroken (") + written + ")"
                                       : String("Onvolledig: ") + sink.total + " van " + expected);
    return false;
  }
  if (vsFleetHex(digest, 32) != vsFleetUpdate.sha256) {
    Update.abort();
    error = "SHA-256 klopt niet";
    return false;
  }
  vsFleetDrawUpdate("Controleren...", expected, expected);
  if (!Update.end()) {   // verifies the image and sets it as the next boot
    error = String("Afronden: ") + Update.errorString();
    return false;
  }
  return true;
}

static void vsFleetMaybeUpdate() {
  if (!vsFleetUpdate.present) return;
  if (vsFleetUpdate.version == VISITESCRIBE_FW_VERSION) return;
  if (captureRunning || sessionOpen) {
    vsFleetReport("deferred", "opname bezig");
    return;
  }
  refreshBattery();
  const int battery = M5.Power.getBatteryLevel();
  const bool charger = vsFleetOnCharger();
  // The rule David set: above the charging threshold while on a charger, or
  // above the unplugged threshold without one (the dock's charge detection is
  // unreliable on a nearly full battery). Never at or below 20 %.
  const bool allowed = battery > VS_OTA_BATTERY_FLOOR &&
      ((charger && battery > vsFleetUpdate.minCharging) ||
       battery > vsFleetUpdate.minUnplugged);
  const String why = String("accu ") + battery + "%, lader " + (charger ? "ja" : "nee");
  if (!allowed) {
    Serial.printf("FLEET: update %s deferred: %s\n", vsFleetUpdate.version.c_str(), why.c_str());
    vsFleetReport("deferred", why);
    return;
  }
  Serial.printf("FLEET: installing %s (%s)\n", vsFleetUpdate.version.c_str(), why.c_str());
  noteActivity();
  String error;
  if (!vsFleetInstall(error)) {
    Serial.printf("FLEET: update failed: %s\n", error.c_str());
    vsFleetReport("failed", error);
    vsFleetDrawUpdate("Update mislukt", 0, 0, C_RED);
#if VS_STICK
    M5.Display.fillRect(0, 104, SCREEN_W, 100, C_WHITE);
    {
      const int y = stickWrapped(108, error.c_str(), C_GREY, 1, 3);
      stickWrapped(y + 4, "Brian werkt verder op de oude versie", C_GREY, 1, 3);
    }
#else
    centeredText(140, error.c_str(), C_GREY, 1);
    centeredText(160, "Brian werkt gewoon verder op de oude versie", C_GREY, 1);
#endif
    const uint32_t until = millis() + 5000;
    while ((int32_t)(millis() - until) < 0) delay(20);
    return;
  }
  vsFleetSetOtaPending(vsFleetUpdate.releaseId, vsFleetUpdate.version);
  vsFleetReport("installing", why);
  vsFleetDrawUpdate("Herstarten...", 1, 1);
  Serial.printf("FLEET: update %s written; restarting\n", vsFleetUpdate.version.c_str());
  if (sdOk) vsLogFlushToSd();
  Serial.flush();
  delay(800);
  ESP.restart();
}
