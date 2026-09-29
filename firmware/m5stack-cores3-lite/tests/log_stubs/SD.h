// Host stub for tests/fleet_log_host.cpp: an in-memory card.
#pragma once
#include <map>
#include "Arduino.h"
enum { FILE_READ = 0, FILE_APPEND = 1 };
struct FakeCard { std::map<std::string, std::string> files; };
inline FakeCard& card() { static FakeCard c; return c; }
class File {
 public:
  std::string path; size_t pos = 0; bool valid = false; int mode = 0;
  File() = default;
  File(const std::string& p, int m) : path(p), valid(true), mode(m) {}
  explicit operator bool() const { return valid; }
  size_t size() { return card().files[path].size(); }
  size_t write(const uint8_t* b, size_t n) { card().files[path].append((const char*)b, n); return n; }
  template <typename... T> int printf(const char* f, T... a) {
    char buf[512]; int n = snprintf(buf, sizeof(buf), f, a...); card().files[path] += buf; return n; }
  int read(uint8_t* b, size_t n) { auto& s = card().files[path]; size_t k = std::min(n, s.size() - pos);
    memcpy(b, s.data() + pos, k); pos += k; return (int)k; }
  void flush() {} void close() { valid = false; }
};
struct FakeSD {
  bool exists(const char* p) { return std::string(p) == "/brianlog" || card().files.count(p); }
  bool mkdir(const char*) { return true; }
  File open(const char* p, int m) { if (m == FILE_READ && !card().files.count(p)) return File();
    card().files[p]; return File(p, m); }
  bool remove(const char* p) { return card().files.erase(p) > 0; }
  bool rename(const char* a, const char* b) { card().files[b] = card().files[a]; card().files.erase(a); return true; }
};
static FakeSD SD;
