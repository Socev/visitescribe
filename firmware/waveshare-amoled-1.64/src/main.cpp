#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#define VISITESCRIBE_WIFI_CONFIGURED 1
#else
#define VISITESCRIBE_WIFI_CONFIGURED 0
#define VISITESCRIBE_WIFI_SSID_1 ""
#define VISITESCRIBE_WIFI_PASSWORD_1 ""
#define VISITESCRIBE_WIFI_SSID_2 ""
#define VISITESCRIBE_WIFI_PASSWORD_2 ""
#endif

#ifndef VISITESCRIBE_BOARD_REV
#define VISITESCRIBE_BOARD_REV 2
#endif

// -----------------------------------------------------------------------------
// Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.64
// -----------------------------------------------------------------------------
#if VISITESCRIBE_BOARD_REV == 2
static constexpr int LCD_CS = 46;
#else
static constexpr int LCD_CS = 9;
#endif

static constexpr int LCD_SCLK = 10;
static constexpr int LCD_D0 = 11;
static constexpr int LCD_D1 = 12;
static constexpr int LCD_D2 = 13;
static constexpr int LCD_D3 = 14;
static constexpr int LCD_RST = 21;
static constexpr int SCREEN_W = 280;
static constexpr int SCREEN_H = 456;
static constexpr int HEADER_H = 64;

static constexpr int TOUCH_SDA = 47;
static constexpr int TOUCH_SCL = 48;
static constexpr uint8_t TOUCH_ADDR = 0x38;

static constexpr int SD_MISO = 40;
static constexpr int SD_MOSI = 39;
static constexpr int SD_SCLK = 41;
static constexpr int SD_CS = 38;

static constexpr int BAT_ADC_GPIO = 4;  // BAT_ADC via onboard 200k/100k divider
static constexpr float BAT_DIVIDER = 3.0f;
static constexpr int VISITESCRIBE_BOOT_GPIO = 0;

// -----------------------------------------------------------------------------
// OurMind-inspired palette. Large solid colour zones are intentional: this UI
// is designed for daily use with one-handed, imprecise taps rather than tiny UI.
// -----------------------------------------------------------------------------
static constexpr uint16_t C_BG       = 0xDEDF;
static constexpr uint16_t C_BLUE     = 0x225D;
static constexpr uint16_t C_BLUE2    = 0x3B7F;
static constexpr uint16_t C_VIOLET   = 0x633D;
static constexpr uint16_t C_TEAL     = 0x2575;
static constexpr uint16_t C_NAVY     = 0x1084;
static constexpr uint16_t C_WHITE    = 0xFFFF;
static constexpr uint16_t C_SOFT     = 0xEF5F;
static constexpr uint16_t C_LINE     = 0xADBF;
static constexpr uint16_t C_GREEN    = 0x35CF;
static constexpr uint16_t C_RED      = 0xEA4B;
static constexpr uint16_t C_AMBER    = 0xFD47;
static constexpr uint16_t C_MUTED    = 0x6B6D;
static constexpr uint16_t C_DARKGREY = 0x39E7;

Arduino_DataBus *displayBus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_GFX *gfx = new Arduino_CO5300(
    displayBus, LCD_RST, 0, SCREEN_W, SCREEN_H, 20, 0, 180, 24);

SPIClass sdSPI(HSPI);

