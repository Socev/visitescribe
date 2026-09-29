#pragma once
// Brian's own logbook: everything that goes to the serial port, kept.
//
// Until v0.9 the serial output existed only on the USB cable. Now every line
// is also:
//   1. numbered  "B<boot>.<line>"  (boot counter from NVS, line within boot)
//      and stamped with the wall clock (once SNTP has set it) and uptime;
//   2. kept in a 256 KiB ring buffer in PSRAM, from the first line of setup();
//   3. appended to /brianlog/log_cur.txt on the SD card -- only while Brian is
//      idle (never during a recording or a sync), rotated to log_old.txt at
//      512 KiB, so a log from before a crash or restart survives;
//   4. sent to the server at every sync (lines since the last upload; see
//      vsFleetUploadLogs in fleet_link.h), or all of it on request.
//
// Not captured: the binary USB-sync stream (muted while the PC owns the port)
// and the device token, which the VSUSB INFO/ENTER replies print (masked).
//
// This header replaces `Serial` for everything included after it with a tee
// that forwards every call unchanged to the real port.

#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

static constexpr size_t VS_LOG_RING_BYTES = 256U * 1024U;
static constexpr size_t VS_LOG_LINE_MAX = 300;
// StickS3: the logbook shares ~4 MB of flash with the recordings.
#if defined(VISITESCRIBE_BOARD_STICKS3)
static constexpr size_t VS_LOG_FILE_MAX = 96U * 1024U;
#else
static constexpr size_t VS_LOG_FILE_MAX = 512U * 1024U;
#endif
static const char* const VS_LOG_DIR = "/brianlog";
static const char* const VS_LOG_CUR = "/brianlog/log_cur.txt";
static const char* const VS_LOG_OLD = "/brianlog/log_old.txt";

static char* vsLogRing = nullptr;          // PSRAM
static size_t vsLogHead = 0;               // next write position
static size_t vsLogUsed = 0;               // bytes in ring
static uint32_t vsLogBoot = 0;
static uint32_t vsLogLine = 0;             // last committed line number this boot
static uint32_t vsLogFlushed = 0;          // last line number written to SD
static uint32_t vsLogDropped = 0;          // lines lost to a full ring before flushing
static char vsLogAcc[VS_LOG_LINE_MAX];
static size_t vsLogAccLen = 0;
// A mutex, not a spinlock: committing a line reads the clock and formats
// text, which must not happen with interrupts disabled.
static SemaphoreHandle_t vsLogMutex = nullptr;
static bool vsLogLock() {
  return vsLogMutex && xSemaphoreTakeRecursive(vsLogMutex, pdMS_TO_TICKS(50)) == pdTRUE;
}
static void vsLogUnlock() { xSemaphoreGiveRecursive(vsLogMutex); }
static bool (*vsLogMuteHook)() = nullptr;  // true while the PC owns the port (binary)
static vprintf_like_t vsLogPrevVprintf = nullptr;

// ---------------------------------------------------------------------------
// ring buffer
// ---------------------------------------------------------------------------

static void vsLogRingDropOldestLine() {
  // Advance the tail past the next newline.
  size_t tail = (vsLogHead + VS_LOG_RING_BYTES - vsLogUsed) % VS_LOG_RING_BYTES;
  while (vsLogUsed) {
    const char c = vsLogRing[tail];
    tail = (tail + 1) % VS_LOG_RING_BYTES;
    --vsLogUsed;
    if (c == '\n') break;
  }
}

// Caller holds the log lock.
static void vsLogRingAppend(const char* s, size_t n) {
  if (!vsLogRing || n >= VS_LOG_RING_BYTES) return;
  while (vsLogUsed + n > VS_LOG_RING_BYTES) {
    vsLogRingDropOldestLine();
  }
  for (size_t i = 0; i < n; ++i) {
    vsLogRing[vsLogHead] = s[i];
    vsLogHead = (vsLogHead + 1) % VS_LOG_RING_BYTES;
  }
  vsLogUsed += n;
}

static uint32_t vsLogLineNumberAt(const char* line) {
  // "B<boot>.<line> ..." -> line
  const char* dot = strchr(line, '.');
  return dot ? (uint32_t)strtoul(dot + 1, nullptr, 10) : 0;
}

