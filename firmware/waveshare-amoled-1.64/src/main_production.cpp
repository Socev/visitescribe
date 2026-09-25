#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>
#include <math.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"

#if __has_include("server_secrets.h")
#include "server_secrets.h"
#endif

#ifndef VISITESCRIBE_SERVER_BASE_URL
#define VISITESCRIBE_SERVER_BASE_URL "https://scribe.primumnonnocere.olares.com"
#endif
#ifndef VISITESCRIBE_DEVICE_ID
#define VISITESCRIBE_DEVICE_ID "visitescribe-waveshare-001"
#endif
#ifndef VISITESCRIBE_DEVICE_TOKEN
#define VISITESCRIBE_DEVICE_TOKEN ""
#endif

// -----------------------------------------------------------------------------
// VisiteScribe MINI production firmware v0.1
// Target: Waveshare ESP32-S3-Touch-AMOLED-1.64 V2 + dual IM73D122 PDM mics.
//
// Product invariants:
// - 48 kHz / 16-bit / stereo WAV on microSD is the source-of-truth master.
// - privacy pause disables the PDM receiver; no PCM is captured in that state.
// - sync never deletes or rewrites a master WAV.
// - PC sync uses the same VSUSB v1 protocol as the CoreS3-Lite.
// - USB hot-plug recovery restarts only the HW CDC peripheral.
// -----------------------------------------------------------------------------

static constexpr int SCREEN_W = 280;
static constexpr int SCREEN_H = 456;
static constexpr int HEADER_H = 64;

static constexpr int LCD_CS = 46;
static constexpr int LCD_SCLK = 10;
static constexpr int LCD_D0 = 11;
static constexpr int LCD_D1 = 12;
static constexpr int LCD_D2 = 13;
static constexpr int LCD_D3 = 14;
static constexpr int LCD_RST = 21;

static constexpr int TOUCH_SDA = 47;
static constexpr int TOUCH_SCL = 48;
static constexpr uint8_t TOUCH_ADDR = 0x38;

static constexpr int SD_MISO = 40;
static constexpr int SD_MOSI = 39;
static constexpr int SD_SCLK = 41;
static constexpr int SD_CS = 38;

static constexpr int BAT_ADC_GPIO = 4;
static constexpr float BAT_DIVIDER = 3.0f;
static constexpr int BOOT_GPIO = 0;

// Dual IM73D122: one PDM clock and one shared data line, SELECT opposite.
static constexpr int PDM_CLK_GPIO = 5;
static constexpr int PDM_DATA_GPIO = 6;

static constexpr uint32_t AUDIO_RATE = 48000;
static constexpr uint16_t AUDIO_CHANNELS = 2;
static constexpr uint16_t AUDIO_BITS = 16;
static constexpr uint32_t AUDIO_BYTES_PER_SECOND =
    AUDIO_RATE * AUDIO_CHANNELS * (AUDIO_BITS / 8);

static constexpr uint8_t DISPLAY_BRIGHTNESS_ACTIVE = 0xD0;
static constexpr uint8_t DISPLAY_BRIGHTNESS_DIM = 0x28;
static constexpr uint32_t DISPLAY_IDLE_DIM_MS = 12000;
static constexpr uint32_t DISPLAY_IDLE_OFF_MS = 60000;
static constexpr uint32_t FINISHED_AUTO_HOME_MS = 8000;

static constexpr uint16_t C_BG       = 0xDEDF;
static constexpr uint16_t C_BLUE     = 0x225D;
static constexpr uint16_t C_VIOLET   = 0x633D;
static constexpr uint16_t C_TEAL     = 0x2575;
static constexpr uint16_t C_NAVY     = 0x1084;
static constexpr uint16_t C_WHITE    = 0xFFFF;
static constexpr uint16_t C_LINE     = 0xADBF;
static constexpr uint16_t C_GREEN    = 0x35CF;
static constexpr uint16_t C_RED      = 0xEA4B;
static constexpr uint16_t C_AMBER    = 0xFD47;
static constexpr uint16_t C_MUTED    = 0x6B6D;

Arduino_DataBus* displayBus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_CO5300* panel = new Arduino_CO5300(
    displayBus, LCD_RST, 0, SCREEN_W, SCREEN_H, 20, 0, 180, 24);
Arduino_GFX* gfx = panel;
SPIClass sdSPI(HSPI);

