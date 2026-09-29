#pragma once
// Brian's own identity and Wi-Fi networks, kept in NVS.
//
// Until v0.8 the device ID, the device token and up to three Wi-Fi networks
// were compiled into the firmware (server_secrets.h / wifi_secrets.h). That
// makes one image per recorder, and an image that carries secrets. From v0.8
// they live in NVS -- the flash area that survives both a USB flash of the
// application and an over-the-air update -- so one image fits every Brian.
//
// Migration: a development build that still has the old headers copies them
// into NVS once, on the first boot that finds NVS empty, so an existing Brian
// keeps its registration and networks. A release build (-DVISITESCRIBE_RELEASE_BUILD)
// never contains them.

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <algorithm>
#include <climits>

static constexpr uint8_t VS_FLEET_MAX_NETWORKS = 8;
static const char* const VS_FLEET_NS = "vsfleet";

struct VsFleetNetwork {
  String ssid;
  String pass;
};

static String vsFleetIdValue;
static String vsFleetTokenValue;
static VsFleetNetwork vsFleetNets[VS_FLEET_MAX_NETWORKS];
static uint8_t vsFleetNetCount = 0;
static uint32_t vsFleetWifiApplied = 0;
static bool vsFleetLoaded = false;

// Connection order for the current sync: indexes into vsFleetNets, strongest
// visible network first. Rebuilt by vsFleetPlanWifi() before every sync.
static uint8_t vsFleetOrder[VS_FLEET_MAX_NETWORKS];
static uint8_t vsFleetOrderCount = 0;