struct Rect {
  int x;
  int y;
  int w;
  int h;
  bool contains(uint16_t px, uint16_t py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

// Four equal main-menu buttons below the 64px status bar.
static const Rect HOME_VISIT   {0,  64, 280, 98};
static const Rect HOME_ROUND   {0, 162, 280, 98};
static const Rect HOME_MEETING {0, 260, 280, 98};
static const Rect HOME_MENU    {0, 358, 280, 98};

// Two huge buttons: 196px each.
static const Rect TWO_TOP      {0,  64, 280, 196};
static const Rect TWO_BOTTOM   {0, 260, 280, 196};

// Three huge buttons: ~1/3 of all usable display space each.
static const Rect THREE_TOP    {0,  64, 280, 131};
static const Rect THREE_MIDDLE {0, 195, 280, 131};
static const Rect THREE_BOTTOM {0, 326, 280, 130};

// Status screen reserves most of the display for information and keeps one
// enormous back button.
static const Rect STATUS_BACK  {0, 348, 280, 108};

// Sync screen keeps a large information field plus two generous actions.
static const Rect SYNC_RETRY   {0, 260, 280, 98};
static const Rect SYNC_BACK    {0, 358, 280, 98};

enum class AppState : uint8_t {
  HOME,
  MODE_CONFIRM,
  RECORDING,
  PAUSED,
  FINISHED,
  MENU,
  STATUS,
  SYNC
};

enum class Mode : uint8_t {
  VISIT,
  ROUND,
  MEETING
};

enum class SyncPhase : uint8_t {
  NOT_STARTED,
  NO_CREDENTIALS,
  CONNECTING_1,
  CONNECTING_2,
  CONNECTED,
  FAILED
};

AppState state = AppState::HOME;
Mode selectedMode = Mode::VISIT;
SyncPhase syncPhase = SyncPhase::NOT_STARTED;

bool sdOk = false;
bool touchWasDown = false;
bool screenDirty = true;
bool bootWasDown = false;
uint32_t bootPressedAtMs = 0;

uint32_t sessionStartedMs = 0;
uint32_t pauseStartedMs = 0;
uint32_t totalPausedMs = 0;
uint32_t finishedAtMs = 0;
uint16_t markerCount = 0;
uint16_t patientNumber = 1;
char currentLogPath[96] = {0};

uint32_t lastDisplayedSecond = 0xFFFFFFFFUL;
uint32_t lastFinishedCountdown = 0xFFFFFFFFUL;
static constexpr uint32_t FINISHED_AUTO_HOME_MS = 8000;

int batteryPercent = -1;
uint16_t batteryMv = 0;
uint32_t lastBatteryReadMs = 0;
int lastBatteryUiPercent = -999;

uint32_t syncAttemptStartedMs = 0;
uint8_t syncNetwork = 0;
static constexpr uint32_t WIFI_ATTEMPT_MS = 6500;

// -----------------------------------------------------------------------------
// Text and drawing helpers
// -----------------------------------------------------------------------------
void textAt(int x, int y, const char *text, uint16_t color, uint8_t size = 1) {
  gfx->setTextColor(color);
  gfx->setTextSize(size);
  gfx->setCursor(x, y);
  gfx->print(text);
}

void textAtBold(int x, int y, const char *text, uint16_t color, uint8_t size = 1) {
  // Heavy enough to remain legible on the small high-density AMOLED.
  textAt(x, y, text, color, size);
  textAt(x + 1, y, text, color, size);
  textAt(x, y + 1, text, color, size);
  if (size >= 2) textAt(x + 1, y + 1, text, color, size);
}

void centeredBold(int y, const char *text, uint16_t color, uint8_t size = 1) {
  int width = (int)strlen(text) * 6 * size;
  int x = (SCREEN_W - width) / 2;
  if (x < 2) x = 2;
  textAtBold(x, y, text, color, size);
}

uint8_t bestLabelSize(const char *label) {
  size_t n = strlen(label);
  if (n <= 6) return 4;
  if (n <= 13) return 3;
  return 2;
}

void centeredInRect(const Rect &r, const char *text, uint16_t color, uint8_t size) {
  int width = (int)strlen(text) * 6 * size;
  int x = r.x + (r.w - width) / 2;
  int y = r.y + (r.h - 8 * size) / 2;
  if (x < 2) x = 2;
  textAtBold(x, y, text, color, size);
}

void bigZone(const Rect &r, const char *label, uint16_t fill, uint16_t textColor,
             const char *subtitle = nullptr) {
  gfx->fillRect(r.x, r.y, r.w, r.h, fill);
  gfx->drawFastHLine(r.x, r.y, r.w, C_WHITE);

  uint8_t size = bestLabelSize(label);
  if (subtitle && subtitle[0]) {
    int width = (int)strlen(label) * 6 * size;
    int x = r.x + (r.w - width) / 2;
    int labelY = r.y + (r.h / 2) - 22;
    if (x < 2) x = 2;
    textAtBold(x, labelY, label, textColor, size);

    int subWidth = (int)strlen(subtitle) * 6;
    int subX = r.x + (r.w - subWidth) / 2;
    if (subX < 2) subX = 2;
    textAtBold(subX, labelY + 40, subtitle, textColor, 1);
  } else {
    centeredInRect(r, label, textColor, size);
  }
}

void drawOurMindMarkMini(int cx, int cy, uint16_t color) {
  gfx->drawRoundRect(cx - 3, cy - 9, 6, 11, 3, color);
  gfx->drawRoundRect(cx - 3, cy - 1, 6, 11, 3, color);
  gfx->drawRoundRect(cx - 9, cy - 3, 11, 6, 3, color);
  gfx->drawRoundRect(cx - 1, cy - 3, 11, 6, 3, color);
  gfx->fillRect(cx - 1, cy - 4, 2, 9, color);
  gfx->fillRect(cx - 4, cy - 1, 9, 2, color);
}

// -----------------------------------------------------------------------------
// Battery
// The board schematic feeds VBAT through a 200k/100k divider to BAT_ADC, so
// the ADC sees one third of battery voltage. Percentage is intentionally an
// approximate LiPo resting-voltage estimate until we calibrate with the real
// 1500/1800mAh packs.
// -----------------------------------------------------------------------------
int batteryPercentFromMv(uint16_t mv) {
  struct Pt { uint16_t mv; uint8_t pct; };
  static const Pt curve[] = {
      {3300, 0}, {3500, 5}, {3600, 12}, {3680, 20}, {3740, 30},
      {3790, 40}, {3830, 50}, {3870, 60}, {3920, 70}, {3980, 80},
      {4070, 90}, {4200, 100}};

  if (mv <= curve[0].mv) return 0;
  if (mv >= curve[11].mv) return 100;
  for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
    if (mv <= curve[i].mv) {
      uint16_t spanMv = curve[i].mv - curve[i - 1].mv;
      int spanPct = curve[i].pct - curve[i - 1].pct;
      return curve[i - 1].pct +
             ((int)(mv - curve[i - 1].mv) * spanPct) / spanMv;
    }
  }
  return 100;
}

bool updateBattery(bool force = false) {
  uint32_t now = millis();
  if (!force && now - lastBatteryReadMs < 5000) return false;
  lastBatteryReadMs = now;

  uint32_t sum = 0;
  for (int i = 0; i < 12; ++i) {
    sum += analogReadMilliVolts(BAT_ADC_GPIO);
    delay(2);
  }
  uint16_t adcMv = (uint16_t)(sum / 12);
  uint16_t measured = (uint16_t)(adcMv * BAT_DIVIDER);

  int oldPct = batteryPercent;
  if (measured < 2800 || measured > 4500) {
    batteryMv = 0;
    batteryPercent = -1;
  } else {
    batteryMv = measured;
    batteryPercent = batteryPercentFromMv(batteryMv);
  }
  return oldPct != batteryPercent;
}

void batteryText(char *out, size_t len) {
  if (batteryPercent < 0) snprintf(out, len, "--%%");
  else snprintf(out, len, "%d%%", batteryPercent);
}

void drawBatteryBadge() {
  char b[12];
  batteryText(b, sizeof(b));
  gfx->fillRect(226, 2, 54, 20, C_WHITE);
  int width = (int)strlen(b) * 6;
  textAtBold(274 - width, 7, b,
             (batteryPercent >= 0 && batteryPercent <= 15) ? C_RED : C_NAVY, 1);
  lastBatteryUiPercent = batteryPercent;
}

void drawTopBar(const char *status, const char *subline = nullptr) {
  gfx->fillRect(0, 0, SCREEN_W, HEADER_H, C_WHITE);
  drawOurMindMarkMini(13, 13, C_BLUE);
  textAtBold(29, 7, "OurMind", C_BLUE, 1);
  drawBatteryBadge();

  uint8_t statusSize = (strlen(status) <= 17) ? 2 : 1;
  centeredBold(27, status, C_NAVY, statusSize);
  if (subline && subline[0]) centeredBold(49, subline, C_MUTED, 1);
  gfx->drawFastHLine(0, HEADER_H - 1, SCREEN_W, C_LINE);
}

// -----------------------------------------------------------------------------
// Session helpers
// -----------------------------------------------------------------------------
const char *modeTitle(Mode mode) {
  switch (mode) {
    case Mode::VISIT: return "VISITE";
    case Mode::ROUND: return "PATIENTRONDE";
    case Mode::MEETING: return "VERGADERING";
  }
  return "?";
}

uint32_t activeElapsedMs() {
  if (sessionStartedMs == 0) return 0;
  uint32_t now = millis();
  uint32_t paused = totalPausedMs;
  if (state == AppState::PAUSED) paused += now - pauseStartedMs;
  return now - sessionStartedMs - paused;
}

void formatElapsed(char *out, size_t len) {
  uint32_t seconds = activeElapsedMs() / 1000;
  snprintf(out, len, "%02lu:%02lu:%02lu",
           (unsigned long)(seconds / 3600),
           (unsigned long)((seconds % 3600) / 60),
           (unsigned long)(seconds % 60));
}

bool initSD() {
  sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, 10000000)) return false;
  if (SD.cardType() == CARD_NONE) return false;
  if (!SD.exists("/visitescribe")) SD.mkdir("/visitescribe");

  File boot = SD.open("/visitescribe/boot.log", FILE_APPEND);
  if (boot) {
    boot.printf("boot_ms=%lu board_rev=%d\n",
                (unsigned long)millis(), VISITESCRIBE_BOARD_REV);
    boot.close();
  }
  return true;
}

