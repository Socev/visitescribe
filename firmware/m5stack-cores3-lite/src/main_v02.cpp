#include <Arduino.h>
#include <M5Unified.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include "brian_ourmind_logo.h"

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

// VisiteScribe CoreS3-Lite v0.2
// CoreS3-Lite input is handled directly:
// - FT6336U touch controller at 0x38 on the internal I2C bus
// - AXP2101 PEK status for the physical power button
// This deliberately does not depend on M5Unified's synthesized touch/button events.

static constexpr int SCREEN_W = 320;
static constexpr int SCREEN_H = 240;
static constexpr int HEADER_H = 40;

static constexpr int SD_SCK  = 36;
static constexpr int SD_MISO = 35;
static constexpr int SD_MOSI = 37;
static constexpr int SD_CS   = 4;
static constexpr uint32_t SD_HZ = 25000000;

static constexpr uint8_t FT6336_ADDR = 0x38;
static constexpr uint8_t AW9523_ADDR = 0x58;
static constexpr uint32_t I2C_HZ = 400000;

static constexpr uint32_t AUDIO_RATE = 48000;
static constexpr uint16_t AUDIO_CHANNELS = 2;
static constexpr uint16_t AUDIO_BITS = 16;
static constexpr size_t AUDIO_BLOCK_SAMPLES = 4096;
static int16_t audioBufA[AUDIO_BLOCK_SAMPLES];
static int16_t audioBufB[AUDIO_BLOCK_SAMPLES];

static constexpr uint8_t BRIGHTNESS_ACTIVE = 180;
static constexpr uint8_t BRIGHTNESS_DIM = 36;
static constexpr uint32_t FINISHED_AUTO_HOME_MS = 3000;
static constexpr uint32_t FALSE_START_AUTO_HOME_MS = 2500;
static constexpr uint32_t FALSE_START_LIMIT_MS = 10000;
static constexpr uint32_t MENU_AUTO_HOME_MS = 30000;
static constexpr uint32_t WIFI_ATTEMPT_MS = 15000;

// Brian functional UI: white base, near-black text, violet accent.
// These are RGB565 approximations of the UX brief values.
static constexpr uint16_t C_BG      = 0xFFFF; // #FFFFFF
static constexpr uint16_t C_WHITE   = 0xFFFF;
static constexpr uint16_t C_NAVY    = 0x2103; // #20211F
static constexpr uint16_t C_BLUE    = 0x62BB; // violet accent #6654D9
static constexpr uint16_t C_VIOLET  = 0x62BB;
static constexpr uint16_t C_TEAL    = 0x62BB;
static constexpr uint16_t C_GREEN   = 0x2328; // #246746
static constexpr uint16_t C_RED     = 0xB106; // #B42332
static constexpr uint16_t C_AMBER   = 0x8A80; // #885300
static constexpr uint16_t C_GREY    = 0x632B; // #62665F
static constexpr uint16_t C_LINE    = 0xE73C; // #E4E7E2
static constexpr uint16_t C_SOFT    = 0xF7BE; // #F5F6F3
static constexpr uint16_t C_VIOLET_SOFT = 0xF77F; // #F0EDFC

