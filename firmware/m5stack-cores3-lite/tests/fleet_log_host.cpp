// Host test for src/fleet_log.h: numbering, token masking, the ring dropping
// whole old lines, SD flush/rotation and reading back after a marker.
// Build: g++ -std=c++17 -I tests/log_stubs -I tests/fleet_stubs tests/fleet_log_host.cpp
#include <cassert>
#include <cstdio>
#include <string>
#include "Arduino.h"
#include "../src/fleet_log.h"

static std::string ring() {
  std::string out(VS_LOG_RING_BYTES, 0); uint32_t last;
  out.resize(vsLogRingCopy(0, &out[0], out.size(), last)); return out;
}

int main() {
  vsLogBegin();
  assert(vsLogBoot == 1);
  vsLogSerial.print("BOOT: reset_reason=3\n");
  vsLogSerial.print("VSUSB INFO brian-x https://s SECRETTOKEN\n");
  vsLogSerial.print("VSUSB OK ENTER brian-x https://s SECRETTOKEN\n");
  vsLogSerial.print("half ");
  vsLogSerial.print("line\n");
  std::string r = ring();
  assert(r.find("B1.1 ") == 0 && r.find("Z 1000 BOOT: reset_reason=3\n") != std::string::npos);
  assert(r.find("SECRETTOKEN") == std::string::npos);
  assert(r.find("VSUSB INFO brian-x https://s <token>") != std::string::npos);
  assert(r.find("VSUSB OK ENTER brian-x https://s <token>") != std::string::npos);
  assert(r.find("B1.4 ") != std::string::npos && r.find(" 1000 half line\n") != std::string::npos);

  // muted while the PC owns the port
  static bool muted = true;
  vsLogMuteHook = []() { return muted; };
  vsLogSerial.print("\x01\x02 binary frame\n");
  muted = false;
  assert(ring().find("binary") == std::string::npos);

  // flush, then more lines, flush again: appended, nothing twice
  assert(vsLogFlushToSd());
  vsLogSerial.print("after flush\n");
  assert(vsLogFlushToSd());
  const std::string& cur = card().files[VS_LOG_CUR];
  assert(cur.find("B1.1 ") == 0 && cur.find("B1.5 ") != std::string::npos && cur.find(" after flush") != std::string::npos);
  assert(cur.find("B1.1 ", 1) == std::string::npos);

  // read back after a marker
  char buf[4096]; uint32_t b = 1, l = 3;
  size_t n = vsLogReadSd(b, l, buf, sizeof(buf));
  std::string got(buf, n);
  assert(got.find("B1.4 ") == 0 && b == 1 && l == 5);
  assert(vsLogReadSd(b, l, buf, sizeof(buf)) == 0);

  // the ring keeps whole lines when it overflows, and the gap is recorded
  std::string big(200, 'x');
  for (int i = 0; i < 3000; ++i) { vsLogSerial.print(big.c_str()); vsLogSerial.print("\n"); }
  r = ring();
  assert(r.size() <= VS_LOG_RING_BYTES && r[0] == 'B' && r.back() == '\n');
  assert(vsLogFlushToSd());
  assert(card().files[VS_LOG_CUR].find("regels niet bewaard") != std::string::npos);

  // rotation once the file passes 512 KiB
  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < 1500; ++i) { vsLogSerial.print(big.c_str()); vsLogSerial.print("\n"); }
    assert(vsLogFlushToSd());
  }
  assert(card().files.count(VS_LOG_OLD));
  puts("fleet log: numbering, token mask, mute, flush without repeats, read-after-marker, "
       "whole-line ring overflow with gap note, rotation passed");
}