struct Rect {
  int x, y, w, h;
  bool contains(uint16_t px, uint16_t py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

static const Rect HOME_VISIT   {0,  64, 280, 98};
static const Rect HOME_ROUND   {0, 162, 280, 98};
static const Rect HOME_MEETING {0, 260, 280, 98};
static const Rect HOME_MENU    {0, 358, 280, 98};
static const Rect TWO_TOP      {0,  64, 280,196};
static const Rect TWO_BOTTOM   {0, 260, 280,196};
static const Rect THREE_TOP    {0,  64, 280,131};
static const Rect THREE_MIDDLE {0, 195, 280,131};
static const Rect THREE_BOTTOM {0, 326, 280,130};
static const Rect STATUS_BACK  {0, 348, 280,108};
static const Rect USB_BACK     {0, 348, 280,108};

enum class AppState : uint8_t {
  HOME, MODE_CONFIRM, RECORDING, PAUSED, FINISHED, MENU, STATUS, USB_INFO
};
enum class Mode : uint8_t { VISIT, ROUND, MEETING };

AppState state = AppState::HOME;
Mode selectedMode = Mode::VISIT;

bool sdOk = false;
bool pdmReady = false;
volatile bool captureEnabled = false;
bool pdmChannelEnabled = false;
bool audioError = false;
bool sessionOpen = false;
bool segmentOpen = false;

uint16_t sessionId = 0;
uint16_t patientNumber = 1;
uint16_t segmentNumber = 1;
uint16_t markerCount = 0;
uint32_t sessionStartedMs = 0;
uint32_t pauseStartedMs = 0;
uint32_t totalPausedMs = 0;
uint32_t finishedAtMs = 0;

char eventsPath[96] = {0};
char wavTmpPath[128] = {0};
char wavFinalPath[128] = {0};
File wavFile;
uint32_t wavDataBytes = 0;

bool touchWasDown = false;
bool bootWasDown = false;
uint32_t bootDownSince = 0;

int batteryPercent = -1;
uint16_t batteryMv = 0;
uint32_t lastBatteryReadMs = 0;
int lastBatteryUiPercent = -999;

uint32_t lastUserActivityMs = 0;
uint32_t lastDisplayedSecond = UINT32_MAX;
bool displayDimmed = false;
bool displayFullyOff = false;
bool screenDirty = true;

i2s_chan_handle_t pdmRx = nullptr;
TaskHandle_t audioTaskHandle = nullptr;
SemaphoreHandle_t wavMutex = nullptr;

// -----------------------------------------------------------------------------
// WAV
// -----------------------------------------------------------------------------
struct __attribute__((packed)) WAVHeader {
  char riff[4] = {'R','I','F','F'};
  uint32_t fileSize = 36;
  char wave[4] = {'W','A','V','E'};
  char fmt[4] = {'f','m','t',' '};
  uint32_t fmtSize = 16;
  uint16_t audioFormat = 1;
  uint16_t numChannels = AUDIO_CHANNELS;
  uint32_t sampleRate = AUDIO_RATE;
  uint32_t byteRate = AUDIO_BYTES_PER_SECOND;
  uint16_t blockAlign = AUDIO_CHANNELS * (AUDIO_BITS / 8);
  uint16_t bitsPerSample = AUDIO_BITS;
  char data[4] = {'d','a','t','a'};
  uint32_t dataSize = 0;
};

static void writeWavHeader(File& f, uint32_t bytes) {
  WAVHeader h;
  h.fileSize = 36 + bytes;
  h.dataSize = bytes;
  f.seek(0);
  f.write(reinterpret_cast<const uint8_t*>(&h), sizeof(h));
  f.flush();
}

static bool readWavHeader(File& f, WAVHeader& h) {
  if (f.read(reinterpret_cast<uint8_t*>(&h), sizeof(h)) != sizeof(h)) return false;
  return memcmp(h.riff, "RIFF", 4) == 0 &&
         memcmp(h.wave, "WAVE", 4) == 0 &&
         memcmp(h.fmt, "fmt ", 4) == 0 &&
         memcmp(h.data, "data", 4) == 0 &&
         h.audioFormat == 1 &&
         h.bitsPerSample == 16 &&
         h.numChannels > 0 &&
         h.sampleRate > 0;
}

// -----------------------------------------------------------------------------
// Drawing
// -----------------------------------------------------------------------------
static void textAt(int x, int y, const char* text, uint16_t color, uint8_t size = 1) {
  gfx->setTextColor(color);
  gfx->setTextSize(size);
  gfx->setCursor(x, y);
  gfx->print(text);
}

static void textAtBold(int x, int y, const char* text, uint16_t color, uint8_t size = 1) {
  textAt(x, y, text, color, size);
  textAt(x + 1, y, text, color, size);
  textAt(x, y + 1, text, color, size);
  if (size >= 2) textAt(x + 1, y + 1, text, color, size);
}

static void centeredBold(int y, const char* text, uint16_t color, uint8_t size = 1) {
  int width = static_cast<int>(strlen(text)) * 6 * size;
  int x = (SCREEN_W - width) / 2;
  if (x < 2) x = 2;
  textAtBold(x, y, text, color, size);
}

static uint8_t bestLabelSize(const char* label) {
  const size_t n = strlen(label);
  if (n <= 6) return 4;
  if (n <= 13) return 3;
  return 2;
}

static void bigZone(const Rect& r, const char* label, uint16_t fill,
                    uint16_t textColor, const char* subtitle = nullptr) {
  gfx->fillRect(r.x, r.y, r.w, r.h, fill);
  gfx->drawFastHLine(r.x, r.y, r.w, C_WHITE);
  const uint8_t size = bestLabelSize(label);
  int labelY = r.y + (r.h - 8 * size) / 2;

  if (subtitle && subtitle[0]) labelY -= 16;
  int width = static_cast<int>(strlen(label)) * 6 * size;
  int x = r.x + (r.w - width) / 2;
  if (x < 2) x = 2;
  textAtBold(x, labelY, label, textColor, size);

  if (subtitle && subtitle[0]) {
    int sw = static_cast<int>(strlen(subtitle)) * 6;
    int sx = r.x + (r.w - sw) / 2;
    if (sx < 2) sx = 2;
    textAtBold(sx, labelY + 8 * size + 18, subtitle, textColor, 1);
  }
}

static void drawOurMindMarkMini(int cx, int cy, uint16_t color) {
  gfx->drawRoundRect(cx - 3, cy - 9, 6, 11, 3, color);
  gfx->drawRoundRect(cx - 3, cy - 1, 6, 11, 3, color);
  gfx->drawRoundRect(cx - 9, cy - 3, 11, 6, 3, color);
  gfx->drawRoundRect(cx - 1, cy - 3, 11, 6, 3, color);
  gfx->fillRect(cx - 1, cy - 4, 2, 9, color);
  gfx->fillRect(cx - 4, cy - 1, 9, 2, color);
}

static int batteryPercentFromMv(uint16_t mv) {
  struct Pt { uint16_t mv; uint8_t pct; };
  static const Pt curve[] = {
    {3300,0},{3500,5},{3600,12},{3680,20},{3740,30},{3790,40},
    {3830,50},{3870,60},{3920,70},{3980,80},{4070,90},{4200,100}
  };
  if (mv <= curve[0].mv) return 0;
  if (mv >= curve[11].mv) return 100;
  for (size_t i = 1; i < sizeof(curve)/sizeof(curve[0]); ++i) {
    if (mv <= curve[i].mv) {
      const uint16_t span = curve[i].mv - curve[i-1].mv;
      const int pctSpan = curve[i].pct - curve[i-1].pct;
      return curve[i-1].pct + ((mv - curve[i-1].mv) * pctSpan) / span;
    }
  }
  return 100;
}

static bool updateBattery(bool force = false) {
  const uint32_t now = millis();
  if (!force && now - lastBatteryReadMs < 5000) return false;
  lastBatteryReadMs = now;
  uint32_t sum = 0;
  for (int i = 0; i < 12; ++i) {
    sum += analogReadMilliVolts(BAT_ADC_GPIO);
    delay(2);
  }
  const uint16_t measured = static_cast<uint16_t>((sum / 12) * BAT_DIVIDER);
  const int old = batteryPercent;
  if (measured < 2800 || measured > 4500) {
    batteryMv = 0;
    batteryPercent = -1;
  } else {
    batteryMv = measured;
    batteryPercent = batteryPercentFromMv(measured);
  }
  return old != batteryPercent;
}

static void drawBatteryBadge() {
  char out[16];
  if (batteryPercent < 0) snprintf(out, sizeof(out), "--%%");
  else snprintf(out, sizeof(out), "%d%%", batteryPercent);
  gfx->fillRect(224, 2, 56, 20, C_WHITE);
  const int width = static_cast<int>(strlen(out)) * 6;
  textAtBold(274 - width, 7, out,
             (batteryPercent >= 0 && batteryPercent <= 15) ? C_RED : C_NAVY, 1);
  lastBatteryUiPercent = batteryPercent;
}

static void drawTopBar(const char* status, const char* sub = nullptr) {
  gfx->fillRect(0, 0, SCREEN_W, HEADER_H, C_WHITE);
  drawOurMindMarkMini(13, 13, C_BLUE);
  textAtBold(29, 7, "OurMind", C_BLUE, 1);
  drawBatteryBadge();
  centeredBold(27, status, C_NAVY, strlen(status) <= 17 ? 2 : 1);
  if (sub && sub[0]) centeredBold(49, sub, C_MUTED, 1);
  gfx->drawFastHLine(0, HEADER_H - 1, SCREEN_W, C_LINE);
}

static const char* modeTitle(Mode m) {
  if (m == Mode::VISIT) return "VISITE";
  if (m == Mode::ROUND) return "PATIENTRONDE";
  return "VERGADERING";
}

static uint32_t activeElapsedMs() {
  if (!sessionOpen || !sessionStartedMs) return 0;
  uint32_t paused = totalPausedMs;
  if (state == AppState::PAUSED) paused += millis() - pauseStartedMs;
  return millis() - sessionStartedMs - paused;
}

static void formatElapsed(char* out, size_t len) {
  const uint32_t sec = activeElapsedMs() / 1000;
  snprintf(out, len, "%02lu:%02lu:%02lu",
           (unsigned long)(sec / 3600),
           (unsigned long)((sec % 3600) / 60),
           (unsigned long)(sec % 60));
}

static void drawHome() {
  drawTopBar("HOOFDMENU", "USB sync automatisch");
  bigZone(HOME_VISIT, "VISITE", C_BLUE, C_WHITE, "1 patient");
  bigZone(HOME_ROUND, "PATIENTRONDE", C_VIOLET, C_WHITE, "aparte audio per patient");
  bigZone(HOME_MEETING, "VERGADERING", C_TEAL, C_WHITE, "overleg / bespreking");
  bigZone(HOME_MENU, "MENU", C_NAVY, C_WHITE, "status en USB");
}

static void drawModeConfirm() {
  drawTopBar(modeTitle(selectedMode));
  bigZone(TWO_TOP, "START", C_GREEN, C_WHITE, "48k stereo master");
  bigZone(TWO_BOTTOM, "TERUG", C_NAVY, C_WHITE);
}

static void drawRecording() {
  char title[40], elapsed[16];
  formatElapsed(elapsed, sizeof(elapsed));
  if (selectedMode == Mode::ROUND)
    snprintf(title, sizeof(title), "OPNAME PATIENT %u", patientNumber);
  else
    snprintf(title, sizeof(title), "OPNAME %s",
             selectedMode == Mode::VISIT ? "VISITE" : "VERGADERING");
  drawTopBar(title, elapsed);
  bigZone(THREE_TOP, "PRIVACY", C_AMBER, C_NAVY, "PDM microfoons uit");
  bigZone(THREE_MIDDLE,
          selectedMode == Mode::ROUND ? "VOLGENDE" : "MARKER",
          C_BLUE, C_WHITE,
          selectedMode == Mode::ROUND ? "nieuw audiobestand" : "markeer dit moment");
  bigZone(THREE_BOTTOM, "STOP", C_RED, C_WHITE);
  lastDisplayedSecond = activeElapsedMs() / 1000;
}

static void drawPaused() {
  char elapsed[16];
  formatElapsed(elapsed, sizeof(elapsed));
  drawTopBar("PRIVACY PAUZE", elapsed);
  bigZone(THREE_TOP, "HERVAT", C_GREEN, C_WHITE, "microfoons weer aan");
  bigZone(THREE_MIDDLE,
          selectedMode == Mode::ROUND ? "VOLGENDE" : "MARKER",
          C_BLUE, C_WHITE);
  bigZone(THREE_BOTTOM, "STOP", C_RED, C_WHITE);
  lastDisplayedSecond = activeElapsedMs() / 1000;
}

static void drawFinished() {
  drawTopBar("OPNAME OPGESLAGEN", "8 sec voor VUL AAN");
  bigZone(TWO_TOP, "VUL AAN", C_BLUE, C_WHITE, "zelfde sessie, nieuw segment");
  bigZone(TWO_BOTTOM, "KLAAR", C_GREEN, C_WHITE, "klaar voor USB-sync");
}

static uint16_t pendingSessionCount();

static void drawStatus() {
  drawTopBar("STATUS");
  gfx->fillRect(0, HEADER_H, SCREEN_W, 284, C_BG);
  char line[64];
  snprintf(line, sizeof(line), "ACCU  %s%d%%  %umV",
           batteryPercent < 0 ? "--" : "", batteryPercent < 0 ? 0 : batteryPercent,
           batteryMv);
  centeredBold(92, line, C_NAVY, 1);
  centeredBold(132, sdOk ? "MICROSD  OK" : "MICROSD  FOUT",
               sdOk ? C_GREEN : C_RED, 2);
  centeredBold(176, pdmReady ? "MICROFOONS  2/2 PDM OK" : "MICROFOONS  FOUT",
               pdmReady ? C_GREEN : C_RED, 1);
  snprintf(line, sizeof(line), "AUDIO  %s",
           audioError ? "SCHRIJFFOUT" : "48k stereo gereed");
  centeredBold(214, line, audioError ? C_RED : C_NAVY, 1);
  snprintf(line, sizeof(line), "WACHT OP SYNC  %u", pendingSessionCount());
  centeredBold(252, line, C_BLUE, 1);
  centeredBold(290, "USB-C aansluiten = automatisch", C_MUTED, 1);
  bigZone(STATUS_BACK, "TERUG", C_NAVY, C_WHITE);
}

static void drawUsbInfo() {
  drawTopBar("USB SYNC", "automatisch");
  gfx->fillRect(0, HEADER_H, SCREEN_W, 284, C_BG);
  centeredBold(105, "SLUIT USB-C AAN", C_BLUE, 2);
  centeredBold(150, "PC-app detecteert recorder", C_NAVY, 1);
  centeredBold(184, "en start sync vanzelf", C_NAVY, 1);
  centeredBold(232, "Master WAV blijft op SD", C_GREEN, 1);
  centeredBold(270, "Geen knop op recorder nodig", C_MUTED, 1);
  bigZone(USB_BACK, "TERUG", C_NAVY, C_WHITE);
}

static void render(bool force = false) {
  if (!force && !screenDirty) {
    if ((state == AppState::RECORDING || state == AppState::PAUSED) &&
        activeElapsedMs() / 1000 != lastDisplayedSecond) {
      screenDirty = true;
    } else {
      return;
    }
  }
  screenDirty = false;
  switch (state) {
    case AppState::HOME: drawHome(); break;
    case AppState::MODE_CONFIRM: drawModeConfirm(); break;
    case AppState::RECORDING: drawRecording(); break;
    case AppState::PAUSED: drawPaused(); break;
    case AppState::FINISHED: drawFinished(); break;
    case AppState::MENU:
      drawTopBar("MENU");
      bigZone(THREE_TOP, "STATUS", C_BLUE, C_WHITE);
      bigZone(THREE_MIDDLE, "USB SYNC", C_TEAL, C_WHITE, "automatisch via PC");
      bigZone(THREE_BOTTOM, "TERUG", C_NAVY, C_WHITE);
      break;
    case AppState::STATUS: drawStatus(); break;
    case AppState::USB_INFO: drawUsbInfo(); break;
  }
}

// -----------------------------------------------------------------------------
// Touch / display power
// -----------------------------------------------------------------------------
static bool readTouch(uint16_t& x, uint16_t& y) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)5) != 5) return false;
  uint8_t b[5];
  for (int i = 0; i < 5; ++i) b[i] = Wire.read();
  if ((b[0] & 0x0F) == 0) return false;
  x = ((uint16_t)(b[1] & 0x0F) << 8) | b[2];
  y = ((uint16_t)(b[3] & 0x0F) << 8) | b[4];
  if (x >= SCREEN_W) x = SCREEN_W - 1;
  if (y >= SCREEN_H) y = SCREEN_H - 1;

  // The production enclosure uses the AMOLED rotated 180 degrees. The FT3168
  // still reports native panel coordinates, so rotate touch by the same amount
  // or visible buttons would react at their former positions.
  x = (SCREEN_W - 1) - x;
  y = (SCREEN_H - 1) - y;
  return true;
}