struct Rect {
  int x, y, w, h;
  bool contains(int px, int py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

static const Rect HOME_VISIT   {0, 40, 320, 50};
static const Rect HOME_ROUND   {0, 90, 320, 50};
static const Rect HOME_MEETING {0,140, 320, 50};
static const Rect HOME_MENU    {0,190, 320, 50};
static const Rect TWO_TOP      {0, 40, 320,100};
static const Rect TWO_BOTTOM   {0,140, 320,100};
static const Rect THREE_TOP    {0, 40, 320, 67};
static const Rect THREE_MIDDLE {0,107, 320, 67};
static const Rect THREE_BOTTOM {0,174, 320, 66};
static const Rect STATUS_DETAILS {16,148,288,40};
static const Rect STATUS_BACK    {16,194,288,40};
static const Rect SYNC_RETRY     {16,140,288,42};
static const Rect SYNC_BACK      {16,190,288,42};

enum class AppState : uint8_t {
  HOME, MODE_CONFIRM, RECORDING, PAUSED, SAVING, FINISHED,
  MENU, STATUS, DETAILS, SYNC, ERROR
};
enum class Mode : uint8_t { VISIT, ROUND, MEETING };
enum class QuickChoice : uint8_t { PATIENT, MEETING, STOP };
enum class SyncPhase : uint8_t { NOT_STARTED, NO_CREDENTIALS, CONNECTING_1, CONNECTING_2, CONNECTED, FAILED };
enum class DisplayPower : uint8_t { ACTIVE, DIMMED, OFF };

AppState state = AppState::HOME;
Mode selectedMode = Mode::VISIT;
SyncPhase syncPhase = SyncPhase::NOT_STARTED;
DisplayPower displayPower = DisplayPower::ACTIVE;

// Later sync layers install these lightweight UI hooks once their inventory
// and server state are available.
static bool (*vsSyncTouchLockedHook)() = nullptr;
static uint32_t (*vsPendingCountHook)() = nullptr;
static bool (*vsSyncRetryAllowedHook)() = nullptr;
static bool (*vsSyncDoneHook)() = nullptr;

bool sdOk = false;
bool touchOk = false;
bool screenDirty = true;
bool sessionOpen = false;
bool captureRunning = false;
bool audioError = false;
bool touchWasDown = false;

// Pocket-first control model. A PWR-started recording captures immediately,
// while touch is only accepted for the first 10 seconds to choose
// VISITE/VERGADERING. VISITE remains single-patient unless double-PWR creates
// a second patient.
bool quickModeChoiceActive = false;
bool visitPatientFlow = false;
QuickChoice quickChoice = QuickChoice::PATIENT;
uint32_t quickModeChoiceStartedMs = 0;
uint32_t quickModeChoiceDeadlineMs = 0;
static constexpr uint32_t QUICK_MODE_CHOICE_MS = 10000;
static constexpr uint32_t QUICK_MODE_EXPLICIT_DWELL_MS = 3000;
static constexpr uint32_t QUICK_MODE_STOP_DWELL_MS = 1200;

bool lastSessionFalseStart = false;
bool lastSessionSaved = false;
uint32_t patientSegmentStartOffsetMs = 0;
char recordingToast[48] = {0};
uint32_t recordingToastUntilMs = 0;
String uiErrorTitle;
String uiErrorDetail;

uint32_t sessionStartedMs = 0;
uint32_t pauseStartedMs = 0;
uint32_t totalPausedMs = 0;
uint32_t finishedAtMs = 0;
uint32_t lastUserActivityMs = 0;
uint32_t lastUiSecond = UINT32_MAX;
uint32_t lastBatteryRefreshMs = 0;
uint32_t syncAttemptStartedMs = 0;

uint16_t sessionId = 0;
uint16_t patientNumber = 1;
uint16_t segmentNumber = 1;
uint16_t markerCount = 0;
uint8_t syncNetwork = 0;

int batteryPct = -1;
bool batteryCharging = false;

char eventsPath[96] = {0};
char wavTmpPath[128] = {0};
char wavFinalPath[128] = {0};
File wavFile;
uint32_t wavDataBytes = 0;

#ifdef VISITESCRIBE_DIRECT_OPUS
#include "direct_opus_backend.h"
#endif

struct AudioDone { int16_t* data; size_t samples; };
QueueHandle_t audioDoneQueue = nullptr;

struct __attribute__((packed)) WAVHeader {
  char riff[4] = {'R','I','F','F'};
  uint32_t fileSize = 36;
  char wave[4] = {'W','A','V','E'};
  char fmt[4] = {'f','m','t',' '};
  uint32_t fmtSize = 16;
  uint16_t audioFormat = 1;
  uint16_t numChannels = AUDIO_CHANNELS;
  uint32_t sampleRate = AUDIO_RATE;
  uint32_t byteRate = AUDIO_RATE * AUDIO_CHANNELS * (AUDIO_BITS / 8);
  uint16_t blockAlign = AUDIO_CHANNELS * (AUDIO_BITS / 8);
  uint16_t bitsPerSample = AUDIO_BITS;
  char data[4] = {'d','a','t','a'};
  uint32_t dataSize = 0;
};

void centeredText(int y, const char* text, uint16_t color, uint8_t size = 1) {
  M5.Display.setTextColor(color);
  M5.Display.setTextSize(size);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(text, SCREEN_W / 2, y);
}

static void drawOurMindBootLogo() {
  const int x0 = SCREEN_W - BRIAN_OURMIND_LOGO_W - 8;
  const int y0 = 6;
  const uint16_t ink = 0x0000;

  // Monochrome bitmap derived from the supplied OurMind logo screenshot.
  // White is already the boot background; only black pixels are drawn.
  const int rowBytes = (BRIAN_OURMIND_LOGO_W + 7) / 8;
  for (int y = 0; y < BRIAN_OURMIND_LOGO_H; ++y) {
    for (int x = 0; x < BRIAN_OURMIND_LOGO_W; ++x) {
      const uint8_t b = pgm_read_byte(
          &BRIAN_OURMIND_LOGO_BITS[y * rowBytes + (x >> 3)]);
      if (b & (0x80 >> (x & 7))) {
        M5.Display.drawPixel(x0 + x, y0 + y, ink);
      }
    }
  }
}

static void drawBrianRobotFrame(int frame) {
  const int bob = (frame % 8 < 4) ? 0 : 2;
  const int cx = 155;
  const int headY = 62 + bob;
  const uint16_t ink = 0x18E3;
  const uint16_t accent = C_BLUE;
  const uint16_t pale = 0xEF7D;

  // Erase the animation area only; the OurMind mark stays stable.
  M5.Display.fillRect(70, 46, 170, 150, C_WHITE);

  // Antenna + little medical cross.
  M5.Display.drawLine(cx, headY - 14, cx, headY - 5, ink);
  M5.Display.fillCircle(cx, headY - 17, 4, accent);
  M5.Display.fillRect(cx - 2, headY - 21, 4, 8, C_WHITE);
  M5.Display.fillRect(cx - 4, headY - 19, 8, 4, C_WHITE);

  // Robot head.
  M5.Display.fillRoundRect(cx - 44, headY, 88, 58, 16, pale);
  M5.Display.drawRoundRect(cx - 44, headY, 88, 58, 16, ink);
  M5.Display.fillRoundRect(cx - 35, headY + 10, 70, 31, 10, C_WHITE);
  M5.Display.drawRoundRect(cx - 35, headY + 10, 70, 31, 10, ink);

  const bool blink = (frame == 5 || frame == 6 || frame == 13);
  if (blink) {
    M5.Display.drawFastHLine(cx - 23, headY + 25, 13, ink);
    M5.Display.drawFastHLine(cx + 10, headY + 25, 13, ink);
  } else {
    M5.Display.fillCircle(cx - 17, headY + 25, 5, accent);
    M5.Display.fillCircle(cx + 17, headY + 25, 5, accent);
    M5.Display.fillCircle(cx - 16, headY + 23, 1, C_WHITE);
    M5.Display.fillCircle(cx + 18, headY + 23, 1, C_WHITE);
  }
  M5.Display.drawLine(cx - 8, headY + 36, cx, headY + 39, ink);
  M5.Display.drawLine(cx, headY + 39, cx + 8, headY + 36, ink);

  // Doctor body + coat.
  const int bodyY = headY + 62;
  M5.Display.fillRoundRect(cx - 48, bodyY, 96, 53, 10, C_WHITE);
  M5.Display.drawRoundRect(cx - 48, bodyY, 96, 53, 10, ink);
  M5.Display.drawLine(cx, bodyY + 2, cx, bodyY + 49, C_LINE);
  M5.Display.drawLine(cx - 31, bodyY + 10, cx - 10, bodyY + 28, C_LINE);
  M5.Display.drawLine(cx + 31, bodyY + 10, cx + 10, bodyY + 28, C_LINE);

  // Stethoscope.
  M5.Display.drawCircle(cx - 17, bodyY + 14, 4, accent);
  M5.Display.drawCircle(cx + 17, bodyY + 14, 4, accent);
  M5.Display.drawLine(cx - 17, bodyY + 18, cx - 17, bodyY + 31, accent);
  M5.Display.drawLine(cx + 17, bodyY + 18, cx + 17, bodyY + 31, accent);
  M5.Display.drawCircle(cx, bodyY + 36, 8, accent);
  M5.Display.fillCircle(cx, bodyY + 36, 3, accent);

  // A tiny moving heartbeat trace.
  const int pulseX = 86 + (frame * 5) % 118;
  M5.Display.drawFastHLine(86, 190, 118, C_LINE);
  M5.Display.drawLine(pulseX - 8, 190, pulseX - 3, 190, accent);
  M5.Display.drawLine(pulseX - 3, 190, pulseX, 182, accent);
  M5.Display.drawLine(pulseX, 182, pulseX + 4, 197, accent);
  M5.Display.drawLine(pulseX + 4, 197, pulseX + 8, 190, accent);
}

static void showBrianBootAnimation() {
  M5.Display.wakeup();
  M5.Display.setBrightness(BRIGHTNESS_ACTIVE);
  M5.Display.fillScreen(C_WHITE);
  drawOurMindBootLogo();

  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(C_GREY);
  M5.Display.setTextSize(1);
  M5.Display.drawString("VisiteScribe", 10, 10);

  for (int frame = 0; frame < 14; ++frame) {
    drawBrianRobotFrame(frame);
    delay(90);
  }

  M5.Display.fillRect(70, 198, 170, 35, C_WHITE);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(C_NAVY);
  M5.Display.setTextSize(3);
  M5.Display.drawString("BRIAN", 155, 211);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_GREY);
  M5.Display.drawString("ready to listen", 155, 231);
  delay(3000);
}

void zone(const Rect& r, const char* title, uint16_t fill, uint16_t fg,
          const char* subtitle = nullptr) {
  // Legacy helper retained for compatibility with older/diagnostic screens.
  // Functional Brian screens use white cards with restrained accents.
  M5.Display.fillRect(r.x, r.y, r.w, r.h, C_WHITE);
  const int inset = 8;
  M5.Display.fillRoundRect(
      r.x + inset, r.y + 4, r.w - inset * 2, r.h - 8, 8, fill);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(fg);
  const uint8_t size = strlen(title) <= 12 ? 2 : 1;
  M5.Display.setTextSize(size);
  const int cy = r.y + r.h / 2 - (subtitle ? 7 : 0);
  M5.Display.drawString(title, r.x + r.w / 2, cy);
  if (subtitle) {
    M5.Display.setTextSize(1);
    M5.Display.drawString(subtitle, r.x + r.w / 2, cy + 19);
  }
}

static void drawTouchButton(const Rect& r, const char* title,
                            const char* subtitle = nullptr,
                            bool primary = false,
                            bool selected = false) {
  const int x = r.x + 12;
  const int y = r.y + 5;
  const int w = r.w - 24;
  const int h = r.h - 10;
  const uint16_t fill = primary ? C_NAVY :
      (selected ? C_VIOLET_SOFT : C_WHITE);
  const uint16_t border = primary ? C_NAVY :
      (selected ? C_VIOLET : C_LINE);
  const uint16_t text = primary ? C_WHITE : C_NAVY;

  M5.Display.fillRoundRect(x, y, w, h, 8, fill);
  M5.Display.drawRoundRect(x, y, w, h, 8, border);
  if (selected) {
    M5.Display.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 7, border);
    M5.Display.drawRoundRect(x + 2, y + 2, w - 4, h - 4, 6, border);
  }
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(text);
  M5.Display.setTextSize(strlen(title) <= 15 ? 2 : 1);
  int cy = y + h / 2 - (subtitle ? 7 : 0);
  M5.Display.drawString(title, x + w / 2, cy);
  if (subtitle) {
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(primary ? C_WHITE : C_GREY);
    M5.Display.drawString(subtitle, x + w / 2, cy + 18);
  }
}

