// Host stub: an in-memory NVS namespace.
#pragma once
#include <map>
#include "Arduino.h"
struct FakeNvs { std::map<std::string, std::string> s; std::map<std::string, uint32_t> u; };
inline FakeNvs& fakeNvs() { static FakeNvs n; return n; }
class Preferences {
 public:
  bool begin(const char*, bool) { return true; }
  void end() {}
  String getString(const char* k, const char* d) { auto& m = fakeNvs().s; return m.count(k) ? String(m[k]) : String(d); }
  size_t putString(const char* k, const String& v) { fakeNvs().s[k] = v; return v.size(); }
  uint8_t getUChar(const char* k, uint8_t d) { auto& m = fakeNvs().u; return m.count(k) ? (uint8_t)m[k] : d; }
  size_t putUChar(const char* k, uint8_t v) { fakeNvs().u[k] = v; return 1; }
  uint32_t getUInt(const char* k, uint32_t d) { auto& m = fakeNvs().u; return m.count(k) ? m[k] : d; }
  size_t putUInt(const char* k, uint32_t v) { fakeNvs().u[k] = v; return 4; }
  bool getBool(const char* k, bool d) { auto& m = fakeNvs().u; return m.count(k) ? m[k] != 0 : d; }
  size_t putBool(const char* k, bool v) { fakeNvs().u[k] = v; return 1; }
  bool remove(const char* k) { fakeNvs().s.erase(k); fakeNvs().u.erase(k); return true; }
};