uint16_t countDemoSessions() {
  if (!sdOk) return 0;
  uint16_t count = 0;
  File root = SD.open("/visitescribe");
  if (!root) return 0;
  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String name = f.name();
      if (name.endsWith(".csv")) count++;
    }
    f.close();
    f = root.openNextFile();
  }
  root.close();
  return count;
}

void logEventAt(const char *eventName, uint32_t offsetMs) {
  if (!sdOk || currentLogPath[0] == '\0') return;
  File f = SD.open(currentLogPath, FILE_APPEND);
  if (!f) return;
  f.printf("%lu,%s,%u,%u\n",
           (unsigned long)offsetMs, eventName, patientNumber, markerCount);
  f.flush();
  f.close();
}

void goHome() {
  state = AppState::HOME;
  sessionStartedMs = 0;
  pauseStartedMs = 0;
  totalPausedMs = 0;
  finishedAtMs = 0;
  markerCount = 0;
  patientNumber = 1;
  currentLogPath[0] = '\0';
  screenDirty = true;
  lastDisplayedSecond = 0xFFFFFFFFUL;
  lastFinishedCountdown = 0xFFFFFFFFUL;
}

// -----------------------------------------------------------------------------
// Screens
// -----------------------------------------------------------------------------
void drawHome() {
  gfx->fillScreen(C_BG);
  drawTopBar("HOOFDMENU");
  bigZone(HOME_VISIT, "VISITE", C_BLUE, C_WHITE, "1 patient");
  bigZone(HOME_ROUND, "PATIENTRONDE", C_VIOLET, C_WHITE, "meerdere patienten");
  bigZone(HOME_MEETING, "VERGADERING", C_TEAL, C_WHITE, "overleg / bespreking");
  bigZone(HOME_MENU, "MENU", C_NAVY, C_WHITE, "status en sync");
}