static void drawPwrHints(const char* first, const char* second = nullptr) {
  M5.Display.fillRect(0, 184, SCREEN_W, 56, C_WHITE);
  M5.Display.drawFastHLine(16, 184, SCREEN_W - 32, C_LINE);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(C_NAVY);
  M5.Display.setTextSize(1);
  if (second) {
    M5.Display.drawString(first, SCREEN_W / 2, 202);
    M5.Display.setTextColor(C_GREY);
    M5.Display.drawString(second, SCREEN_W / 2, 224);
  } else {
    M5.Display.drawString(first, SCREEN_W / 2, 214);
  }
}

static uint32_t pendingCountUi() {
  return vsPendingCountHook ? vsPendingCountHook() : 0;
}

static void setRecordingToast(const char* text, uint32_t ms = 1500) {
  snprintf(recordingToast, sizeof(recordingToast), "%s", text ? text : "");
  recordingToastUntilMs = millis() + ms;
  screenDirty = true;
}

void refreshBattery() {
  batteryPct = M5.Power.getBatteryLevel();
  batteryCharging = M5.Power.isCharging() == m5::Power_Class::is_charging;
  lastBatteryRefreshMs = millis();
}

void drawHeader(const char* status, const char* sub = nullptr) {
  M5.Display.fillRect(0, 0, SCREEN_W, HEADER_H, C_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(C_NAVY);
  M5.Display.setTextSize(1);
  M5.Display.drawString("Brian", 10, 7);

  char batt[24];
  if (batteryPct < 0) snprintf(batt, sizeof(batt), "--%%");
  else snprintf(batt, sizeof(batt), "%s%d%%",
                batteryCharging ? "+" : "", batteryPct);
  M5.Display.setTextDatum(top_right);
  M5.Display.setTextColor(
      (batteryPct >= 0 && batteryPct <= 15) ? C_AMBER : C_GREY);
  M5.Display.drawString(batt, 310, 7);

  if (status && status[0]) {
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(C_NAVY);
    M5.Display.setTextSize(strlen(status) <= 24 ? 2 : 1);
    M5.Display.drawString(status, SCREEN_W / 2, sub ? 23 : 28);
  }
  if (sub && sub[0]) {
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(C_GREY);
    M5.Display.drawString(sub, SCREEN_W / 2, 36);
  }
  M5.Display.drawFastHLine(0, HEADER_H - 1, SCREEN_W, C_LINE);
}

const char* modeTitle(Mode m) {
  if (m == Mode::VISIT) return "Patiënt";
  if (m == Mode::ROUND) return "Patientronde";
  return "Vergadering";
}

uint32_t activeElapsedMs() {
  if (!sessionOpen || sessionStartedMs == 0) return 0;
  uint32_t paused = totalPausedMs;
  if (state == AppState::PAUSED) paused += millis() - pauseStartedMs;
  return millis() - sessionStartedMs - paused;
}

static uint32_t recordingDisplayElapsedMs() {
  const uint32_t total = activeElapsedMs();
  if (selectedMode == Mode::MEETING) return total;
  return total >= patientSegmentStartOffsetMs
      ? total - patientSegmentStartOffsetMs : 0;
}

static void formatCompactElapsed(uint32_t ms, char* out, size_t len) {
  const uint32_t sec = ms / 1000;
  if (sec >= 3600) {
    snprintf(out, len, "%lu:%02lu:%02lu",
             (unsigned long)(sec / 3600),
             (unsigned long)((sec % 3600) / 60),
             (unsigned long)(sec % 60));
  } else {
    snprintf(out, len, "%02lu:%02lu",
             (unsigned long)(sec / 60),
             (unsigned long)(sec % 60));
  }
}

void formatElapsed(char* out, size_t len) {
  formatCompactElapsed(activeElapsedMs(), out, len);
}

void drawHome() {
  drawHeader("");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);

  centeredText(74, "Klaar voor", C_NAVY, 3);
  centeredText(104, "opname", C_NAVY, 3);
  centeredText(134, "Standaard: Patiënt", C_GREY, 1);

  centeredText(160, "Lokaal klaar voor gebruik", C_GREY, 1);
  drawPwrHints("1x PWR  -  Start", "2x PWR  -  Menu");
}

void drawModeConfirm() {
  drawHeader(modeTitle(selectedMode));
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);
  centeredText(88, "Klaar om op te nemen", C_NAVY, 2);
  drawTouchButton(TWO_TOP, "Start", nullptr, true);
  drawTouchButton(TWO_BOTTOM, "Terug");
}

void drawRecording() {
  char elapsed[16];
  formatCompactElapsed(recordingDisplayElapsedMs(), elapsed, sizeof(elapsed));

  drawHeader("");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);

  // Recording state is always the most prominent fact.
  M5.Display.fillCircle(23, 57, 6, C_RED);
  M5.Display.setTextDatum(middle_left);
  M5.Display.setTextColor(C_RED);
  M5.Display.setTextSize(2);
  M5.Display.drawString("Neemt op", 38, 57);

  if (quickModeChoiceActive) {
    const int32_t leftMs =
        static_cast<int32_t>(quickModeChoiceDeadlineMs - millis());
    const uint32_t remaining =
        leftMs <= 0 ? 0 : (static_cast<uint32_t>(leftMs) + 999) / 1000;

    Rect patientChoice{4, 68, 312, 44};
    Rect meetingChoice{4, 114, 312, 44};
    Rect stopChoice{236, 160, 76, 24};

    drawTouchButton(
        patientChoice, "Patiënt", "standaard", false,
        quickChoice == QuickChoice::PATIENT);
    drawTouchButton(
        meetingChoice, "Vergadering", nullptr, false,
        quickChoice == QuickChoice::MEETING);

    const bool stopSelected = quickChoice == QuickChoice::STOP;
    const uint16_t stopFill = stopSelected ? C_RED : C_WHITE;
    const uint16_t stopText = stopSelected ? C_WHITE : C_RED;
    M5.Display.fillRoundRect(
        stopChoice.x, stopChoice.y, stopChoice.w, stopChoice.h, 7, stopFill);
    M5.Display.drawRoundRect(
        stopChoice.x, stopChoice.y, stopChoice.w, stopChoice.h, 7, C_RED);
    if (stopSelected) {
      M5.Display.drawRoundRect(
          stopChoice.x + 1, stopChoice.y + 1,
          stopChoice.w - 2, stopChoice.h - 2, 6, C_RED);
      M5.Display.drawRoundRect(
          stopChoice.x + 2, stopChoice.y + 2,
          stopChoice.w - 4, stopChoice.h - 4, 5, C_RED);
    }
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(stopText);
    M5.Display.setTextSize(1);
    M5.Display.drawString(
        "STOP", stopChoice.x + stopChoice.w / 2,
        stopChoice.y + stopChoice.h / 2);

    char line[40];
    snprintf(line, sizeof(line), "Keuze over %lus",
             (unsigned long)remaining);
    M5.Display.setTextDatum(middle_left);
    M5.Display.setTextColor(C_GREY);
    M5.Display.setTextSize(1);
    M5.Display.drawString(line, 16, 172);

    drawPwrHints("PWR  -  volgende keuze");
    lastUiSecond = activeElapsedMs() / 1000;
    return;
  }

  char context[56];
  if (selectedMode == Mode::MEETING) {
    snprintf(context, sizeof(context), "Vergadering");
  } else {
    snprintf(context, sizeof(context), "Patiënt %u", patientNumber);
  }
  centeredText(84, context, C_NAVY, 2);

  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(C_NAVY);
  M5.Display.setTextSize(4);
  M5.Display.drawString(elapsed, SCREEN_W / 2, 124);

  if (recordingToast[0] &&
      static_cast<int32_t>(recordingToastUntilMs - millis()) > 0) {
    centeredText(158, recordingToast, C_VIOLET, 1);
  } else if (selectedMode == Mode::MEETING && markerCount > 0) {
    char marks[40];
    snprintf(marks, sizeof(marks), "%u markering%s",
             markerCount, markerCount == 1 ? "" : "en");
    centeredText(158, marks, C_GREY, 1);
  } else {
    centeredText(158, "Touch uit", C_GREY, 1);
  }

  drawPwrHints(
      "1x PWR  -  Stop",
      selectedMode == Mode::MEETING
          ? "2x PWR  -  Markeer"
          : "2x PWR  -  Volgende patiënt");
  lastUiSecond = activeElapsedMs() / 1000;
}

