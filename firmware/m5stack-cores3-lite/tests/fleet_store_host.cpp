// Host test for src/fleet_store.h: identity migration, the Wi-Fi list and the
// connection order. Build: g++ -std=c++17 -I tests/fleet_stubs tests/fleet_store_host.cpp
#include <cassert>
#include <cstdio>
#include "../src/fleet_store.h"

static void reset() {
  fakeNvs() = FakeNvs();
  vsFleetLoaded = false;
  vsFleetIdValue = vsFleetTokenValue = "";
  vsFleetNetCount = 0;
  vsFleetWifiApplied = 0;
  for (auto& n : vsFleetNets) n = VsFleetNetwork();
  WiFi.seen.clear();
}

int main() {
  const char* ssids[3] = {"Praktijk", "Thuis", ""};
  const char* pass[3] = {"praktijk-pw", "thuis-pw", ""};

  // 1. An existing hand-registered Brian keeps its identity and networks.
  reset();
  vsFleetBegin("visitescribe-cores3-001", "tok123", ssids, pass, 3);
  assert(String(vsFleetDeviceId()) == "visitescribe-cores3-001");
  assert(String(vsFleetToken()) == "tok123");
  assert(vsFleetNetCount == 2);
  // ...and on the next boot NVS wins, even if the compiled values differ.
  vsFleetLoaded = false;
  const char* other[3] = {"Anders", "", ""};
  vsFleetBegin("iets-anders", "tok999", other, pass, 3);
  assert(String(vsFleetDeviceId()) == "visitescribe-cores3-001");
  assert(String(vsFleetToken()) == "tok123");
  assert(vsFleetNetCount == 2 && vsFleetNets[0].ssid == "Praktijk");

  // 2. A new Brian (release build, nothing compiled in) derives its ID from the MAC.
  reset();
  vsFleetBegin(nullptr, nullptr, nullptr, nullptr, 0);
  assert(String(vsFleetDeviceId()) == "brian-a1b2c3d4e5f6");
  assert(!vsFleetHasToken() && !vsFleetHasNetworks());
  // The placeholder token from the example header is not an identity.
  reset();
  vsFleetBegin("visitescribe-cores3-001", "PASTE_DEVICE_TOKEN_HERE", nullptr, nullptr, 0);
  assert(String(vsFleetDeviceId()) == "brian-a1b2c3d4e5f6");

  // 3. Removing every network does not bring the compiled ones back.
  reset();
  vsFleetBegin("x", "t", ssids, pass, 3);
  vsFleetRemoveNetwork("Praktijk");
  vsFleetRemoveNetwork("Thuis");
  vsFleetLoaded = false;
  vsFleetBegin("x", "t", ssids, pass, 3);
  assert(vsFleetNetCount == 0);

  // 4. Upsert replaces, the list is capped, remove keeps order.
  reset();
  vsFleetBegin(nullptr, nullptr, nullptr, nullptr, 0);
  assert(vsFleetUpsertNetwork("A", "aaaaaaaa"));
  assert(vsFleetUpsertNetwork("A", "bbbbbbbb"));
  assert(vsFleetNetCount == 1 && vsFleetNets[0].pass == "bbbbbbbb");
  for (int i = 1; i < VS_FLEET_MAX_NETWORKS; ++i) {
    char n[4]; snprintf(n, sizeof(n), "N%d", i);
    assert(vsFleetUpsertNetwork(n, ""));
  }
  assert(!vsFleetUpsertNetwork("TooMany", "cccccccc"));
  assert(vsFleetUpsertNetwork("A", "dddddddd"));          // replacing still works when full
  vsFleetRemoveNetwork("N3");
  assert(vsFleetNetCount == VS_FLEET_MAX_NETWORKS - 1 && vsFleetNets[3].ssid == "N4");
  assert(!vsFleetUpsertNetwork(String(std::string(33, 'x')), ""));   // SSID max 32
  // survives a reboot
  vsFleetLoaded = false;
  vsFleetBegin(nullptr, nullptr, nullptr, nullptr, 0);
  assert(vsFleetNetCount == VS_FLEET_MAX_NETWORKS - 1 && vsFleetNets[0].pass == "dddddddd");

  // 5. Connection order: networks in range, strongest first; none in range -> all.
  reset();
  vsFleetBegin(nullptr, nullptr, nullptr, nullptr, 0);
  vsFleetUpsertNetwork("Ver", "12345678");
  vsFleetUpsertNetwork("Dichtbij", "12345678");
  vsFleetUpsertNetwork("Midden", "12345678");
  WiFi.seen = {{"Buren", -40}, {"Midden", -70}, {"Dichtbij", -50}, {"Midden", -65}};
  vsFleetPlanWifi();
  assert(vsFleetOrderCount == 3);
  assert(String(vsFleetWifiSsidAt(0)) == "Dichtbij");
  assert(String(vsFleetWifiSsidAt(1)) == "Midden");
  assert(String(vsFleetWifiSsidAt(2)) == "Ver");      // not seen, still tried last
  assert(String(vsFleetWifiSsidAt(3)) == "");
  WiFi.seen = {{"Buren", -40}};
  vsFleetPlanWifi();
  assert(vsFleetOrderCount == 3 && String(vsFleetWifiSsidAt(0)) == "Ver");

  // 6. Wi-Fi op high-water mark persists.
  vsFleetSaveWifiApplied(17);
  vsFleetLoaded = false;
  vsFleetBegin(nullptr, nullptr, nullptr, nullptr, 0);
  assert(vsFleetWifiApplied == 17);

  puts("fleet store: migration, new identity, no resurrection, cap/replace/remove, order, op mark passed");
}