void drawModeConfirm() {
  gfx->fillScreen(C_BG);
  drawTopBar(modeTitle(selectedMode));
  bigZone(TWO_TOP, "START", C_GREEN, C_WHITE);
  bigZone(TWO_BOTTOM, "TERUG", C_NAVY, C_WHITE);
}

void recordingHeader(char *status, size_t statusLen) {
  if (selectedMode == Mode::ROUND) {
    snprintf(status, statusLen, "OPNAME PATIENT %u", patientNumber);
  } else if (selectedMode == Mode::VISIT) {
    snprintf(status, statusLen, "OPNAME VISITE");
  } else {
    snprintf(status, statusLen, "OPNAME VERGADERING");
  }
}

void pausedHeader(char *status, size_t statusLen) {
  if (selectedMode == Mode::ROUND) {
    snprintf(status, statusLen, "PRIVACY PAUZE P%u", patientNumber);
  } else {
    snprintf(status, statusLen, "PRIVACY PAUZE");
  }
}

void drawRecording() {
  gfx->fillScreen(C_BG);
  char status[32];
  char elapsed[16];
  recordingHeader(status, sizeof(status));
  formatElapsed(elapsed, sizeof(elapsed));
  drawTopBar(status, elapsed);

  bigZone(THREE_TOP, "PRIVACY", C_AMBER, C_NAVY, "tijdelijk niet opnemen");
  bigZone(THREE_MIDDLE,
          selectedMode == Mode::ROUND ? "VOLGENDE" : "MARKER",
          C_BLUE, C_WHITE,
          selectedMode == Mode::ROUND ? "nieuwe patient" : "markeer dit moment");
  bigZone(THREE_BOTTOM, "STOP", C_RED, C_WHITE, "opname afronden");

  lastDisplayedSecond = activeElapsedMs() / 1000;
}

void drawPaused() {
  gfx->fillScreen(C_BG);
  char status[32];
  char elapsed[16];
  pausedHeader(status, sizeof(status));
  formatElapsed(elapsed, sizeof(elapsed));
  drawTopBar(status, elapsed);

  bigZone(THREE_TOP, "HERVAT", C_GREEN, C_WHITE, "microfoon weer aan");
  bigZone(THREE_MIDDLE,
          selectedMode == Mode::ROUND ? "VOLGENDE" : "MARKER",
          C_BLUE, C_WHITE,
          selectedMode == Mode::ROUND ? "nieuwe patient" : "markeer dit moment");
  bigZone(THREE_BOTTOM, "STOP", C_RED, C_WHITE, "opname afronden");
}