void drawPaused() {
  drawHeader("Pauze");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);
  centeredText(92, "Opname gepauzeerd", C_AMBER, 2);
  centeredText(130, "Deze functie hoort niet bij de", C_GREY, 1);
  centeredText(148, "normale Brian-workflow.", C_GREY, 1);
  drawPwrHints("1x PWR  -  Stop");
}

static void drawSaving() {
  drawHeader("");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);
  centeredText(98, "Opname bewaren...", C_NAVY, 2);
  centeredText(132, "Een ogenblik", C_GREY, 1);
  centeredText(166, "PWR tijdelijk uitgeschakeld", C_GREY, 1);
}

void drawFinished() {
  drawHeader("");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);

  if (lastSessionFalseStart) {
    centeredText(86, "Valse start", C_AMBER, 3);
    centeredText(124, "Opname verwijderd", C_GREY, 1);
    centeredText(150, "korter dan 10 seconden", C_GREY, 1);
  } else {
    M5.Display.drawCircle(42, 87, 13, C_GREEN);
    M5.Display.drawLine(35, 87, 40, 92, C_GREEN);
    M5.Display.drawLine(40, 92, 50, 80, C_GREEN);
    M5.Display.setTextDatum(middle_left);
    M5.Display.setTextColor(C_GREEN);
    M5.Display.setTextSize(3);
    M5.Display.drawString("Opgeslagen", 70, 86);
    centeredText(122, "Op dit apparaat", C_GREY, 1);

    char context[56];
    if (selectedMode == Mode::MEETING) {
      snprintf(context, sizeof(context), "Vergadering");
    } else if (patientNumber <= 1) {
      snprintf(context, sizeof(context), "Patiënt - 1");
    } else {
      snprintf(context, sizeof(context), "Patiënten - %u", patientNumber);
    }
    centeredText(146, context, C_NAVY, 1);
    centeredText(166, "Nog te verzenden", C_VIOLET, 1);
  }

  drawPwrHints("1x PWR  -  Nieuwe opname", "2x PWR  -  Menu");
}

void drawMenu() {
  drawHeader("Menu");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);

  drawTouchButton(
      THREE_TOP, "Verzenden", "via wifi naar server", true);
  drawTouchButton(THREE_MIDDLE, "Apparaatstatus");
  drawTouchButton(THREE_BOTTOM, "Terug");
}

void drawStatus() {
  drawHeader("Apparaatstatus");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);

  char line[64];
  snprintf(line, sizeof(line), "Batterij                 %s%d%%",
           batteryCharging ? "+" : "", batteryPct);
  M5.Display.setTextDatum(middle_left);
  M5.Display.setTextColor(
      (batteryPct >= 0 && batteryPct <= 15) ? C_AMBER : C_NAVY);
  M5.Display.setTextSize(1);
  M5.Display.drawString(line, 24, 64);

  M5.Display.setTextColor(sdOk ? C_NAVY : C_RED);
  M5.Display.drawString(
      sdOk ? "Opslag                    Beschikbaar"
           : "Opslag                    Fout",
      24, 87);

  M5.Display.setTextColor(audioError ? C_RED : C_NAVY);
  M5.Display.drawString(
      audioError ? "Audio                     Fout"
                 : "Audio                     Gereed",
      24, 110);

  const bool wifi = WiFi.status() == WL_CONNECTED;
  M5.Display.setTextColor(C_NAVY);
  M5.Display.drawString(
      wifi ? "Wifi                      Verbonden"
           : "Wifi                      Niet verbonden",
      24, 133);

  drawTouchButton(STATUS_DETAILS, "Details");
  drawTouchButton(STATUS_BACK, "Terug");
}

static void drawDetails() {
  drawHeader("Details");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);

  char line[80];
  centeredText(59, sdOk ? "microSD: OK" : "microSD: FOUT",
               sdOk ? C_NAVY : C_RED, 1);
  centeredText(79, touchOk ? "Touch: FT6336 OK" : "Touch: FOUT",
               touchOk ? C_GREY : C_RED, 1);
#ifdef VISITESCRIBE_DIRECT_OPUS
  centeredText(99, audioError ? "Audio: FOUT" : "Audio: 16k mono Opus",
               audioError ? C_RED : C_GREY, 1);
#else
  centeredText(99, audioError ? "Audio: FOUT" : "Audio: 48k stereo WAV",
               audioError ? C_RED : C_GREY, 1);
#endif
  snprintf(line, sizeof(line), "Board ID: %d", (int)M5.getBoard());
  centeredText(119, line, C_GREY, 1);

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(line, sizeof(line), "SSID: %s", WiFi.SSID().c_str());
    centeredText(139, line, C_GREY, 1);
    snprintf(line, sizeof(line), "IP: %s", WiFi.localIP().toString().c_str());
    centeredText(159, line, C_GREY, 1);
  } else {
    centeredText(149, "Wifi: niet verbonden", C_GREY, 1);
  }

  drawTouchButton(STATUS_BACK, "Terug");
}

void drawSync() {
  drawHeader("Verzenden");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);

  if (syncPhase == SyncPhase::NO_CREDENTIALS) {
    centeredText(88, "Geen wifi ingesteld", C_AMBER, 2);
    centeredText(120, "Opnames blijven op dit apparaat", C_GREY, 1);
  } else if (syncPhase == SyncPhase::CONNECTING_1 ||
             syncPhase == SyncPhase::CONNECTING_2) {
    centeredText(88, "Wifi verbinden...", C_NAVY, 2);
    centeredText(120, "Even geduld", C_GREY, 1);
  } else if (syncPhase == SyncPhase::CONNECTED) {
    centeredText(88, "Opnames voorbereiden...", C_NAVY, 2);
    centeredText(120, "Wifi verbonden", C_GREY, 1);
  } else if (syncPhase == SyncPhase::FAILED) {
    centeredText(82, "Geen verbinding", C_AMBER, 2);
    centeredText(112, "Opnames blijven op dit apparaat", C_GREY, 1);
    drawTouchButton(SYNC_RETRY, "Opnieuw", nullptr, true);
    drawTouchButton(SYNC_BACK, "Later");
    return;
  } else {
    centeredText(95, "Klaar om te verzenden", C_NAVY, 2);
  }

  if (syncPhase != SyncPhase::CONNECTED) {
    drawTouchButton(SYNC_BACK, "Later");
  }
}

static void drawError() {
  drawHeader("");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);
  centeredText(80,
               uiErrorTitle.length() ? uiErrorTitle.c_str() : "Er ging iets mis",
               C_RED, 2);
  if (uiErrorDetail.length()) {
    String d = uiErrorDetail;
    if (d.length() > 48) d = d.substring(0, 48);
    centeredText(118, d.c_str(), C_GREY, 1);
  }
  centeredText(150, "Controleer Apparaatstatus", C_GREY, 1);
  drawTouchButton(STATUS_DETAILS, "Apparaatstatus");
  drawTouchButton(STATUS_BACK, "Terug");
}