static void initTouch() {
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(300000);
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.endTransmission();
}

static void noteUserActivity() {
  lastUserActivityMs = millis();
  if (displayFullyOff) {
    panel->displayOn();
    displayFullyOff = false;
  }
  if (displayDimmed) displayDimmed = false;
  panel->setBrightness(DISPLAY_BRIGHTNESS_ACTIVE);
}

static bool wakeDisplayOnlyIfOff() {
  if (!displayFullyOff) return false;
  panel->displayOn();
  panel->setBrightness(DISPLAY_BRIGHTNESS_ACTIVE);
  displayFullyOff = false;
  displayDimmed = false;
  lastUserActivityMs = millis();
  screenDirty = true;
  render(true);
  return true;
}

static void serviceDisplayPower() {
  const uint32_t idle = millis() - lastUserActivityMs;
  if (!displayDimmed && idle >= DISPLAY_IDLE_DIM_MS) {
    panel->setBrightness(DISPLAY_BRIGHTNESS_DIM);
    displayDimmed = true;
  }
  if (!displayFullyOff && idle >= DISPLAY_IDLE_OFF_MS) {
    panel->displayOff();
    displayFullyOff = true;
  }
}

// -----------------------------------------------------------------------------
// Storage / session metadata
// -----------------------------------------------------------------------------
static bool initSD() {
  sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, 10000000)) return false;
  if (SD.cardType() == CARD_NONE) return false;
  if (!SD.exists("/visitescribe")) SD.mkdir("/visitescribe");
  return true;
}