void drawFinished() {
  gfx->fillScreen(C_BG);
  drawTopBar("OPNAME OPGESLAGEN", "AUTO TERUG OVER 8 SEC");
  bigZone(TWO_TOP, "VUL AAN", C_BLUE, C_WHITE, "doorgaan in dezelfde sessie");
  bigZone(TWO_BOTTOM, "KLAAR", C_GREEN, C_WHITE, "terug naar hoofdmenu");
  lastFinishedCountdown = 8;
}

void drawMenu() {
  gfx->fillScreen(C_BG);
  drawTopBar("MENU");
  bigZone(THREE_TOP, "STATUS", C_BLUE, C_WHITE, "accu / mic / opslag");
  bigZone(THREE_MIDDLE, "SYNC", C_TEAL, C_WHITE, "Wi-Fi alleen op verzoek");
  bigZone(THREE_BOTTOM, "TERUG", C_NAVY, C_WHITE, "hoofdmenu");
}

void drawStatusInfo() {
  gfx->fillRect(0, HEADER_H, SCREEN_W, STATUS_BACK.y - HEADER_H, C_BG);

  char line[64];
  if (batteryPercent >= 0) {
    snprintf(line, sizeof(line), "ACCU  %d%%", batteryPercent);
    centeredBold(84, line, batteryPercent <= 15 ? C_RED : C_NAVY, 3);
    snprintf(line, sizeof(line), "%.2f V  (voorlopige schatting)", batteryMv / 1000.0f);
    centeredBold(121, line, C_MUTED, 1);
  } else {
    centeredBold(88, "ACCU --", C_MUTED, 3);
    centeredBold(124, "nog geen LiPo gemeten", C_MUTED, 1);
  }

  centeredBold(163, sdOk ? "MICROSD  OK" : "MICROSD  FOUT",
               sdOk ? C_GREEN : C_RED, 2);

  centeredBold(204, "MICROFOONS  0 / 2", C_AMBER, 2);
  centeredBold(229, "wacht op IM73D122 integratie", C_MUTED, 1);

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(line, sizeof(line), "WIFI  %s", WiFi.SSID().c_str());
    centeredBold(265, line, C_GREEN, 1);
  } else {
    centeredBold(265, "WIFI  UIT", C_NAVY, 2);
  }

  snprintf(line, sizeof(line), "LOKALE SESSIES  %u", countDemoSessions());
  centeredBold(307, line, C_BLUE, 1);
}

void drawStatus() {
  gfx->fillScreen(C_BG);
  drawTopBar("STATUS");
  drawStatusInfo();
  bigZone(STATUS_BACK, "TERUG", C_NAVY, C_WHITE);
}

const char *syncPhaseTitle() {
  switch (syncPhase) {
    case SyncPhase::NO_CREDENTIALS: return "WIFI NIET INGESTELD";
    case SyncPhase::CONNECTING_1:
    case SyncPhase::CONNECTING_2: return "VERBINDEN...";
    case SyncPhase::CONNECTED: return "NETWERK VERBONDEN";
    case SyncPhase::FAILED: return "GEEN VERBINDING";
    default: return "SYNC GEREED";
  }
}

void drawSyncInfo() {
  gfx->fillRect(0, HEADER_H, SCREEN_W, SYNC_RETRY.y - HEADER_H, C_BG);
  centeredBold(92, syncPhaseTitle(),
               syncPhase == SyncPhase::CONNECTED ? C_GREEN :
               syncPhase == SyncPhase::FAILED ? C_RED : C_NAVY,
               syncPhase == SyncPhase::CONNECTING_1 || syncPhase == SyncPhase::CONNECTING_2 ? 2 : 1);

  if (syncPhase == SyncPhase::CONNECTING_1 || syncPhase == SyncPhase::CONNECTING_2) {
    const char *ssid = syncPhase == SyncPhase::CONNECTING_1
                           ? VISITESCRIBE_WIFI_SSID_1
                           : VISITESCRIBE_WIFI_SSID_2;
    centeredBold(132, ssid, C_BLUE, 2);
    centeredBold(175, "Wi-Fi is normaal volledig uit", C_MUTED, 1);
    centeredBold(194, "en wordt alleen voor SYNC aangezet", C_MUTED, 1);
  } else if (syncPhase == SyncPhase::CONNECTED) {
    centeredBold(132, WiFi.SSID().c_str(), C_BLUE, 2);
    centeredBold(174, "NETWERKTEST OK", C_GREEN, 2);
    centeredBold(205, "server-upload volgt met echte audio", C_MUTED, 1);
  } else if (syncPhase == SyncPhase::NO_CREDENTIALS) {
    centeredBold(132, "wifi_secrets.h ontbreekt", C_RED, 1);
    centeredBold(172, "credentials blijven lokaal", C_MUTED, 1);
    centeredBold(191, "en komen niet in GitHub", C_MUTED, 1);
  } else if (syncPhase == SyncPhase::FAILED) {
    centeredBold(139, "beide netwerken geprobeerd", C_MUTED, 1);
    centeredBold(178, "druk OPNIEUW om nogmaals te testen", C_MUTED, 1);
  }
}