// Caller holds the log lock.
static void vsLogCommitLocked(const char* text, size_t len) {
  // Mask the device token in the replies that print it.
  char masked[VS_LOG_LINE_MAX];
  if (len > 6 && (strncmp(text, "VSUSB INFO ", 11) == 0 ||
                  strncmp(text, "VSUSB OK ENTER ", 15) == 0)) {
    size_t spaces = 0, cut = len;
    for (size_t i = 0; i < len; ++i) {
      if (text[i] == ' ' && ++spaces == (text[6] == 'O' ? 5 : 4)) { cut = i; break; }
    }
    const size_t keep = cut < sizeof(masked) - 12 ? cut : sizeof(masked) - 12;
    memcpy(masked, text, keep);
    memcpy(masked + keep, " <token>", 8);
    text = masked;
    len = keep + 8;
  }
  char prefix[64];
  const time_t now = time(nullptr);
  char when[24] = "-";
  if (now > 1700000000) {
    struct tm t;
    gmtime_r(&now, &t);
    strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &t);
  }
  const int p = snprintf(prefix, sizeof(prefix), "B%lu.%lu %s %lu ",
                         (unsigned long)vsLogBoot, (unsigned long)(++vsLogLine), when,
                         (unsigned long)millis());
  vsLogRingAppend(prefix, (size_t)p);
  vsLogRingAppend(text, len);
  vsLogRingAppend("\n", 1);
}

static void vsLogCapture(const uint8_t* data, size_t n) {
  if (!vsLogRing || (vsLogMuteHook && vsLogMuteHook())) return;
  if (xPortInIsrContext() || !vsLogLock()) return;   // never block a print
  for (size_t i = 0; i < n; ++i) {
    const char c = (char)data[i];
    if (c == '\r') continue;
    if (c == '\n' || vsLogAccLen >= VS_LOG_LINE_MAX - 1) {
      if (vsLogAccLen) vsLogCommitLocked(vsLogAcc, vsLogAccLen);
      vsLogAccLen = 0;
      if (c == '\n') continue;
    }
    // Keep it text: control characters and stray binary become '?'.
    vsLogAcc[vsLogAccLen++] = ((uint8_t)c < 0x20 && c != '\t') ? '?' : c;
  }
  vsLogUnlock();
}

