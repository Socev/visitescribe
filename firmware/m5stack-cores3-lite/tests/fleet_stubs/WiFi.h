// Host stub: a scan result the test controls.
#pragma once
#include <vector>
#include "Arduino.h"
enum { WIFI_OFF = 0, WIFI_MODE_STA = 1, WIFI_STA = 1 };
struct FakeWiFi {
  std::vector<std::pair<std::string, int>> seen;
  int getMode() { return WIFI_MODE_STA; }
  void mode(int) {}
  int scanNetworks(bool, bool) { return (int)seen.size(); }
  String SSID(int i) { return String(seen[i].first); }
  int RSSI(int i) { return seen[i].second; }
  int encryptionType(int) { return 3; }
  int channel(int) { return 6; }
  void scanDelete() {}
};
static FakeWiFi WiFi;