static String baseName(const char* name) {
  String s(name ? name : "");
  const int slash = s.lastIndexOf('/');
  if (slash >= 0) s = s.substring(slash + 1);
  return s;
}

static void recoverTmpWavs() {
  if (!sdOk) return;
  File dir = SD.open("/visitescribe");
  if (!dir) return;
  std::vector<String> tmpPaths;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      String base = baseName(f.name());
      if (base.endsWith(".wav.tmp")) tmpPaths.push_back(String("/visitescribe/") + base);
    }
    f.close();
  }
  dir.close();

  for (const auto& tmp : tmpPaths) {
    File f = SD.open(tmp, FILE_READ);
    const size_t physical = f ? f.size() : 0;
    if (f) f.close();
    if (physical <= sizeof(WAVHeader)) continue;

    // Open in-place without truncating the recovered PCM payload.
    f = SD.open(tmp, "r+");
    if (!f) continue;
    writeWavHeader(f, static_cast<uint32_t>(physical - sizeof(WAVHeader)));
    f.close();

    String recovered = tmp.substring(0, tmp.length() - 4);
    if (SD.exists(recovered)) recovered += ".recovered.wav";
    if (SD.rename(tmp, recovered)) {
      Serial.printf("RECOVERY: finalized %s -> %s\n", tmp.c_str(), recovered.c_str());
    }
  }
}

static uint16_t findNextSessionId() {
  for (uint16_t i = 1; i < 65000; ++i) {
    char p[64];
    snprintf(p, sizeof(p), "/visitescribe/s%05u_events.csv", i);
    if (!SD.exists(p)) return i;
  }
  return 1;
}

static String syncPath(const String& prefix) {
  return String("/visitescribe/") + prefix + "_sync.txt";
}

static String randomUuid() {
  uint8_t b[16];
  esp_fill_random(b, sizeof(b));
  b[6] = (b[6] & 0x0F) | 0x40;
  b[8] = (b[8] & 0x3F) | 0x80;
  char out[37];
  snprintf(out, sizeof(out),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
           b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
  return String(out);
}

static bool readSyncMeta(const String& prefix, String& uuid, String& syncState) {
  const String path = syncPath(prefix);
  if (!SD.exists(path)) return false;
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("uuid=")) uuid = line.substring(5);
    else if (line.startsWith("state=")) syncState = line.substring(6);
  }
  f.close();
  return uuid.length() == 36;
}

static bool writeSyncMeta(const String& prefix, const String& uuid, const char* syncState) {
  const String path = syncPath(prefix);
  if (SD.exists(path)) SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  f.printf("uuid=%s\nstate=%s\n", uuid.c_str(), syncState);
  f.flush();
  f.close();
  return true;
}

static bool ensureSessionUuid(const String& prefix, String& uuid, String& syncState) {
  if (readSyncMeta(prefix, uuid, syncState)) return true;
  uuid = randomUuid();
  syncState = "queued";
  return writeSyncMeta(prefix, uuid, syncState.c_str());
}

static bool eventsShowComplete(const String& path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  bool complete = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.indexOf(",session_stopped,") >= 0) {
      complete = true;
      break;
    }
  }
  f.close();
  return complete;
}

static std::vector<String> collectWavs(const String& prefix) {
  std::vector<String> wavs;
  File dir = SD.open("/visitescribe");
  if (!dir) return wavs;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      String base = baseName(f.name());
      if (base.startsWith(prefix + "_") && base.endsWith(".wav")) {
        wavs.push_back(String("/visitescribe/") + base);
      }
    }
    f.close();
  }
  dir.close();
  std::sort(wavs.begin(), wavs.end(),
            [](const String& a, const String& b) { return a.compareTo(b) < 0; });
  return wavs;
}

static String modeFromWavs(const std::vector<String>& wavs) {
  for (const auto& w : wavs) {
    if (w.indexOf("_round_") >= 0) return "multi_patient";
    if (w.indexOf("_meeting") >= 0) return "meeting";
  }
  return "single_patient";
}

static std::vector<String> pendingPrefixes() {
  std::vector<String> prefixes;
  if (!sdOk) return prefixes;
  File dir = SD.open("/visitescribe");
  if (!dir) return prefixes;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      String base = baseName(f.name());
      if (base.startsWith("s") && base.endsWith("_events.csv") && base.length() >= 6) {
        const String prefix = base.substring(0, 6);
        const String ep = String("/visitescribe/") + base;
        String uuid, st;
        const bool hasMeta = readSyncMeta(prefix, uuid, st);
        if ((!hasMeta || st != "ingested") && eventsShowComplete(ep) &&
            !collectWavs(prefix).empty()) {
          prefixes.push_back(prefix);
        }
      }
    }
    f.close();
  }
  dir.close();
  std::sort(prefixes.begin(), prefixes.end(),
            [](const String& a, const String& b) { return a.compareTo(b) < 0; });
  prefixes.erase(std::unique(prefixes.begin(), prefixes.end(),
                             [](const String& a, const String& b) { return a == b; }),
                 prefixes.end());
  return prefixes;
}