static void updateRecordingDynamic() {
  if (state != AppState::RECORDING) return;

  if (quickModeChoiceActive) {
    const int32_t leftMs =
        static_cast<int32_t>(quickModeChoiceDeadlineMs - millis());
    const uint32_t remaining =
        leftMs <= 0 ? 0 : (static_cast<uint32_t>(leftMs) + 999) / 1000;
    char line[40];
    snprintf(line, sizeof(line), "Keuze over %lus",
             (unsigned long)remaining);

    // Only repaint the countdown text; the choice cards stay untouched.
    M5.Display.fillRect(12, 160, 210, 24, C_WHITE);
    M5.Display.setTextDatum(middle_left);
    M5.Display.setTextColor(C_GREY);
    M5.Display.setTextSize(1);
    M5.Display.drawString(line, 16, 172);
    lastUiSecond = activeElapsedMs() / 1000;
    return;
  }

  char elapsed[16];
  formatCompactElapsed(recordingDisplayElapsedMs(), elapsed, sizeof(elapsed));

  // The timer owns a fixed white rectangle. Clearing only this rectangle
  // prevents the visible whole-screen flash that used to happen every second.
  M5.Display.fillRect(54, 102, 212, 45, C_WHITE);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(C_NAVY);
  M5.Display.setTextSize(4);
  M5.Display.drawString(elapsed, SCREEN_W / 2, 124);

  // Toast/marker/touch line may also change without redrawing the frame.
  M5.Display.fillRect(24, 148, 272, 28, C_WHITE);
  if (recordingToast[0] &&
      static_cast<int32_t>(recordingToastUntilMs - millis()) > 0) {
    centeredText(158, recordingToast, C_VIOLET, 1);
  } else if (selectedMode == Mode::MEETING && markerCount > 0) {
    char marks[40];
    snprintf(marks, sizeof(marks), "%u markering%s",
             markerCount, markerCount == 1 ? "" : "en");
    centeredText(158, marks, C_GREY, 1);
  } else {
    centeredText(158, "Touch uit", C_GREY, 1);
  }

  lastUiSecond = activeElapsedMs() / 1000;
}

void render(bool force = false) {
  // Do not redraw timers or UI into a sleeping LCD controller.
  if (!force && displayPower == DisplayPower::OFF) return;

  if (!force && !screenDirty) {
    if (state == AppState::RECORDING &&
        activeElapsedMs() / 1000 != lastUiSecond) {
      updateRecordingDynamic();
    }
    return;
  }

  screenDirty = false;
  switch (state) {
    case AppState::HOME: drawHome(); break;
    case AppState::MODE_CONFIRM: drawModeConfirm(); break;
    case AppState::RECORDING: drawRecording(); break;
    case AppState::PAUSED: drawPaused(); break;
    case AppState::SAVING: drawSaving(); break;
    case AppState::FINISHED: drawFinished(); break;
    case AppState::MENU: drawMenu(); break;
    case AppState::STATUS: drawStatus(); break;
    case AppState::DETAILS: drawDetails(); break;
    case AppState::SYNC: drawSync(); break;
    case AppState::ERROR: drawError(); break;
  }
}

bool ensureStorage() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, SD_HZ)) return false;
  if (SD.cardType() == CARD_NONE) return false;
  if (!SD.exists("/visitescribe")) SD.mkdir("/visitescribe");
  return true;
}

bool ensureTouchController() {
  if (M5.In_I2C.scanID(FT6336_ADDR, 100000)) return true;
  // CoreS3-Lite: FT6336 RESET = AW9523 P0_0. Re-assert it if needed.
  M5.In_I2C.bitOff(AW9523_ADDR, 0x04, 0x01, I2C_HZ); // P0_0 = output
  M5.In_I2C.bitOff(AW9523_ADDR, 0x02, 0x01, I2C_HZ); // reset low
  delay(10);
  M5.In_I2C.bitOn(AW9523_ADDR, 0x02, 0x01, I2C_HZ);  // reset high
  delay(120);
  return M5.In_I2C.scanID(FT6336_ADDR, 100000);
}

bool readDirectTouch(int& x, int& y) {
  uint8_t b[5] = {0};
  if (!M5.In_I2C.readRegister(FT6336_ADDR, 0x02, b, sizeof(b), I2C_HZ)) return false;
  if ((b[0] & 0x0F) == 0) return false;
  x = ((b[1] & 0x0F) << 8) | b[2];
  y = ((b[3] & 0x0F) << 8) | b[4];
  if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return false;
  return true;
}

uint16_t findNextSessionId() {
  for (uint16_t i = 1; i < 65000; ++i) {
    char p[64];
    snprintf(p, sizeof(p), "/visitescribe/s%05u_events.csv", i);
    if (!SD.exists(p)) return i;
  }
  return 1;
}

void writeHeader(File& f, uint32_t bytes) {
  WAVHeader h;
  h.fileSize = 36 + bytes;
  h.dataSize = bytes;
  f.seek(0);
  f.write(reinterpret_cast<const uint8_t*>(&h), sizeof(h));
  f.flush();
}

void makeAudioPaths() {
  const char* tag = selectedMode == Mode::VISIT ? "visit" : (selectedMode == Mode::ROUND ? "round" : "meeting");
  const bool patientFiles =
      selectedMode == Mode::ROUND || (selectedMode == Mode::VISIT && visitPatientFlow);
  if (patientFiles) {
    snprintf(wavFinalPath, sizeof(wavFinalPath), "/visitescribe/s%05u_%s_p%03u_s%02u.wav", sessionId, tag, patientNumber, segmentNumber);
  } else if (segmentNumber <= 1) {
    snprintf(wavFinalPath, sizeof(wavFinalPath), "/visitescribe/s%05u_%s.wav", sessionId, tag);
  } else {
    snprintf(wavFinalPath, sizeof(wavFinalPath), "/visitescribe/s%05u_%s_add%02u.wav", sessionId, tag, segmentNumber - 1);
  }
  snprintf(wavTmpPath, sizeof(wavTmpPath), "%s.tmp", wavFinalPath);
}

bool openWavSegment() {
#ifdef VISITESCRIBE_DIRECT_OPUS
  audioError = false;
  return vsDirectOpusBeginSegment();
#else
  makeAudioPaths();
  if (SD.exists(wavTmpPath)) SD.remove(wavTmpPath);
  wavFile = SD.open(wavTmpPath, FILE_WRITE);
  if (!wavFile) return false;
  WAVHeader h;
  if (wavFile.write(reinterpret_cast<const uint8_t*>(&h), sizeof(h)) != sizeof(h)) {
    wavFile.close();
    return false;
  }
  wavDataBytes = 0;
  audioError = false;
  return true;
#endif
}

void finalizeWavSegment() {
#ifdef VISITESCRIBE_DIRECT_OPUS
  if (!vsDirectOpusFinishSegment()) audioError = true;
#else
  if (!wavFile) return;
  writeHeader(wavFile, wavDataBytes);
  wavFile.close();
  if (SD.exists(wavFinalPath)) SD.remove(wavFinalPath);
  if (!SD.rename(wavTmpPath, wavFinalPath)) audioError = true;
#endif
}

void logEvent(const char* eventName, uint32_t offsetMs) {
  if (!sdOk || eventsPath[0] == '\0') return;
#ifdef VISITESCRIBE_DIRECT_OPUS
  if (!vsDirectOpusLockSd()) return;
#endif
  File f = SD.open(eventsPath, FILE_APPEND);
  if (f) {
    f.printf("%lu,%s,%u,%u,%u,%s\n",
             (unsigned long)offsetMs, eventName,
             patientNumber, segmentNumber, markerCount, wavFinalPath);
    f.close();
  }
#ifdef VISITESCRIBE_DIRECT_OPUS
  vsDirectOpusUnlockSd();
#endif
}