// ESP-IDF's own log lines (E (2897) I2S: ...) go through here too.
static int vsLogVprintf(const char* fmt, va_list args) {
  char buf[VS_LOG_LINE_MAX];
  va_list copy;
  va_copy(copy, args);
  const int n = vsnprintf(buf, sizeof(buf), fmt, copy);
  va_end(copy);
  if (n > 0) vsLogCapture(reinterpret_cast<const uint8_t*>(buf),
                          (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
  return vsLogPrevVprintf ? vsLogPrevVprintf(fmt, args) : vprintf(fmt, args);
}

// Start of setup(): counts the boot and allocates the ring.
static void vsLogBegin() {
  if (vsLogRing) return;
  Preferences p;
  if (p.begin("vslog", false)) {
    vsLogBoot = p.getUInt("boot", 0) + 1;
    p.putUInt("boot", vsLogBoot);
    p.end();
  }
  vsLogMutex = xSemaphoreCreateRecursiveMutex();
  vsLogRing = static_cast<char*>(
      heap_caps_malloc(VS_LOG_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  vsLogPrevVprintf = esp_log_set_vprintf(vsLogVprintf);
}

// ---------------------------------------------------------------------------
// reading the ring (oldest first), line by line
// ---------------------------------------------------------------------------

// Copies the ring's complete lines with line number > `after` (this boot)
// into `out` (max `cap` bytes, whole lines only). Returns bytes copied and the
// last line number copied in `last`.
static size_t vsLogRingCopy(uint32_t after, char* out, size_t cap, uint32_t& last) {
  size_t written = 0;
  last = after;
  if (!vsLogRing || !vsLogLock()) return 0;
  size_t pos = (vsLogHead + VS_LOG_RING_BYTES - vsLogUsed) % VS_LOG_RING_BYTES;
  size_t left = vsLogUsed;
  char line[VS_LOG_LINE_MAX + 72];
  size_t len = 0;
  while (left) {
    const char c = vsLogRing[pos];
    pos = (pos + 1) % VS_LOG_RING_BYTES;
    --left;
    if (len < sizeof(line) - 1) line[len++] = c;
    if (c != '\n') continue;
    line[len] = 0;
    const uint32_t no = vsLogLineNumberAt(line);
    if (no > after) {
      if (written + len > cap) break;
      memcpy(out + written, line, len);
      written += len;
      last = no;
    }
    len = 0;
  }
  vsLogUnlock();
  return written;
}

// Line number of the oldest line still in the ring (0 when empty).
static uint32_t vsLogRingOldest() {
  if (!vsLogRing || !vsLogLock()) return 0;
  uint32_t no = 0;
  if (vsLogUsed) {
    char head[32];
    size_t pos = (vsLogHead + VS_LOG_RING_BYTES - vsLogUsed) % VS_LOG_RING_BYTES;
    const size_t n = vsLogUsed < sizeof(head) - 1 ? vsLogUsed : sizeof(head) - 1;
    for (size_t i = 0; i < n; ++i) head[i] = vsLogRing[(pos + i) % VS_LOG_RING_BYTES];
    head[n] = 0;
    no = vsLogLineNumberAt(head);
  }
  vsLogUnlock();
  return no;
}

// ---------------------------------------------------------------------------
// SD card
// ---------------------------------------------------------------------------

// Appends the lines not yet on SD. Call only while nothing else uses the card
// (not recording, not syncing, not in USB maintenance).
static bool vsLogFlushToSd() {
  if (!vsLogRing || vsLogLine <= vsLogFlushed) return true;
  if (!SD.exists(VS_LOG_DIR) && !SD.mkdir(VS_LOG_DIR)) return false;
  File f = SD.open(VS_LOG_CUR, FILE_APPEND);
  if (!f) return false;
  if (f.size() > VS_LOG_FILE_MAX) {
    f.close();
    SD.remove(VS_LOG_OLD);
    SD.rename(VS_LOG_CUR, VS_LOG_OLD);
    f = SD.open(VS_LOG_CUR, FILE_APPEND);
    if (!f) return false;
  }
  static constexpr size_t CHUNK = 16 * 1024;
  char* buf = static_cast<char*>(heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buf) {
    f.close();
    return false;
  }
  // Lines that fell out of the ring before we got here.
  const uint32_t oldestInRing = vsLogRingOldest();
  if (oldestInRing > vsLogFlushed + 1) {
    const uint32_t lost = oldestInRing - vsLogFlushed - 1;
    f.printf("B%lu.%lu - %lu LOG: %lu regels niet bewaard (buffer vol tijdens lange opname)\n",
             (unsigned long)vsLogBoot, (unsigned long)(oldestInRing - 1), (unsigned long)millis(),
             (unsigned long)lost);
    vsLogDropped += lost;
  }
  bool ok = true;
  for (;;) {
    uint32_t last = vsLogFlushed;
    const size_t n = vsLogRingCopy(vsLogFlushed, buf, CHUNK, last);
    if (!n) break;
    if (f.write(reinterpret_cast<const uint8_t*>(buf), n) != n) {
      ok = false;
      break;
    }
    vsLogFlushed = last;
  }
  free(buf);
  f.flush();
  f.close();
  return ok;
}

// Reads whole lines with (boot, line) > (afterBoot, afterLine) from the SD
// files, oldest first, into out (max cap bytes). Continues where it left off
// through the returned marker; returns bytes copied.
static bool vsLogKeyAfter(const char* line, uint32_t afterBoot, uint32_t afterLine,
                          uint32_t& boot, uint32_t& no) {
  if (line[0] != 'B') return false;
  char* end = nullptr;
  boot = (uint32_t)strtoul(line + 1, &end, 10);
  if (!end || *end != '.') return false;
  no = (uint32_t)strtoul(end + 1, nullptr, 10);
  return boot > afterBoot || (boot == afterBoot && no > afterLine);
}

static size_t vsLogReadSd(uint32_t& afterBoot, uint32_t& afterLine, char* out, size_t cap) {
  size_t written = 0;
  const char* files[2] = {VS_LOG_OLD, VS_LOG_CUR};
  static constexpr size_t RBUF = 4096;
  char* rbuf = static_cast<char*>(heap_caps_malloc(RBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!rbuf) return 0;
  char line[VS_LOG_LINE_MAX + 72];
  bool full = false;
  for (const char* path : files) {
    if (full) break;
    File f = SD.open(path, FILE_READ);
    if (!f) continue;
    size_t len = 0;
    for (;;) {
      const int got = f.read(reinterpret_cast<uint8_t*>(rbuf), RBUF);
      if (got <= 0) break;
      for (int i = 0; i < got; ++i) {
        const char c = rbuf[i];
        if (len < sizeof(line) - 1) line[len++] = c;
        if (c != '\n') continue;
        line[len] = 0;
        uint32_t b, n;
        if (vsLogKeyAfter(line, afterBoot, afterLine, b, n)) {
          if (written + len > cap) { full = true; break; }
          memcpy(out + written, line, len);
          written += len;
          afterBoot = b;
          afterLine = n;
        }
        len = 0;
      }
      if (full) break;
    }
    f.close();
  }
  free(rbuf);
  return written;
}

// ---------------------------------------------------------------------------
// the Serial tee
// ---------------------------------------------------------------------------

class VsLogSerial : public Stream {
 public:
  using Print::write;
  size_t write(uint8_t c) override {
    vsLogCapture(&c, 1);
    return ::Serial.write(c);
  }
  size_t write(const uint8_t* buf, size_t n) override {
    vsLogCapture(buf, n);
    return ::Serial.write(buf, n);
  }
  int available() override { return ::Serial.available(); }
  int read() override { return ::Serial.read(); }
  int peek() override { return ::Serial.peek(); }
  void flush() override { ::Serial.flush(); }
  int availableForWrite() override { return ::Serial.availableForWrite(); }
  void begin(unsigned long baud) { ::Serial.begin(baud); }
  void end() { ::Serial.end(); }
  bool isPlugged() { return ::Serial.isPlugged(); }
  void setTxTimeoutMs(uint32_t ms) { ::Serial.setTxTimeoutMs(ms); }
  operator bool() const { return (bool)::Serial; }
};

static VsLogSerial vsLogSerial;