void drawSync() {
  gfx->fillScreen(C_BG);
  drawTopBar("SYNC");
  drawSyncInfo();
  bigZone(SYNC_RETRY, "OPNIEUW", C_BLUE, C_WHITE);
  bigZone(SYNC_BACK, "TERUG", C_NAVY, C_WHITE, "Wi-Fi weer uit");
}

void drawRecordingTimerOnly() {
  uint32_t sec = activeElapsedMs() / 1000;
  if (sec == lastDisplayedSecond) return;
  lastDisplayedSecond = sec;

  char elapsed[16];
  formatElapsed(elapsed, sizeof(elapsed));
  gfx->fillRect(70, 47, 140, 15, C_WHITE);
  centeredBold(49, elapsed, C_MUTED, 1);
}

void drawFinishedCountdownOnly() {
  if (state != AppState::FINISHED) return;
  uint32_t elapsed = millis() - finishedAtMs;
  uint32_t remain = elapsed >= FINISHED_AUTO_HOME_MS
                        ? 0
                        : (FINISHED_AUTO_HOME_MS - elapsed + 999) / 1000;
  if (remain == lastFinishedCountdown) return;
  lastFinishedCountdown = remain;

  char line[32];
  snprintf(line, sizeof(line), "AUTO TERUG OVER %lu SEC", (unsigned long)remain);
  gfx->fillRect(42, 47, 196, 15, C_WHITE);
  centeredBold(49, line, C_MUTED, 1);
}

void render(bool force = false) {
  if (!force && !screenDirty) {
    if (state == AppState::RECORDING || state == AppState::PAUSED) {
      drawRecordingTimerOnly();
    } else if (state == AppState::FINISHED) {
      drawFinishedCountdownOnly();
    }
    return;
  }

  screenDirty = false;
  switch (state) {
    case AppState::HOME: drawHome(); break;
    case AppState::MODE_CONFIRM: drawModeConfirm(); break;
    case AppState::RECORDING: drawRecording(); break;
    case AppState::PAUSED: drawPaused(); break;
    case AppState::FINISHED: drawFinished(); break;
    case AppState::MENU: drawMenu(); break;
    case AppState::STATUS: drawStatus(); break;
    case AppState::SYNC: drawSync(); break;
  }
}

// -----------------------------------------------------------------------------
// Touch
// -----------------------------------------------------------------------------
bool readTouch(uint16_t &x, uint16_t &y) {
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
  return true;
}

void initTouch() {
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(300000);
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.endTransmission();
}

// -----------------------------------------------------------------------------
// Manual Wi-Fi / sync preparation
// Wi-Fi is OFF at boot and whenever the user leaves the sync screen.
// Credentials live only in gitignored include/wifi_secrets.h.
// -----------------------------------------------------------------------------
void wifiOff() {
  WiFi.disconnect(true, true);
  delay(20);
  WiFi.mode(WIFI_OFF);
}

void startWifiAttempt(uint8_t index) {
#if VISITESCRIBE_WIFI_CONFIGURED
  syncNetwork = index;
  WiFi.disconnect(true, true);
  delay(20);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);

  const char *ssid = index == 0 ? VISITESCRIBE_WIFI_SSID_1 : VISITESCRIBE_WIFI_SSID_2;
  const char *password = index == 0 ? VISITESCRIBE_WIFI_PASSWORD_1 : VISITESCRIBE_WIFI_PASSWORD_2;
  WiFi.begin(ssid, password);
  syncPhase = index == 0 ? SyncPhase::CONNECTING_1 : SyncPhase::CONNECTING_2;
  syncAttemptStartedMs = millis();