static uint16_t pendingSessionCount() {
  return static_cast<uint16_t>(pendingPrefixes().size());
}

struct LocalSession {
  String prefix;
  String eventsPath;
  String uuid;
  String mode;
  std::vector<String> wavs;
};

static bool loadLocalSession(const String& prefix, LocalSession& out) {
  out.prefix = prefix;
  out.eventsPath = String("/visitescribe/") + prefix + "_events.csv";
  out.wavs = collectWavs(prefix);
  if (out.wavs.empty()) return false;
  out.mode = modeFromWavs(out.wavs);
  String st;
  if (!ensureSessionUuid(prefix, out.uuid, st)) return false;
  return st != "ingested";
}

// -----------------------------------------------------------------------------
// PDM audio
// -----------------------------------------------------------------------------
static bool setPdmCapture(bool enable) {
  if (!pdmReady || !pdmRx) return false;
  if (enable == pdmChannelEnabled) {
    captureEnabled = enable;
    return true;
  }

  if (enable) {
    const esp_err_t err = i2s_channel_enable(pdmRx);
    if (err != ESP_OK) {
      Serial.printf("AUDIO: i2s enable failed: %s\n", esp_err_to_name(err));
      audioError = true;
      return false;
    }
    pdmChannelEnabled = true;
    captureEnabled = true;
    return true;
  }

  captureEnabled = false;
  const esp_err_t err = i2s_channel_disable(pdmRx);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("AUDIO: i2s disable failed: %s\n", esp_err_to_name(err));
    audioError = true;
    return false;
  }
  pdmChannelEnabled = false;
  delay(5);
  return true;
}

static bool initPdm() {
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  esp_err_t err = i2s_new_channel(&chanCfg, nullptr, &pdmRx);
  if (err != ESP_OK) {
    Serial.printf("AUDIO: i2s_new_channel failed: %s\n", esp_err_to_name(err));
    return false;
  }

  i2s_pdm_rx_clk_config_t clkCfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(AUDIO_RATE);
  clkCfg.dn_sample_mode = I2S_PDM_DSR_8S;

  i2s_pdm_rx_slot_config_t slotCfg =
      I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);

  i2s_pdm_rx_config_t pdmCfg = {};
  pdmCfg.clk_cfg = clkCfg;
  pdmCfg.slot_cfg = slotCfg;
  pdmCfg.gpio_cfg.clk = static_cast<gpio_num_t>(PDM_CLK_GPIO);
  pdmCfg.gpio_cfg.dins[0] = static_cast<gpio_num_t>(PDM_DATA_GPIO);
  pdmCfg.gpio_cfg.invert_flags.clk_inv = false;

  err = i2s_channel_init_pdm_rx_mode(pdmRx, &pdmCfg);
  if (err != ESP_OK) {
    Serial.printf("AUDIO: PDM init failed: %s\n", esp_err_to_name(err));
    i2s_del_channel(pdmRx);
    pdmRx = nullptr;
    return false;
  }

  Serial.printf("AUDIO: dual PDM ready GPIO%d/%d 48k stereo; idle disabled\n",
                PDM_CLK_GPIO, PDM_DATA_GPIO);
  return true;
}

static void audioTask(void*) {
  alignas(4) static int16_t pcm[2048]; // ~21 ms stereo PCM
  for (;;) {
    if (!captureEnabled || !pdmChannelEnabled) {
      vTaskDelay(pdMS_TO_TICKS(4));
      continue;
    }

    size_t bytesRead = 0;
    const esp_err_t err =
        i2s_channel_read(pdmRx, pcm, sizeof(pcm), &bytesRead, pdMS_TO_TICKS(200));
    if (err != ESP_OK || bytesRead < 4 || !captureEnabled) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    size_t sampleCount = bytesRead / sizeof(int16_t);
    sampleCount &= ~static_cast<size_t>(1);

    // ESP-IDF PDM stereo returns RIGHT first. Store conventional LEFT,RIGHT.
    for (size_t i = 0; i < sampleCount; i += 2) {
      const int16_t right = pcm[i];
      const int16_t left = pcm[i + 1];
      pcm[i] = left;
      pcm[i + 1] = right;
    }

    if (xSemaphoreTake(wavMutex, portMAX_DELAY) == pdTRUE) {
      if (captureEnabled && segmentOpen && wavFile) {
        const size_t bytes = sampleCount * sizeof(int16_t);
        const size_t written = wavFile.write(reinterpret_cast<uint8_t*>(pcm), bytes);
        wavDataBytes += static_cast<uint32_t>(written);
        if (written != bytes) {
          audioError = true;
          captureEnabled = false;
        }
      }
      xSemaphoreGive(wavMutex);
    }
  }
}

static void makeAudioPaths() {
  const char* tag = selectedMode == Mode::VISIT ? "visit" :
                    selectedMode == Mode::ROUND ? "round" : "meeting";
  if (selectedMode == Mode::ROUND) {
    snprintf(wavFinalPath, sizeof(wavFinalPath),
             "/visitescribe/s%05u_%s_p%03u_s%02u.wav",
             sessionId, tag, patientNumber, segmentNumber);
  } else if (segmentNumber <= 1) {
    snprintf(wavFinalPath, sizeof(wavFinalPath),
             "/visitescribe/s%05u_%s.wav", sessionId, tag);
  } else {
    snprintf(wavFinalPath, sizeof(wavFinalPath),
             "/visitescribe/s%05u_%s_add%02u.wav",
             sessionId, tag, segmentNumber - 1);
  }
  snprintf(wavTmpPath, sizeof(wavTmpPath), "%s.tmp", wavFinalPath);
}