void audioReleased(void*, void* data, size_t length) {
  if (!audioDoneQueue) return;
  AudioDone done{static_cast<int16_t*>(data), length};
  xQueueSend(audioDoneQueue, &done, 0);
}

bool queueAudio(int16_t* buffer) {
  return M5.Mic.record(buffer, AUDIO_BLOCK_SAMPLES, AUDIO_RATE, true);
}

void serviceAudio() {
  if (!audioDoneQueue) return;
  AudioDone done;
  while (xQueueReceive(audioDoneQueue, &done, 0) == pdTRUE) {
#ifdef VISITESCRIBE_DIRECT_OPUS
    if (done.data && done.samples) {
      if (!vsDirectOpusConsumeStereo(done.data, done.samples)) {
        audioError = true;
      }
    }
#else
    if (wavFile && done.data && done.samples) {
      size_t bytes = done.samples * sizeof(int16_t);
      size_t written = wavFile.write(reinterpret_cast<uint8_t*>(done.data), bytes);
      if (written != bytes) audioError = true;
      wavDataBytes += written;
    }
#endif
    if (captureRunning && done.data && !queueAudio(done.data)) {
      audioError = true;
    }
  }
}

bool startCapture() {
#ifdef VISITESCRIBE_DIRECT_OPUS
  if (!vsDirectOpusReady()) {
    audioError = true;
    return false;
  }
#else
  if (!wavFile) return false;
#endif

  while (audioDoneQueue && uxQueueMessagesWaiting(audioDoneQueue)) {
    AudioDone d;
    xQueueReceive(audioDoneQueue, &d, 0);
  }

  M5.Speaker.end();
  auto cfg = M5.Mic.config();
  cfg.sample_rate = AUDIO_RATE;
  cfg.input_channel = m5::input_stereo;
  cfg.over_sampling = 1;
  cfg.noise_filter_level = 0;
#ifdef VISITESCRIBE_DIRECT_OPUS
  // Keep microphone DMA/capture above the encoder. Opus gets the same core at
  // low priority so capture always wins if both become runnable together.
  cfg.task_pinned_core = VS_DO_MIC_CORE;
  cfg.task_priority = VS_DO_MIC_PRIORITY;
#endif
  M5.Mic.config(cfg);
  M5.Mic.setBufferReleaseCallback(nullptr, audioReleased);

  if (!M5.Mic.begin()) {
    audioError = true;
    return false;
  }

  captureRunning = true;
  if (!queueAudio(audioBufA) || !queueAudio(audioBufB)) {
    captureRunning = false;
    audioError = true;
    M5.Mic.end();
    return false;
  }
  return true;
}

void stopCapture() {
  captureRunning = false;
  uint32_t deadline = millis() + 1000;
  while (M5.Mic.isRecording() && (int32_t)(deadline - millis()) > 0) {
    serviceAudio();
    delay(1);
  }
  M5.Mic.end();
  serviceAudio();
}

static bool discardCurrentFalseStart() {
#ifdef VISITESCRIBE_DIRECT_OPUS
  return vsDirectOpusDiscardSession(sessionId);
#else
  bool ok = true;
  if (wavFile) wavFile.close();
  if (wavTmpPath[0] && SD.exists(wavTmpPath) && !SD.remove(wavTmpPath)) ok = false;
  if (wavFinalPath[0] && SD.exists(wavFinalPath) && !SD.remove(wavFinalPath)) ok = false;
  if (eventsPath[0] && SD.exists(eventsPath) && !SD.remove(eventsPath)) ok = false;

  char syncPath[96];
  snprintf(syncPath, sizeof(syncPath), "/visitescribe/s%05u_sync.txt", sessionId);
  if (SD.exists(syncPath) && !SD.remove(syncPath)) ok = false;
  return ok;
#endif
}

bool startNewSession(Mode mode) {
  if (!sdOk) return false;
  selectedMode = mode;
  sessionId = findNextSessionId();
#ifdef VISITESCRIBE_DIRECT_OPUS
  vsDirectOpusResetSession(sessionId);
#endif
  patientNumber = 1;
  segmentNumber = 1;
  markerCount = 0;
  patientSegmentStartOffsetMs = 0;
  recordingToast[0] = '\0';
  recordingToastUntilMs = 0;
  lastSessionFalseStart = false;
  lastSessionSaved = false;
  uiErrorTitle = "";
  uiErrorDetail = "";
  totalPausedMs = 0;
  pauseStartedMs = 0;
  sessionStartedMs = millis();
  sessionOpen = true;
  snprintf(eventsPath, sizeof(eventsPath), "/visitescribe/s%05u_events.csv", sessionId);
  if (SD.exists(eventsPath)) SD.remove(eventsPath);
  File events = SD.open(eventsPath, FILE_WRITE);
  if (!events) {
    sessionOpen = false;
    uiErrorTitle = "Opnemen niet mogelijk";
    uiErrorDetail = "Kan opslag niet openen";
    return false;
  }
  events.println("elapsed_ms,event,patient,segment,markers,audio_file");
  events.close();

  if (!openWavSegment()) {
    sessionOpen = false;
    uiErrorTitle = "Opnemen niet mogelijk";
    uiErrorDetail = "Audiobestand kon niet starten";
    return false;
  }
  logEvent("session_started", 0);
  if (!startCapture()) {
    finalizeWavSegment();
    sessionOpen = false;
    uiErrorTitle = "Opnemen niet mogelijk";
    uiErrorDetail = "Microfoon kon niet starten";
    return false;
  }
  state = AppState::RECORDING;
  screenDirty = true;
  return true;
}

bool startQuickSession() {
  // Capture starts immediately. PATIENT is the default; the chooser remains
  // visible for ten seconds unless the user explicitly changes the selection.
  visitPatientFlow = true;
  quickModeChoiceActive = true;
  quickChoice = QuickChoice::PATIENT;
  quickModeChoiceStartedMs = millis();
  quickModeChoiceDeadlineMs =
      quickModeChoiceStartedMs + QUICK_MODE_CHOICE_MS;

  if (!startNewSession(Mode::VISIT)) {
    quickModeChoiceActive = false;
    visitPatientFlow = false;
    quickModeChoiceStartedMs = 0;
    quickModeChoiceDeadlineMs = 0;
    return false;
  }
  return true;
}

void selectQuickMode(Mode mode) {
  if (!quickModeChoiceActive || !sessionOpen) return;

  if (mode == Mode::MEETING) {
    selectedMode = Mode::MEETING;
    visitPatientFlow = false;
    quickChoice = QuickChoice::MEETING;
  } else {
    selectedMode = Mode::VISIT;
    visitPatientFlow = true;
    quickChoice = QuickChoice::PATIENT;
  }

  // An explicit choice stays on screen for a few seconds. This makes touch
  // selection feel confirmed rather than disappearing immediately, and still
  // gives the user time to switch back.
  quickModeChoiceDeadlineMs =
      millis() + QUICK_MODE_EXPLICIT_DWELL_MS;
  lastUserActivityMs = millis();
  screenDirty = true;
}

void selectQuickStop() {
  if (!quickModeChoiceActive || !sessionOpen) return;
  quickChoice = QuickChoice::STOP;
  quickModeChoiceDeadlineMs =
      millis() + QUICK_MODE_STOP_DWELL_MS;
  lastUserActivityMs = millis();
  screenDirty = true;
}

void cycleQuickChoice() {
  if (!quickModeChoiceActive || !sessionOpen) return;

  if (quickChoice == QuickChoice::PATIENT) {
    selectQuickMode(Mode::MEETING);
  } else if (quickChoice == QuickChoice::MEETING) {
    selectQuickStop();
  } else {
    selectQuickMode(Mode::VISIT);
  }
}

