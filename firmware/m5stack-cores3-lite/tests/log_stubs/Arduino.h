// Host stub for tests/fleet_log_host.cpp.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
inline unsigned long& fakeMillis() { static unsigned long m = 1000; return m; }
inline unsigned long millis() { return fakeMillis(); }
class String : public std::string {
 public:
  String() = default;
  String(const char* s) : std::string(s ? s : "") {}
  String(const std::string& s) : std::string(s) {}
  size_t length() const { return size(); }
};
struct Print {
  virtual ~Print() = default;
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t*, size_t) = 0;
  size_t write(const char* s) { return write((const uint8_t*)s, strlen(s)); }
  size_t print(const char* s) { return write((const uint8_t*)s, strlen(s)); }
};
struct Stream : Print {
  virtual int available() = 0; virtual int read() = 0; virtual int peek() = 0;
  virtual void flush() {} virtual int availableForWrite() { return 0; }
};
struct HWCDCStub {
  size_t write(uint8_t) { return 1; } size_t write(const uint8_t*, size_t n) { return n; }
  int available() { return 0; } int read() { return -1; } int peek() { return -1; }
  void flush() {} int availableForWrite() { return 64; } void begin(unsigned long) {}
  void end() {} bool isPlugged() { return true; } void setTxTimeoutMs(uint32_t) {}
  explicit operator bool() const { return true; }
};
static HWCDCStub Serial;