static bool openSegment() {
  makeAudioPaths();
  if (SD.exists(wavTmpPath)) SD.remove(wavTmpPath);

  if (xSemaphoreTake(wavMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
  wavFile = SD.open(wavTmpPath, FILE_WRITE);
  if (!wavFile) {
    xSemaphoreGive(wavMutex);
    return false;
  }
  WAVHeader h;
  const bool ok =
      wavFile.write(reinterpret_cast<const uint8_t*>(&h), sizeof(h)) == sizeof(h);
  wavDataBytes = 0;
  segmentOpen = ok;
  xSemaphoreGive(wavMutex);
  if (!ok) return false;

  if (!setPdmCapture(true)) return false;
  return true;
}

static void closeSegment() {
  setPdmCapture(false);
  if (xSemaphoreTake(wavMutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
    audioError = true;
    return;
  }

  if (segmentOpen && wavFile) {
    writeWavHeader(wavFile, wavDataBytes);
    wavFile.close();
    segmentOpen = false;
    if (SD.exists(wavFinalPath)) SD.remove(wavFinalPath);
    if (!SD.rename(wavTmpPath, wavFinalPath)) {
      audioError = true;
      Serial.printf("AUDIO: rename failed; retained %s\n", wavTmpPath);
    }
  }
  xSemaphoreGive(wavMutex);
}

static void logEvent(const char* eventName, uint32_t offsetMs) {
  if (!sdOk || !eventsPath[0]) return;
  File f = SD.open(eventsPath, FILE_APPEND);
  if (!f) return;
  f.printf("%lu,%s,%u,%u,%u,%s\n",
           (unsigned long)offsetMs, eventName,
           patientNumber, segmentNumber, markerCount, wavFinalPath);
  f.flush();
  f.close();
}

// -----------------------------------------------------------------------------
// Recorder state
// -----------------------------------------------------------------------------
static bool startNewSession(Mode mode) {
  if (!sdOk || !pdmReady) return false;
  selectedMode = mode;
  sessionId = findNextSessionId();
  patientNumber = 1;
  segmentNumber = 1;
  markerCount = 0;
  totalPausedMs = 0;
  pauseStartedMs = 0;
  sessionStartedMs = millis();
  sessionOpen = true;
  audioError = false;

  snprintf(eventsPath, sizeof(eventsPath),
           "/visitescribe/s%05u_events.csv", sessionId);
  if (SD.exists(eventsPath)) SD.remove(eventsPath);
  File events = SD.open(eventsPath, FILE_WRITE);
  if (!events) {
    sessionOpen = false;
    return false;
  }
  events.println("elapsed_ms,event,patient,segment,markers,audio_file");
  events.close();

  char prefix[16];
  snprintf(prefix, sizeof(prefix), "s%05u", sessionId);
  String uuid, st;
  if (!ensureSessionUuid(prefix, uuid, st)) {
    sessionOpen = false;
    return false;
  }

  if (!openSegment()) {
    sessionOpen = false;
    return false;
  }
  logEvent("session_started", 0);
  state = AppState::RECORDING;
  screenDirty = true;
  return true;
}

static void togglePrivacy() {
  if (state == AppState::RECORDING) {
    const uint32_t off = activeElapsedMs();
    if (!setPdmCapture(false)) audioError = true;
    pauseStartedMs = millis();
    state = AppState::PAUSED;
    logEvent("privacy_pause_started", off);
  } else if (state == AppState::PAUSED) {
    const uint32_t off = activeElapsedMs();
    totalPausedMs += millis() - pauseStartedMs;
    pauseStartedMs = 0;
    if (!setPdmCapture(true)) audioError = true;
    state = AppState::RECORDING;
    logEvent("privacy_pause_ended", off);
  }
  screenDirty = true;
}

static void addMarkerOrNext() {
  const uint32_t off = activeElapsedMs();
  if (selectedMode == Mode::ROUND) {
    setPdmCapture(false);
    logEvent("patient_boundary", off);
    closeSegment();
    ++patientNumber;
    segmentNumber = 1;
    if (!openSegment()) audioError = true;
    state = AppState::RECORDING;
    logEvent("patient_started", off);
  } else {
    ++markerCount;
    logEvent("marker", off);
  }
  screenDirty = true;
}

static void stopSession() {
  if (!sessionOpen) return;
  const uint32_t off = activeElapsedMs();
  const bool wasPaused = state == AppState::PAUSED;
  setPdmCapture(false);
  logEvent("session_stopped", off);
  closeSegment();
  if (wasPaused) {
    totalPausedMs += millis() - pauseStartedMs;
    pauseStartedMs = 0;
  }
  state = AppState::FINISHED;
  finishedAtMs = millis();
  screenDirty = true;
}

static void resumeSession() {
  if (!sessionOpen || state != AppState::FINISHED) return;
  totalPausedMs += millis() - finishedAtMs;
  finishedAtMs = 0;
  ++segmentNumber;
  if (!openSegment()) audioError = true;
  state = AppState::RECORDING;
  logEvent("session_resumed", activeElapsedMs());
  screenDirty = true;
}

static void goHome() {
  setPdmCapture(false);
  sessionOpen = false;
  eventsPath[0] = wavTmpPath[0] = wavFinalPath[0] = 0;
  sessionStartedMs = pauseStartedMs = totalPausedMs = finishedAtMs = 0;
  patientNumber = segmentNumber = 1;
  markerCount = 0;
  state = AppState::HOME;
  screenDirty = true;
  lastDisplayedSecond = UINT32_MAX;
}

// -----------------------------------------------------------------------------
// USB sync v1 (same protocol as CoreS3-Lite)
// -----------------------------------------------------------------------------
static bool usbSyncActive = false;
static String usbRxLine;
static uint8_t* usbScratch = nullptr;
static constexpr size_t USB_SCRATCH_BYTES = 64U * 1024U;
static uint8_t deviceRootKey[32] = {0};
static bool deviceRootKeyReady = false;

static bool usbPlugStateKnown = false;
static bool usbWasPlugged = false;
static bool usbRecoveryPending = false;
static uint32_t usbLastRecoveryMs = 0;

static bool recorderBusyForUsb() {
  return captureEnabled ||
         state == AppState::RECORDING ||
         state == AppState::PAUSED ||
         state == AppState::FINISHED;
}

static bool ensureUsbScratch() {
  if (usbScratch) return true;
  usbScratch = static_cast<uint8_t*>(
      heap_caps_malloc(USB_SCRATCH_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  return usbScratch != nullptr;
}

static bool ensureDeviceRootKey() {
  if (deviceRootKeyReady) return true;
  Preferences prefs;
  if (!prefs.begin("visitescribe", false)) return false;
  const size_t n = prefs.getBytesLength("rootkey");
  if (n == sizeof(deviceRootKey)) {
    prefs.getBytes("rootkey", deviceRootKey, sizeof(deviceRootKey));
  } else {
    esp_fill_random(deviceRootKey, sizeof(deviceRootKey));
    if (prefs.putBytes("rootkey", deviceRootKey, sizeof(deviceRootKey)) !=
        sizeof(deviceRootKey)) {
      prefs.end();
      memset(deviceRootKey, 0, sizeof(deviceRootKey));
      return false;
    }
  }
  prefs.end();
  deviceRootKeyReady = true;
  return true;
}

static bool hmacSha256(const uint8_t* key, size_t keyLen,
                       const String& text, uint8_t out[32]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  return mbedtls_md_hmac(
      info, key, keyLen,
      reinterpret_cast<const unsigned char*>(text.c_str()), text.length(), out) == 0;
}

static bool sessionKey(const String& uuid, uint8_t out[32]) {
  if (!ensureDeviceRootKey()) return false;
  return hmacSha256(deviceRootKey, sizeof(deviceRootKey),
                    String("session:") + uuid, out);
}

static String base64Bytes(const uint8_t* data, size_t len) {
  const size_t cap = 4 * ((len + 2) / 3) + 1;
  std::vector<unsigned char> out(cap);
  size_t written = 0;
  if (mbedtls_base64_encode(out.data(), out.size(), &written, data, len) != 0)
    return String();
  out[written] = 0;
  return String(reinterpret_cast<char*>(out.data()));
}

static bool usbSafePath(const String& path) {
  return path.startsWith("/visitescribe/") &&
         path.indexOf("..") < 0 &&
         path.length() < 180;
}

static void drawUsbConnected() {
  if (displayFullyOff) {
    panel->displayOn();
    displayFullyOff = false;
  }
  panel->setBrightness(DISPLAY_BRIGHTNESS_ACTIVE);
  displayDimmed = false;
  drawTopBar("USB SYNC", "PC verbonden");
  gfx->fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_BG);
  centeredBold(142, "AUTOMATISCHE SYNC", C_BLUE, 2);
  centeredBold(198, "PC verwerkt en uploadt", C_NAVY, 1);
  centeredBold(236, "Master WAV blijft lokaal", C_GREEN, 1);
}

static void usbHotplugService() {
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
  const bool plugged = Serial.isPlugged();

  if (!usbPlugStateKnown) {
    usbPlugStateKnown = true;
    usbWasPlugged = plugged;

    // A recorder that boots/flashes while USB is already attached never sees a
    // false->true hot-plug edge. On ESP32-S3 HW CDC that can leave a visible
    // COM port whose RX/TX path is stale. Schedule the same one-shot CDC-only
    // recovery used for a later physical hot-plug.
    usbRecoveryPending = plugged;
    return;
  }
  if (!plugged) {
    usbWasPlugged = false;
    usbRecoveryPending = false;
    return;
  }
  if (!usbWasPlugged) {
    usbWasPlugged = true;
    usbRecoveryPending = true;
  }
  if (!usbRecoveryPending || usbSyncActive || recorderBusyForUsb() ||
      millis() - usbLastRecoveryMs < 1500) {
    return;
  }

  usbRecoveryPending = false;
  usbLastRecoveryMs = millis();
  Serial.end();
  delay(80);
  Serial.begin(115200);
  delay(250);
#endif
}

static void usbReplyList() {
  const auto prefixes = pendingPrefixes();
  uint32_t listed = 0;
  for (const auto& prefix : prefixes) {
    LocalSession local;
    if (!loadLocalSession(prefix, local)) continue;

    File events = SD.open(local.eventsPath, FILE_READ);
    const size_t eventSize = events ? events.size() : 0;
    if (events) events.close();

    Serial.printf("VSUSB SESSION %s %s %s %u %u\n",
                  local.prefix.c_str(), local.uuid.c_str(), local.mode.c_str(),
                  (unsigned)local.wavs.size(), (unsigned)eventSize);
    Serial.printf("VSUSB EVENTS %u %s\n",
                  (unsigned)eventSize, local.eventsPath.c_str());
    for (const auto& path : local.wavs) {
      File wav = SD.open(path, FILE_READ);
      const size_t bytes = wav ? wav.size() : 0;
      if (wav) wav.close();
      Serial.printf("VSUSB WAV %u %s\n", (unsigned)bytes, path.c_str());
    }
    Serial.println("VSUSB ENDSESSION");
    ++listed;
  }
  Serial.printf("VSUSB ENDLIST %lu\n", (unsigned long)listed);
}

static void usbReplySessionKey(const String& uuid) {
  uint8_t key[32];
  if (!sessionKey(uuid, key)) {
    Serial.println("VSUSB ERROR KEY");
    return;
  }
  const String b64 = base64Bytes(key, sizeof(key));
  memset(key, 0, sizeof(key));
  Serial.printf("VSUSB KEY %s %s\n", uuid.c_str(), b64.c_str());
}

static void usbReadFile(const String& path, uint32_t offset, uint32_t wanted) {
  static constexpr uint32_t MAX_READ = 256U * 1024U;
  if (!usbSafePath(path) || wanted == 0 || wanted > MAX_READ) {
    Serial.println("VSUSB ERROR READ_ARGS");
    return;
  }
  if (!ensureUsbScratch()) {
    Serial.println("VSUSB ERROR READ_BUFFER");
    return;
  }

  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.println("VSUSB ERROR READ_OPEN");
    return;
  }
  const uint32_t fileSize = static_cast<uint32_t>(f.size());
  if (offset > fileSize || !f.seek(offset)) {
    f.close();
    Serial.println("VSUSB ERROR READ_SEEK");
    return;
  }

  uint32_t sendBytes = wanted;
  if (sendBytes > fileSize - offset) sendBytes = fileSize - offset;
  Serial.printf("VSUSB DATA %lu\n", (unsigned long)sendBytes);
  Serial.flush();

  uint32_t sent = 0;
  while (sent < sendBytes) {
    size_t request = sendBytes - sent;
    if (request > USB_SCRATCH_BYTES) request = USB_SCRATCH_BYTES;
    const size_t got = f.read(usbScratch, request);
    if (!got) break;

    size_t written = 0;
    while (written < got) {
      const size_t n = Serial.write(usbScratch + written, got - written);
      if (!n) {
        delay(1);
        continue;
      }
      written += n;
    }
    sent += got;
  }
  f.close();
  Serial.flush();
  Serial.printf("\nVSUSB ENDDATA %lu\n", (unsigned long)sent);
  Serial.flush();
}

static void usbHandleCommand(String line) {
  line.trim();
  if (!line.startsWith("VSUSB ")) return;

  if (line == "VSUSB HELLO") {
    Serial.println("VSUSB READY 1");
    return;
  }

  if (line == "VSUSB ENTER") {
    if (recorderBusyForUsb()) {
      Serial.println("VSUSB BUSY RECORDING");
      return;
    }
    usbSyncActive = true;
    drawUsbConnected();
    const String tokenB64 = base64Bytes(
        reinterpret_cast<const uint8_t*>(VISITESCRIBE_DEVICE_TOKEN),
        strlen(VISITESCRIBE_DEVICE_TOKEN));
    Serial.printf("VSUSB OK ENTER %s %s %s\n",
                  VISITESCRIBE_DEVICE_ID,
                  VISITESCRIBE_SERVER_BASE_URL,
                  tokenB64.c_str());
    Serial.flush();
    return;
  }

  if (!usbSyncActive) {
    Serial.println("VSUSB ERROR NOT_ENTERED");
    return;
  }

  if (line == "VSUSB INFO") {
    const String tokenB64 = base64Bytes(
        reinterpret_cast<const uint8_t*>(VISITESCRIBE_DEVICE_TOKEN),
        strlen(VISITESCRIBE_DEVICE_TOKEN));
    Serial.printf("VSUSB INFO %s %s %s\n",
                  VISITESCRIBE_DEVICE_ID,
                  VISITESCRIBE_SERVER_BASE_URL,
                  tokenB64.c_str());
    return;
  }

  if (line == "VSUSB LIST") {
    usbReplyList();
    return;
  }

  if (line == "VSUSB EXIT") {
    Serial.println("VSUSB OK EXIT");
    Serial.flush();
    usbSyncActive = false;
    goHome();
    render(true);
    return;
  }

  if (line.startsWith("VSUSB KEY ")) {
    String uuid = line.substring(strlen("VSUSB KEY "));
    uuid.trim();
    if (uuid.length() != 36) {
      Serial.println("VSUSB ERROR KEY_ARGS");
      return;
    }
    usbReplySessionKey(uuid);
    return;
  }

  if (line.startsWith("VSUSB MARK ")) {
    String rest = line.substring(strlen("VSUSB MARK "));
    const int sep = rest.indexOf(' ');
    if (sep <= 0) {
      Serial.println("VSUSB ERROR MARK_ARGS");
      return;
    }
    const String prefix = rest.substring(0, sep);
    String uuid = rest.substring(sep + 1);
    uuid.trim();
    if (prefix.length() != 6 || uuid.length() != 36 ||
        !writeSyncMeta(prefix, uuid, "ingested")) {
      Serial.println("VSUSB ERROR MARK");
      return;
    }
    Serial.printf("VSUSB OK MARK %s\n", prefix.c_str());
    return;
  }

  if (line.startsWith("VSUSB READ ")) {
    String rest = line.substring(strlen("VSUSB READ "));
    const int s1 = rest.indexOf(' ');
    const int s2 = s1 >= 0 ? rest.indexOf(' ', s1 + 1) : -1;
    if (s1 <= 0 || s2 <= s1) {
      Serial.println("VSUSB ERROR READ_ARGS");
      return;
    }
    const String path = rest.substring(0, s1);
    const uint32_t offset =
        static_cast<uint32_t>(strtoul(rest.substring(s1 + 1, s2).c_str(), nullptr, 10));
    const uint32_t len =
        static_cast<uint32_t>(strtoul(rest.substring(s2 + 1).c_str(), nullptr, 10));
    usbReadFile(path, offset, len);
    return;
  }

  Serial.println("VSUSB ERROR UNKNOWN");
}

static bool usbService() {
  usbHotplugService();

  while (Serial.available()) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') continue;
    if (ch == '\n') {
      if (usbRxLine.length()) {
        const String line = usbRxLine;
        usbRxLine = "";
        usbHandleCommand(line);
      }
    } else if (usbRxLine.length() < 255) {
      usbRxLine += ch;
    } else {
      usbRxLine = "";
    }
  }
  return usbSyncActive;
}

// -----------------------------------------------------------------------------
// Input
// -----------------------------------------------------------------------------
static void handleTouchPress(uint16_t x, uint16_t y) {
  switch (state) {
    case AppState::HOME:
      if (HOME_VISIT.contains(x,y)) {
        selectedMode = Mode::VISIT; state = AppState::MODE_CONFIRM;
      } else if (HOME_ROUND.contains(x,y)) {
        selectedMode = Mode::ROUND; state = AppState::MODE_CONFIRM;
      } else if (HOME_MEETING.contains(x,y)) {
        selectedMode = Mode::MEETING; state = AppState::MODE_CONFIRM;
      } else if (HOME_MENU.contains(x,y)) {
        state = AppState::MENU;
      }
      break;

    case AppState::MODE_CONFIRM:
      if (TWO_TOP.contains(x,y)) {
        if (!startNewSession(selectedMode)) {
          audioError = true;
          goHome();
        }
      } else if (TWO_BOTTOM.contains(x,y)) {
        goHome();
      }
      break;

    case AppState::RECORDING:
    case AppState::PAUSED:
      if (THREE_TOP.contains(x,y)) togglePrivacy();
      else if (THREE_MIDDLE.contains(x,y)) addMarkerOrNext();
      else if (THREE_BOTTOM.contains(x,y)) stopSession();
      break;

    case AppState::FINISHED:
      if (TWO_TOP.contains(x,y)) resumeSession();
      else if (TWO_BOTTOM.contains(x,y)) goHome();
      break;

    case AppState::MENU:
      if (THREE_TOP.contains(x,y)) state = AppState::STATUS;
      else if (THREE_MIDDLE.contains(x,y)) state = AppState::USB_INFO;
      else if (THREE_BOTTOM.contains(x,y)) goHome();
      break;

    case AppState::STATUS:
      if (STATUS_BACK.contains(x,y)) state = AppState::MENU;
      break;

    case AppState::USB_INFO:
      if (USB_BACK.contains(x,y)) state = AppState::MENU;
      break;
  }

  screenDirty = true;
}

static void pollBootButton() {
  const bool down = digitalRead(BOOT_GPIO) == LOW;
  const uint32_t now = millis();
  if (down && !bootWasDown) bootDownSince = now;

  if (down && bootWasDown && bootDownSince && now - bootDownSince > 1200) {
    if (state == AppState::RECORDING || state == AppState::PAUSED) stopSession();
    bootDownSince = 0;
  }
  if (!down) bootDownSince = 0;
  bootWasDown = down;
}

// -----------------------------------------------------------------------------
// Setup / loop
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("VisiteScribe MINI production v0.1");

  pinMode(BOOT_GPIO, INPUT_PULLUP);
  wavMutex = xSemaphoreCreateMutex();

  if (!gfx->begin()) Serial.println("DISPLAY: init failed");
  gfx->setRotation(2);  // enclosure orientation: 180 degrees
  gfx->fillScreen(C_BG);
  panel->setBrightness(DISPLAY_BRIGHTNESS_ACTIVE);

  initTouch();
  updateBattery(true);

  sdOk = initSD();
  if (sdOk) recoverTmpWavs();
  Serial.printf("STORAGE: microSD %s\n", sdOk ? "OK" : "NOT FOUND");

  pdmReady = initPdm();
  if (pdmReady) {
    const BaseType_t ok = xTaskCreatePinnedToCore(
        audioTask, "pdm-audio", 8192, nullptr, 5, &audioTaskHandle, 0);
    if (ok != pdPASS) {
      pdmReady = false;
      Serial.println("AUDIO: task create failed");
    }
  }

  ensureUsbScratch();
  lastUserActivityMs = millis();
  render(true);

  Serial.printf("DEVICE: id=%s USB=v1 PDM=%s SD=%s\n",
                VISITESCRIBE_DEVICE_ID,
                pdmReady ? "2/2" : "FAIL",
                sdOk ? "OK" : "FAIL");
}

void loop() {
  if (usbService()) {
    delay(1);
    return;
  }

  uint16_t x = 0, y = 0;
  const bool touchDown = readTouch(x, y);
  if (touchDown && !touchWasDown) {
    if (!wakeDisplayOnlyIfOff()) {
      noteUserActivity();
      handleTouchPress(x, y);
    }
  } else if (touchDown && !displayFullyOff) {
    noteUserActivity();
  }
  touchWasDown = touchDown;

  const bool bootDown = digitalRead(BOOT_GPIO) == LOW;
  if (bootDown && displayFullyOff) wakeDisplayOnlyIfOff();
  pollBootButton();

  if (updateBattery(false) && batteryPercent != lastBatteryUiPercent &&
      !displayFullyOff) {
    if (state == AppState::STATUS) screenDirty = true;
    else drawBatteryBadge();
  }

  if (state == AppState::FINISHED &&
      millis() - finishedAtMs >= FINISHED_AUTO_HOME_MS) {
    goHome();
  }

  if (!displayFullyOff) render();
  serviceDisplayPower();
  delay(8);
}