static void commitQuickRecordingMode() {
  if (!quickModeChoiceActive || !sessionOpen) return;

  if (selectedMode == Mode::MEETING) {
#ifndef VISITESCRIBE_DIRECT_OPUS
    // The WAV backend starts before the type choice, so only its final rename
    // target changes. Direct Opus chunks use mode-neutral chunk names.
    snprintf(wavFinalPath, sizeof(wavFinalPath),
             "/visitescribe/s%05u_meeting.wav", sessionId);
#endif
    logEvent("mode_selected_meeting", activeElapsedMs());
  } else {
    selectedMode = Mode::VISIT;
    visitPatientFlow = true;
    logEvent("mode_selected_visit", activeElapsedMs());
  }

  quickModeChoiceActive = false;
  quickModeChoiceStartedMs = 0;
  quickModeChoiceDeadlineMs = 0;
  lastUserActivityMs = millis();
  screenDirty = true;
}

void serviceQuickModeChoiceTimeout() {
  if (!quickModeChoiceActive) return;
  if (static_cast<int32_t>(quickModeChoiceDeadlineMs - millis()) > 0) return;

  const bool stopSelected = quickChoice == QuickChoice::STOP;
  commitQuickRecordingMode();

  if (stopSelected && sessionOpen) {
    stopSession();
  }
}

void togglePrivacy() {
  if (state == AppState::RECORDING) {
    uint32_t off = activeElapsedMs();
    stopCapture();
    pauseStartedMs = millis();
    state = AppState::PAUSED;
    logEvent("privacy_pause_started", off);
  } else if (state == AppState::PAUSED) {
    uint32_t off = activeElapsedMs();
    totalPausedMs += millis() - pauseStartedMs;
    pauseStartedMs = 0;
    if (!startCapture()) audioError = true;
    state = AppState::RECORDING;
    logEvent("privacy_pause_ended", off);
  }
  screenDirty = true;
}

void addMarkerOrNext() {
  const uint32_t off = activeElapsedMs();
  const bool patientMode =
      selectedMode == Mode::ROUND ||
      (selectedMode == Mode::VISIT && visitPatientFlow);

  if (patientMode) {
    if (state == AppState::RECORDING) stopCapture();
    logEvent("patient_boundary", off);
    finalizeWavSegment();

    if (audioError) {
      sessionOpen = false;
      uiErrorTitle = "Opname onderbroken";
      uiErrorDetail = "Opslaan van patientsegment mislukt";
      state = AppState::ERROR;
      screenDirty = true;
      return;
    }

    ++patientNumber;
    segmentNumber = 1;
    patientSegmentStartOffsetMs = off;
    if (!openWavSegment() || !startCapture()) {
      audioError = true;
      sessionOpen = false;
      uiErrorTitle = "Opname onderbroken";
      uiErrorDetail = "Nieuwe patient kon niet starten";
      state = AppState::ERROR;
      screenDirty = true;
      return;
    }

    state = AppState::RECORDING;
    logEvent("patient_started", off);
    char toast[48];
    snprintf(toast, sizeof(toast), "Patiënt %u gestart", patientNumber);
    setRecordingToast(toast);
  } else {
    ++markerCount;
    logEvent("marker", off);
    char toast[48];
    snprintf(toast, sizeof(toast), "Markering %u geplaatst", markerCount);
    setRecordingToast(toast);
  }
  screenDirty = true;
}

void stopSession() {
  if (!sessionOpen) return;

  const uint32_t off = activeElapsedMs();
  const bool paused = state == AppState::PAUSED;

  if (!paused) stopCapture();

  // Make the local-finalization state explicit before the blocking encoder/SD
  // close. PWR/touch handlers ignore SAVING.
  state = AppState::SAVING;
  screenDirty = true;
  render(true);

  logEvent("session_stopped", off);
  finalizeWavSegment();
  if (paused) {
    totalPausedMs += millis() - pauseStartedMs;
    pauseStartedMs = 0;
  }

  sessionOpen = false;
  quickModeChoiceActive = false;
  quickModeChoiceStartedMs = 0;
  quickModeChoiceDeadlineMs = 0;

  if (off < FALSE_START_LIMIT_MS) {
    const bool removed = discardCurrentFalseStart();
    if (!removed) {
      uiErrorTitle = "Verwijderen niet gelukt";
      uiErrorDetail = "Valse start staat mogelijk nog op opslag";
      state = AppState::ERROR;
      screenDirty = true;
      return;
    }

    audioError = false;
    lastSessionFalseStart = true;
    lastSessionSaved = false;
    finishedAtMs = millis();
    state = AppState::FINISHED;
    screenDirty = true;
    Serial.printf(
        "RECORDER: false start s%05u discarded duration=%lums\n",
        sessionId, (unsigned long)off);
    return;
  }

  if (audioError) {
    uiErrorTitle = "Opslaan niet gelukt";
    uiErrorDetail = "Controleer de opslag bij Apparaatstatus";
    state = AppState::ERROR;
    screenDirty = true;
    return;
  }

  lastSessionFalseStart = false;
  lastSessionSaved = true;
  finishedAtMs = millis();
  state = AppState::FINISHED;
  screenDirty = true;
}


void handleActiveRecordingError() {
  if (state != AppState::RECORDING || !audioError) return;

  const uint32_t off = activeElapsedMs();
  stopCapture();

  state = AppState::SAVING;
  screenDirty = true;
  render(true);

  logEvent("recording_error", off);
  finalizeWavSegment();

  sessionOpen = false;
  quickModeChoiceActive = false;
  quickModeChoiceStartedMs = 0;
  uiErrorTitle = "Opname onderbroken";
  uiErrorDetail = "Audio- of opslagfout gedetecteerd";
  state = AppState::ERROR;
  screenDirty = true;

  Serial.printf(
      "RECORDER: active recording error s%05u duration=%lums\n",
      sessionId, (unsigned long)off);
}

void resumeSession() {
  if (!sessionOpen || state != AppState::FINISHED) return;
  totalPausedMs += millis() - finishedAtMs;
  finishedAtMs = 0;
  ++segmentNumber;
  if (!openWavSegment() || !startCapture()) audioError = true;
  state = AppState::RECORDING;
  logEvent("session_resumed", activeElapsedMs());
  screenDirty = true;
}

void goHome() {
  if (captureRunning) stopCapture();
  sessionOpen = false;
  quickModeChoiceActive = false;
  visitPatientFlow = false;
  quickChoice = QuickChoice::PATIENT;
  quickModeChoiceStartedMs = 0;
  quickModeChoiceDeadlineMs = 0;
  recordingToast[0] = '\0';
  recordingToastUntilMs = 0;
  eventsPath[0] = wavTmpPath[0] = wavFinalPath[0] = '\0';
  sessionStartedMs = pauseStartedMs = totalPausedMs = finishedAtMs = 0;
  patientSegmentStartOffsetMs = 0;
  patientNumber = segmentNumber = 1;
  markerCount = 0;
  state = AppState::HOME;
  lastUserActivityMs = millis();
  screenDirty = true;
}

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
  WiFi.begin(index == 0 ? VISITESCRIBE_WIFI_SSID_1 : VISITESCRIBE_WIFI_SSID_2,
             index == 0 ? VISITESCRIBE_WIFI_PASSWORD_1 : VISITESCRIBE_WIFI_PASSWORD_2);
  syncPhase = index == 0 ? SyncPhase::CONNECTING_1 : SyncPhase::CONNECTING_2;
  syncAttemptStartedMs = millis();
#else
  (void)index;
  syncPhase = SyncPhase::NO_CREDENTIALS;
#endif
  screenDirty = true;
}

void beginSync() {
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
  if (syncPhase != SyncPhase::CONNECTING_1 && syncPhase != SyncPhase::CONNECTING_2) return;
  if (WiFi.status() == WL_CONNECTED) { syncPhase = SyncPhase::CONNECTED; screenDirty = true; return; }
  if (millis() - syncAttemptStartedMs >= WIFI_ATTEMPT_MS) {
    if (syncPhase == SyncPhase::CONNECTING_1) startWifiAttempt(1);
    else { wifiOff(); syncPhase = SyncPhase::FAILED; screenDirty = true; }
  }
}

