// Host stub for tests/fleet_store_host.cpp: just enough Arduino for fleet_store.h.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
class String : public std::string {
 public:
  String() = default;
  String(const char* s) : std::string(s ? s : "") {}
  String(const std::string& s) : std::string(s) {}
  String(unsigned v) : std::string(std::to_string(v)) {}
  size_t length() const { return size(); }
  String substring(size_t a) const { return substr(a); }
  void toUpperCase() { for (auto& c : *this) c = (char)toupper(c); }
};
inline String operator+(const String& a, const char* b) { return String(std::string(a) + b); }
inline String operator+(const char* a, const String& b) { return String(std::string(a) + std::string(b)); }
inline String operator+(const String& a, const String& b) { return String(std::string(a) + std::string(b)); }
inline void delay(int) {}
struct SerialStub { template <typename... T> void printf(const char*, T...) {} void println(const char*) {} };
static SerialStub Serial;