static String vsFleetMacHex() {
  uint8_t mac[6] = {0};
  esp_efuse_mac_get_default(mac);
  char out[13];
  snprintf(out, sizeof(out), "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(out);
}

static String vsFleetMacPretty() {
  uint8_t mac[6] = {0};
  esp_efuse_mac_get_default(mac);
  char out[18];
  snprintf(out, sizeof(out), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(out);
}

// Used through the VISITESCRIBE_DEVICE_ID / VISITESCRIBE_DEVICE_TOKEN macros by
// all older layers. The pointers stay valid until the value changes; no caller
// keeps them longer than one request.
const char* vsFleetDeviceId() { return vsFleetIdValue.c_str(); }
const char* vsFleetToken() { return vsFleetTokenValue.c_str(); }
static bool vsFleetHasToken() { return vsFleetTokenValue.length() > 0; }
static bool vsFleetHasNetworks() { return vsFleetNetCount > 0; }

const char* vsFleetWifiSsidAt(uint8_t index) {
  if (index >= vsFleetOrderCount) return "";
  return vsFleetNets[vsFleetOrder[index]].ssid.c_str();
}
const char* vsFleetWifiPassAt(uint8_t index) {
  if (index >= vsFleetOrderCount) return "";
  return vsFleetNets[vsFleetOrder[index]].pass.c_str();
}

static void vsFleetResetOrder() {
  vsFleetOrderCount = vsFleetNetCount;
  for (uint8_t i = 0; i < vsFleetNetCount; ++i) vsFleetOrder[i] = i;
}

static bool vsFleetSaveNetworks() {
  Preferences p;
  if (!p.begin(VS_FLEET_NS, false)) return false;
  bool ok = p.putUChar("wifi_n", vsFleetNetCount) == 1;
  for (uint8_t i = 0; i < VS_FLEET_MAX_NETWORKS; ++i) {
    char ks[8], kp[8];
    snprintf(ks, sizeof(ks), "ssid%u", i);
    snprintf(kp, sizeof(kp), "pass%u", i);
    if (i < vsFleetNetCount) {
      ok &= p.putString(ks, vsFleetNets[i].ssid) == vsFleetNets[i].ssid.length();
      // putString returns 0 for an empty string; an open network is valid.
      p.putString(kp, vsFleetNets[i].pass);
    } else {
      p.remove(ks);
      p.remove(kp);
    }
  }
  p.putBool("wifi_init", true);
  p.end();
  vsFleetResetOrder();
  return ok;
}

static int vsFleetFindNetwork(const String& ssid) {
  for (uint8_t i = 0; i < vsFleetNetCount; ++i) {
    if (vsFleetNets[i].ssid == ssid) return i;
  }
  return -1;
}

// Insert or replace. Returns false only when the list is full.
static bool vsFleetUpsertNetwork(const String& ssid, const String& pass, bool save = true) {
  if (!ssid.length() || ssid.length() > 32 || pass.length() > 63) return false;
  int at = vsFleetFindNetwork(ssid);
  if (at < 0) {
    if (vsFleetNetCount >= VS_FLEET_MAX_NETWORKS) return false;
    at = vsFleetNetCount++;
  }
  vsFleetNets[at].ssid = ssid;
  vsFleetNets[at].pass = pass;
  return save ? vsFleetSaveNetworks() : true;
}

static bool vsFleetRemoveNetwork(const String& ssid, bool save = true) {
  const int at = vsFleetFindNetwork(ssid);
  if (at < 0) return true;
  for (uint8_t i = at; i + 1 < vsFleetNetCount; ++i) vsFleetNets[i] = vsFleetNets[i + 1];
  --vsFleetNetCount;
  vsFleetNets[vsFleetNetCount] = VsFleetNetwork();
  return save ? vsFleetSaveNetworks() : true;
}

static bool vsFleetSaveIdentity(const String& deviceId, const String& token) {
  Preferences p;
  if (!p.begin(VS_FLEET_NS, false)) return false;
  bool ok = p.putString("devid", deviceId) == deviceId.length();
  if (token.length()) ok &= p.putString("token", token) == token.length();
  else p.remove("token");
  p.end();
  if (ok) {
    vsFleetIdValue = deviceId;
    vsFleetTokenValue = token;
  }
  return ok;
}

static bool vsFleetSaveWifiApplied(uint32_t applied) {
  Preferences p;
  if (!p.begin(VS_FLEET_NS, false)) return false;
  const bool ok = p.putUInt("wifi_done", applied) == sizeof(uint32_t);
  p.end();
  if (ok) vsFleetWifiApplied = applied;
  return ok;
}

// A pending over-the-air update survives the reboot that installs it, so the
// first boot of the new image knows to check in and confirm itself.
// The version is stored with it so a boot that finds the old version running
// knows the bootloader rolled back, and can report that.
static void vsFleetSetOtaPending(const String& releaseId, const String& version = String()) {
  Preferences p;
  if (!p.begin(VS_FLEET_NS, false)) return;
  if (releaseId.length()) {
    p.putString("ota_rel", releaseId);
    p.putString("ota_ver", version);
  } else {
    p.remove("ota_rel");
    p.remove("ota_ver");
  }
  p.end();
}

static String vsFleetOtaPending(String* version = nullptr) {
  Preferences p;
  if (!p.begin(VS_FLEET_NS, true)) return String();
  const String v = p.getString("ota_rel", "");
  if (version) *version = p.getString("ota_ver", "");
  p.end();
  return v;
}

static bool vsFleetLegacyToken(const char* token) {
  return token && token[0] && strcmp(token, "PASTE_DEVICE_TOKEN_HERE") != 0;
}

// Load NVS, migrating compiled-in values once when NVS is still empty.
static void vsFleetBegin(const char* legacyId, const char* legacyToken,
                         const char* const* legacySsids, const char* const* legacyPass,
                         uint8_t legacyCount) {
  if (vsFleetLoaded) return;
  vsFleetLoaded = true;
  Preferences p;
  const bool open = p.begin(VS_FLEET_NS, false);
  String id = open ? p.getString("devid", "") : String();
  String token = open ? p.getString("token", "") : String();
  const bool wifiInit = open && p.getBool("wifi_init", false);
  vsFleetWifiApplied = open ? p.getUInt("wifi_done", 0) : 0;

  if (!id.length()) {
    // Only a build made for that one existing Brian may adopt the compiled
    // identity (VISITESCRIBE_MIGRATE_LEGACY_IDENTITY, env cores3-lite-migrate).
    // Otherwise every new Brian flashed from the same checkout would become
    // the same device.
    if (legacyId && legacyId[0] && vsFleetLegacyToken(legacyToken)) {
      // An existing, hand-registered Brian: keep its identity.
      id = legacyId;
      token = legacyToken;
      Serial.printf("FLEET: migrated compiled identity %s into NVS\n", id.c_str());
    } else {
      id = String("brian-") + vsFleetMacHex();
      token = "";
      Serial.printf("FLEET: new identity %s\n", id.c_str());
    }
    if (open) {
      p.putString("devid", id);
      if (token.length()) p.putString("token", token);
    }
  }
  vsFleetIdValue = id;
  vsFleetTokenValue = token;

  vsFleetNetCount = 0;
  if (wifiInit) {
    const uint8_t n = p.getUChar("wifi_n", 0);
    for (uint8_t i = 0; i < n && i < VS_FLEET_MAX_NETWORKS; ++i) {
      char ks[8], kp[8];
      snprintf(ks, sizeof(ks), "ssid%u", i);
      snprintf(kp, sizeof(kp), "pass%u", i);
      const String ssid = p.getString(ks, "");
      if (!ssid.length()) continue;
      vsFleetNets[vsFleetNetCount].ssid = ssid;
      vsFleetNets[vsFleetNetCount].pass = p.getString(kp, "");
      ++vsFleetNetCount;
    }
  }
  if (open) p.end();

  if (!wifiInit) {
    for (uint8_t i = 0; i < legacyCount; ++i) {
      if (legacySsids[i] && legacySsids[i][0]) {
        vsFleetUpsertNetwork(legacySsids[i], legacyPass[i] ? legacyPass[i] : "", false);
      }
    }
    vsFleetSaveNetworks();   // also marks wifi_init, even when empty
    if (vsFleetNetCount) {
      Serial.printf("FLEET: migrated %u compiled Wi-Fi network(s) into NVS\n",
                    (unsigned)vsFleetNetCount);
    }
  }
  vsFleetResetOrder();
  Serial.printf("FLEET: device=%s token=%s networks=%u wifi_ops_applied=%lu\n",
                vsFleetIdValue.c_str(), vsFleetHasToken() ? "yes" : "no",
                (unsigned)vsFleetNetCount, (unsigned long)vsFleetWifiApplied);
}

// Scan once and try the known networks that are actually in range, strongest
// first. When none of them shows up (a hidden SSID, or a scan that failed),
// fall back to trying every known network in stored order.
static void vsFleetPlanWifi() {
  vsFleetResetOrder();
  if (vsFleetNetCount <= 1) return;
  if ((WiFi.getMode() & WIFI_MODE_STA) == 0) {
    WiFi.mode(WIFI_STA);
    delay(100);
  }
  const int found = WiFi.scanNetworks(false, true);
  if (found <= 0) {
    WiFi.scanDelete();
    return;
  }
  int32_t best[VS_FLEET_MAX_NETWORKS];
  for (uint8_t i = 0; i < vsFleetNetCount; ++i) best[i] = INT32_MIN;
  for (int s = 0; s < found; ++s) {
    const String seen = WiFi.SSID(s);
    const int at = vsFleetFindNetwork(seen);
    if (at >= 0 && WiFi.RSSI(s) > best[at]) best[at] = WiFi.RSSI(s);
  }
  WiFi.scanDelete();
  uint8_t count = 0;
  for (uint8_t i = 0; i < vsFleetNetCount; ++i) {
    if (best[i] != INT32_MIN) vsFleetOrder[count++] = i;
  }
  if (!count) return;   // keep the stored order
  std::sort(vsFleetOrder, vsFleetOrder + count,
            [&](uint8_t a, uint8_t b) { return best[a] > best[b]; });
  // Networks the scan did not see (hidden SSIDs, a missed beacon) still get
  // their turn, after the visible ones.
  uint8_t total = count;
  for (uint8_t i = 0; i < vsFleetNetCount; ++i) {
    if (best[i] == INT32_MIN) vsFleetOrder[total++] = i;
  }
  vsFleetOrderCount = total;
  Serial.printf("FLEET: %u of %u known network(s) in range; trying %s first\n",
                (unsigned)count, (unsigned)vsFleetNetCount,
                vsFleetNets[vsFleetOrder[0]].ssid.c_str());
}