void noteActivity() {
  lastUserActivityMs = millis();
  if (displayPower != DisplayPower::ACTIVE) {
    if (displayPower == DisplayPower::OFF) M5.Display.wakeup();
    M5.Display.setBrightness(BRIGHTNESS_ACTIVE);
    displayPower = DisplayPower::ACTIVE;
    screenDirty = true;
  }
}

bool wakeOnlyIfOff() {
  if (displayPower != DisplayPower::OFF) return false;
  M5.Display.wakeup();
  M5.Display.setBrightness(BRIGHTNESS_ACTIVE);
  displayPower = DisplayPower::ACTIVE;
  lastUserActivityMs = millis();
  screenDirty = true;
  return true;
}

void handleTouch(int x, int y) {
  switch (state) {
    case AppState::HOME:
      // Pocket-first HOME deliberately ignores touch.
      break;
    case AppState::MODE_CONFIRM:
      if (TWO_TOP.contains(x,y)) startNewSession(selectedMode);
      else if (TWO_BOTTOM.contains(x,y)) goHome();
      break;
    case AppState::RECORDING:
    case AppState::PAUSED:
    case AppState::SAVING:
    case AppState::FINISHED:
      // Active/saved pocket flow is PWR-only.
      break;
    case AppState::MENU:
      if (THREE_TOP.contains(x,y)) beginSync();
      else if (THREE_MIDDLE.contains(x,y)) {
        refreshBattery();
        state = AppState::STATUS;
        screenDirty = true;
      } else if (THREE_BOTTOM.contains(x,y)) {
        goHome();
      }
      break;
    case AppState::STATUS:
      if (STATUS_DETAILS.contains(x,y)) {
        state = AppState::DETAILS;
        lastUserActivityMs = millis();
        screenDirty = true;
      } else if (STATUS_BACK.contains(x,y)) {
        state = AppState::MENU;
        lastUserActivityMs = millis();
        screenDirty = true;
      }
      break;
    case AppState::DETAILS:
      if (STATUS_BACK.contains(x,y)) {
        state = AppState::STATUS;
        lastUserActivityMs = millis();
        screenDirty = true;
      }
      break;
    case AppState::SYNC:
      if (vsSyncTouchLockedHook && vsSyncTouchLockedHook()) break;
      if (SYNC_RETRY.contains(x,y) &&
          (!vsSyncRetryAllowedHook || vsSyncRetryAllowedHook())) {
        startWifiAttempt(0);
      } else if (SYNC_BACK.contains(x,y)) {
        wifiOff();
        syncPhase = SyncPhase::NOT_STARTED;
        goHome();
      }
      break;
    case AppState::ERROR:
      if (STATUS_DETAILS.contains(x,y)) {
        refreshBattery();
        state = AppState::STATUS;
        screenDirty = true;
      } else if (STATUS_BACK.contains(x,y)) {
        goHome();
      }
      break;
  }
}

void handlePowerButton() {
  switch (state) {
    case AppState::HOME: startNewSession(Mode::VISIT); break;
    case AppState::MODE_CONFIRM: startNewSession(selectedMode); break;
    case AppState::RECORDING:
    case AppState::PAUSED: stopSession(); break;
    case AppState::FINISHED: resumeSession(); break;
    default: break;
  }
}

void serviceInputs() {
  // Poll AXP2101 before M5.update(), because reading PEK status clears the IRQ status bits.
  uint8_t pek = 0;
  if (M5.Power.getType() == m5::Power_Class::pmic_axp2101) pek = M5.Power.Axp2101.getPekPress();

  int tx = 0, ty = 0;
  bool touchDown = touchOk && readDirectTouch(tx, ty);

  if ((pek & 0x02) != 0) {
    if (!wakeOnlyIfOff()) { noteActivity(); if (sdOk) handlePowerButton(); }
  }

  if (touchDown && !touchWasDown) {
    if (!wakeOnlyIfOff()) { noteActivity(); if (sdOk) handleTouch(tx, ty); }
  }
  touchWasDown = touchDown;

  // Keep M5Unified housekeeping alive for power/battery/audio internals, but its
  // synthesized PWR button is disabled in setup; our input path above is authoritative.
  M5.update();
}

void serviceDisplayPower() {
  // Sync is intentionally always visible. A user looking at upload progress
  // must never need to wake the display just to discover whether it finished.
  if (state == AppState::SYNC) return;

  // Keep the entire ten-second type chooser awake and touchable.
  if (quickModeChoiceActive) return;

  // SAVING and blocking errors stay readable while the local result is being
  // established. FINISHED auto-navigates before its normal dim timeout.
  if (state == AppState::SAVING || state == AppState::ERROR) return;

  const uint32_t idle = millis() - lastUserActivityMs;
  const bool recording =
      state == AppState::RECORDING || state == AppState::PAUSED;
  const bool menuLike =
      state == AppState::MENU ||
      state == AppState::STATUS ||
      state == AppState::DETAILS;
  const bool homeLike =
      state == AppState::HOME || state == AppState::FINISHED;

  // Validated starting values from the UX brief, with the user's explicit
  // correction that recording must not dim/go dark after only a few seconds.
  const uint32_t dimMs = recording ? 10000UL :
      (menuLike ? 15000UL : (homeLike ? 15000UL : 15000UL));
  const uint32_t offMs = recording ? 30000UL :
      (menuLike ? 30000UL : (homeLike ? 30000UL : 30000UL));

  if (idle >= offMs && displayPower != DisplayPower::OFF) {
    M5.Display.sleep();
    displayPower = DisplayPower::OFF;
  } else if (idle >= dimMs && displayPower == DisplayPower::ACTIVE) {
    M5.Display.setBrightness(BRIGHTNESS_DIM);
    displayPower = DisplayPower::DIMMED;
  }
}

void setup() {
  Serial.begin(115200);
  delay(250);

  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StackCoreS3;
  cfg.pmic_button = false;       // direct AXP2101 handling in serviceInputs()
  cfg.internal_mic = true;
  cfg.internal_spk = true;
  M5.begin(cfg);

  M5.Display.setRotation(1);
  M5.Display.setBrightness(BRIGHTNESS_ACTIVE);
  M5.Display.fillScreen(C_BG);
  M5.Speaker.end();

  showBrianBootAnimation();
  M5.Display.fillScreen(C_BG);

  audioDoneQueue = xQueueCreate(4, sizeof(AudioDone));
  touchOk = ensureTouchController();
  sdOk = ensureStorage();
  refreshBattery();
  wifiOff();
  lastUserActivityMs = millis();

#ifdef VISITESCRIBE_DIRECT_OPUS
  Serial.println("VisiteScribe CoreS3-Lite DIRECT OGG/OPUS TEST");
#else
  Serial.println("VisiteScribe CoreS3-Lite v0.2");
#endif
  Serial.printf("board=%d pmic=%d touch=%s sd=%s battery=%d%%\n",
                (int)M5.getBoard(), (int)M5.Power.getType(), touchOk ? "OK" : "FAIL", sdOk ? "OK" : "FAIL", batteryPct);

  if (!sdOk) {
    drawHeader("MICROSD FOUT");
    centeredText(110, "Plaats FAT32 microSD en reset", C_RED, 2);
  } else {
    render(true);
  }
}

void loop() {
  serviceInputs();
  serviceAudio();
  serviceSync();

  if (millis() - lastBatteryRefreshMs > 10000) {
    int old = batteryPct;
    refreshBattery();
    if (old != batteryPct && state == AppState::STATUS) screenDirty = true;
  }

  if (state == AppState::FINISHED && millis() - finishedAtMs >= FINISHED_AUTO_HOME_MS) goHome();

  render();
  serviceDisplayPower();
  delay(5);
}