#else
  (void)index;
  syncPhase = SyncPhase::NO_CREDENTIALS;
#endif
  screenDirty = true;
}

void beginManualSync() {
  state = AppState::SYNC;
#if VISITESCRIBE_WIFI_CONFIGURED
  startWifiAttempt(0);
#else
  syncPhase = SyncPhase::NO_CREDENTIALS;
  screenDirty = true;
#endif
}

void serviceSync() {
  if (state != AppState::SYNC) return;
  if (syncPhase != SyncPhase::CONNECTING_1 &&
      syncPhase != SyncPhase::CONNECTING_2) return;

  if (WiFi.status() == WL_CONNECTED) {
    syncPhase = SyncPhase::CONNECTED;
    screenDirty = true;
    Serial.printf("Wi-Fi connected: %s, IP=%s\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    return;
  }

  if (millis() - syncAttemptStartedMs >= WIFI_ATTEMPT_MS) {
    if (syncPhase == SyncPhase::CONNECTING_1) {
      startWifiAttempt(1);
    } else {
      wifiOff();
      syncPhase = SyncPhase::FAILED;
      screenDirty = true;
    }
  }
}

// -----------------------------------------------------------------------------
// Session actions. UI feedback happens before potentially slower SD writes;
// event timestamps are captured first, so responsiveness does not reduce
// timeline precision.
// -----------------------------------------------------------------------------
void selectMode(Mode mode) {
  selectedMode = mode;
  state = AppState::MODE_CONFIRM;
  screenDirty = true;
}

void startDemo() {
  sessionStartedMs = millis();
  totalPausedMs = 0;
  pauseStartedMs = 0;
  markerCount = 0;
  patientNumber = 1;

  snprintf(currentLogPath, sizeof(currentLogPath),
           "/visitescribe/demo_%08lx.csv",
           (unsigned long)sessionStartedMs);

  state = AppState::RECORDING;
  screenDirty = true;
  render(true);  // instant visual feedback before SD I/O

  if (sdOk) {
    File f = SD.open(currentLogPath, FILE_WRITE);
    if (f) {
      f.println("elapsed_ms,event,patient,markers");
      f.close();
    }
  }
  logEventAt("session_started", 0);
}

void togglePrivacy() {
  uint32_t eventOffset = activeElapsedMs();
  if (state == AppState::RECORDING) {
    pauseStartedMs = millis();
    state = AppState::PAUSED;
    screenDirty = true;
    render(true);
    logEventAt("privacy_pause_started", eventOffset);
  } else if (state == AppState::PAUSED) {
    uint32_t pausedFor = millis() - pauseStartedMs;
    totalPausedMs += pausedFor;
    pauseStartedMs = 0;
    state = AppState::RECORDING;
    screenDirty = true;
    render(true);
    logEventAt("privacy_pause_ended", eventOffset);
  }
}

void addMarker() {
  uint32_t eventOffset = activeElapsedMs();
  markerCount++;
  if (selectedMode == Mode::ROUND) patientNumber++;

  screenDirty = true;
  render(true);
  logEventAt(selectedMode == Mode::ROUND ? "patient_boundary" : "marker",
             eventOffset);
}

void stopDemo() {
  if (state != AppState::RECORDING && state != AppState::PAUSED) return;
  uint32_t eventOffset = activeElapsedMs();
  bool wasPaused = state == AppState::PAUSED;
  uint32_t pausedFor = wasPaused ? millis() - pauseStartedMs : 0;

  finishedAtMs = millis();
  state = AppState::FINISHED;
  screenDirty = true;
  render(true);
  logEventAt("session_stopped", eventOffset);

  if (wasPaused) {
    totalPausedMs += pausedFor;
    pauseStartedMs = 0;
  }
}

void resumeFinishedSession() {
  if (state != AppState::FINISHED) return;
  uint32_t now = millis();
  totalPausedMs += now - finishedAtMs;
  finishedAtMs = 0;
  state = AppState::RECORDING;
  uint32_t eventOffset = activeElapsedMs();
  screenDirty = true;
  render(true);
  logEventAt("session_resumed", eventOffset);
}

void handleTouchPress(uint16_t x, uint16_t y) {
  Serial.printf("touch x=%u y=%u state=%u\n", x, y, (unsigned)state);

  switch (state) {
    case AppState::HOME:
      if (HOME_VISIT.contains(x, y)) selectMode(Mode::VISIT);
      else if (HOME_ROUND.contains(x, y)) selectMode(Mode::ROUND);
      else if (HOME_MEETING.contains(x, y)) selectMode(Mode::MEETING);
      else if (HOME_MENU.contains(x, y)) {
        state = AppState::MENU;
        screenDirty = true;
      }
      break;

    case AppState::MODE_CONFIRM:
      if (TWO_TOP.contains(x, y)) startDemo();
      else if (TWO_BOTTOM.contains(x, y)) goHome();
      break;

    case AppState::RECORDING:
      if (THREE_TOP.contains(x, y)) togglePrivacy();
      else if (THREE_MIDDLE.contains(x, y)) addMarker();
      else if (THREE_BOTTOM.contains(x, y)) stopDemo();
      break;

    case AppState::PAUSED:
      if (THREE_TOP.contains(x, y)) togglePrivacy();
      else if (THREE_MIDDLE.contains(x, y)) addMarker();
      else if (THREE_BOTTOM.contains(x, y)) stopDemo();
      break;

    case AppState::FINISHED:
      if (TWO_TOP.contains(x, y)) resumeFinishedSession();
      else if (TWO_BOTTOM.contains(x, y)) goHome();
      break;

    case AppState::MENU:
      if (THREE_TOP.contains(x, y)) {
        state = AppState::STATUS;
        updateBattery(true);
        screenDirty = true;
      } else if (THREE_MIDDLE.contains(x, y)) {
        beginManualSync();
      } else if (THREE_BOTTOM.contains(x, y)) {
        goHome();
      }
      break;

    case AppState::STATUS:
      if (STATUS_BACK.contains(x, y)) {
        state = AppState::MENU;
        screenDirty = true;
      }
      break;

    case AppState::SYNC:
      if (SYNC_RETRY.contains(x, y)) {
        startWifiAttempt(0);
      } else if (SYNC_BACK.contains(x, y)) {
        wifiOff();
        syncPhase = SyncPhase::NOT_STARTED;
        state = AppState::MENU;
        screenDirty = true;
      }
      break;
  }
}

void pollBootButton() {
  bool down = digitalRead(VISITESCRIBE_BOOT_GPIO) == LOW;
  uint32_t now = millis();
  if (down && !bootWasDown) bootPressedAtMs = now;

  if (down && bootWasDown && bootPressedAtMs != 0 &&
      now - bootPressedAtMs > 1200) {
    if (state == AppState::RECORDING || state == AppState::PAUSED) {
      stopDemo();
    } else {
      if (state == AppState::SYNC) wifiOff();
      goHome();
    }
    bootPressedAtMs = 0;
  }

  if (!down) bootPressedAtMs = 0;
  bootWasDown = down;
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("VisiteScribe MINI - large-touch OurMind UI v0.5");
  Serial.printf("Board revision target: V%d\n", VISITESCRIBE_BOARD_REV);

  pinMode(VISITESCRIBE_BOOT_GPIO, INPUT_PULLUP);
  pinMode(BAT_ADC_GPIO, INPUT);

  if (!gfx->begin()) Serial.println("ERROR: display init failed");
  gfx->fillScreen(C_BG);

  initTouch();
  sdOk = initSD();
  updateBattery(true);

  // Deliberate privacy/power policy: never associate with Wi-Fi at boot.
  WiFi.mode(WIFI_OFF);

  Serial.printf("microSD: %s\n", sdOk ? "OK" : "NOT FOUND");
  Serial.printf("battery: %s (%u mV)\n",
                batteryPercent < 0 ? "not detected" : "detected", batteryMv);
#if VISITESCRIBE_WIFI_CONFIGURED
  Serial.println("Wi-Fi credentials: local config present; radio remains OFF until SYNC.");
#else
  Serial.println("Wi-Fi credentials: not configured; copy wifi_secrets.example.h locally.");
#endif
  Serial.println("Microphones: main demo still simulated; IM73D122 lab firmware is separate.");

  render(true);
}

void loop() {
  uint16_t x = 0, y = 0;
  bool touchDown = readTouch(x, y);
  if (touchDown && !touchWasDown) handleTouchPress(x, y);
  touchWasDown = touchDown;

  pollBootButton();
  serviceSync();

  bool batteryChanged = updateBattery(false);
  if (batteryChanged && batteryPercent != lastBatteryUiPercent) {
    if (state == AppState::STATUS) screenDirty = true;
    else drawBatteryBadge();
  }

  if (state == AppState::FINISHED &&
      millis() - finishedAtMs >= FINISHED_AUTO_HOME_MS) {
    goHome();
  }

  render();
  delay(12);
}
