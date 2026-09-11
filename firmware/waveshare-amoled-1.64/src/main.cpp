#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

#ifndef VISITESCRIBE_BOARD_REV
#define VISITESCRIBE_BOARD_REV 2
#endif

// Waveshare ESP32-S3-Touch-AMOLED-1.64
// V2 official Waveshare pinout uses LCD CS=46.
// V1 / older Arduino_GFX examples use LCD CS=9.
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

static constexpr int TOUCH_SDA = 47;
static constexpr int TOUCH_SCL = 48;
static constexpr uint8_t TOUCH_ADDR = 0x38;

// V2 official Waveshare SD/SPI pinout.
static constexpr int SD_MISO = 40;
static constexpr int SD_MOSI = 39;
static constexpr int SD_SCLK = 41;
static constexpr int SD_CS = 38;

// GPIO0 is the physical BOOT button. Do not call this BOOT_PIN: Arduino-ESP32
// 3.x already defines BOOT_PIN in esp32-hal.h.
static constexpr int VISITESCRIBE_BOOT_GPIO = 0;

static constexpr uint16_t C_BG      = 0x0000;
static constexpr uint16_t C_PANEL   = 0x1082;
static constexpr uint16_t C_PANEL2  = 0x18E3;
static constexpr uint16_t C_WHITE   = 0xFFFF;
static constexpr uint16_t C_MUTED   = 0x9CF3;
static constexpr uint16_t C_GREEN   = 0x4FE9;
static constexpr uint16_t C_RED     = 0xF986;
static constexpr uint16_t C_AMBER   = 0xFD20;
static constexpr uint16_t C_BLUE    = 0x3DDF;
static constexpr uint16_t C_CYAN    = 0x4FFF;

// Stronger full-screen home colours for the AMOLED.
static constexpr uint16_t C_HOME_HEADER  = 0x0841;
static constexpr uint16_t C_HOME_VISIT   = 0x149F;
static constexpr uint16_t C_HOME_ROUND   = 0x04B4;
static constexpr uint16_t C_HOME_MEETING = 0x2589;
static constexpr uint16_t C_HOME_DIVIDER = 0xFFFF;

Arduino_DataBus *displayBus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_GFX *gfx = new Arduino_CO5300(
    displayBus,
    LCD_RST,
    0,
    SCREEN_W,
    SCREEN_H,
    20,
    0,
    180,
    24);

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

// Home screen: a compact 48px title bar plus exactly three equal 136px
// touch areas that consume the rest of the 280x456 display.
static const Rect HOME_VISIT  {0, 48, 280, 136};
static const Rect HOME_ROUND  {0, 184, 280, 136};
static const Rect HOME_MEETING{0, 320, 280, 136};

static const Rect CONF_START  {18, 282, 244, 66};
static const Rect CONF_BACK   {18, 362, 244, 50};
static const Rect REC_PRIVACY {16, 326, 120, 62};
static const Rect REC_MARKER  {144,326, 120, 62};
static const Rect REC_STOP    {16, 400, 248, 42};
static const Rect DONE_BACK   {18, 350, 244, 58};

enum class AppState : uint8_t {
  HOME,
  CONFIRM,
  RECORDING,
  PAUSED,
  FINISHED
};

enum class Mode : uint8_t {
  VISIT,
  ROUND,
  MEETING
};

AppState state = AppState::HOME;
Mode selectedMode = Mode::VISIT;

bool sdOk = false;
bool touchWasDown = false;
bool screenDirty = true;
uint32_t lastFrameMs = 0;
uint32_t sessionStartedMs = 0;
uint32_t pauseStartedMs = 0;
uint32_t totalPausedMs = 0;
uint32_t finishedAtMs = 0;
uint32_t bootPressedAtMs = 0;
bool bootWasDown = false;
uint16_t markerCount = 0;
uint16_t patientNumber = 1;
char currentLogPath[96] = {0};

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

void textAt(int x, int y, const char *text, uint16_t color, uint8_t size = 1) {
  gfx->setTextColor(color);
  gfx->setTextSize(size);
  gfx->setCursor(x, y);
  gfx->print(text);
}

void centered(int y, const char *text, uint16_t color, uint8_t size = 1) {
  int width = (int)strlen(text) * 6 * size;
  int x = (SCREEN_W - width) / 2;
  if (x < 2) x = 2;
  textAt(x, y, text, color, size);
}

void button(const Rect &r, const char *label, uint16_t fill, uint16_t border, uint16_t textColor, uint8_t textSize = 2) {
  gfx->fillRoundRect(r.x, r.y, r.w, r.h, 12, fill);
  gfx->drawRoundRect(r.x, r.y, r.w, r.h, 12, border);
  int width = (int)strlen(label) * 6 * textSize;
  int x = r.x + (r.w - width) / 2;
  int y = r.y + (r.h - 8 * textSize) / 2;
  textAt(x, y, label, textColor, textSize);
}

void drawHeader(const char *right = nullptr) {
  textAt(16, 14, "VisiteScribe", C_WHITE, 2);
  textAt(16, 37, "MINI  prototype", C_MUTED, 1);
  if (right) {
    int width = (int)strlen(right) * 6;
    textAt(SCREEN_W - 16 - width, 19, right, C_MUTED, 1);
  }
  gfx->drawFastHLine(16, 57, 248, C_PANEL2);
}

void drawHomeMode(const Rect &r, const char *label, const char *subtitle, uint16_t fill) {
  gfx->fillRect(r.x, r.y, r.w, r.h, fill);
  gfx->drawFastHLine(0, r.y, SCREEN_W, C_HOME_DIVIDER);

  int labelWidth = (int)strlen(label) * 18;  // 6 px glyph width * size 3
  int labelX = (SCREEN_W - labelWidth) / 2;
  textAt(labelX, r.y + 43, label, C_WHITE, 3);

  int subWidth = (int)strlen(subtitle) * 6;
  int subX = (SCREEN_W - subWidth) / 2;
  textAt(subX, r.y + 82, subtitle, C_WHITE, 1);
}

void drawHome() {
  gfx->fillScreen(C_BG);

  // Compact title bar. The title remains geometrically centered even with
  // the tiny SD status at the right edge.
  gfx->fillRect(0, 0, SCREEN_W, 48, C_HOME_HEADER);
  centered(16, "VisiteScribe", C_CYAN, 2);
  textAt(244, 20, sdOk ? "SD" : "--", sdOk ? C_GREEN : C_AMBER, 1);

  drawHomeMode(HOME_VISIT, "VISITE", "1 patient", C_HOME_VISIT);
  drawHomeMode(HOME_ROUND, "PATIENTRONDE", "meerdere patienten", C_HOME_ROUND);
  drawHomeMode(HOME_MEETING, "VERGADERING", "overleg / bespreking", C_HOME_MEETING);
  gfx->drawFastHLine(0, SCREEN_H - 1, SCREEN_W, C_HOME_DIVIDER);
}

void drawConfirm() {
  gfx->fillScreen(C_BG);
  drawHeader(sdOk ? "SD OK" : "SD --");
  centered(88, modeTitle(selectedMode), C_WHITE, 2);
  centered(134, "DEMO-OPNAME", C_AMBER, 2);
  centered(176, "Er wordt nog geen audio opgenomen.", C_MUTED, 1);
  centered(194, "Wel testen we touch, workflow", C_MUTED, 1);
  centered(210, "en opslag op microSD.", C_MUTED, 1);
  button(CONF_START, "START DEMO", C_GREEN, C_GREEN, C_BG, 2);
  button(CONF_BACK, "TERUG", C_PANEL, C_MUTED, C_WHITE, 2);
}

void formatElapsed(char *out, size_t len) {
  uint32_t seconds = activeElapsedMs() / 1000;
  uint32_t h = seconds / 3600;
  uint32_t m = (seconds % 3600) / 60;
  uint32_t s = seconds % 60;
  snprintf(out, len, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
}

void drawVuDemo() {
  uint32_t seed = esp_random();
  int baseY = 224;
  int maxH = 52;
  int gap = 5;
  int barW = 7;
  int bars = 22;
  int totalW = bars * barW + (bars - 1) * gap;
  int startX = (SCREEN_W - totalW) / 2;

  gfx->fillRect(0, 164, SCREEN_W, 112, C_BG);
  centered(168, "DEMO - GEEN AUDIO", C_AMBER, 1);

  for (int i = 0; i < bars; ++i) {
    seed = seed * 1664525UL + 1013904223UL;
    int h = 6 + ((seed >> 24) % maxH);
    int x = startX + i * (barW + gap);
    uint16_t c = h > 42 ? C_AMBER : C_GREEN;
    gfx->fillRoundRect(x, baseY - h / 2, barW, h, 3, c);
  }
}

void drawRecording() {
  gfx->fillScreen(C_BG);
  drawHeader("REC DEMO");

  char elapsed[16];
  formatElapsed(elapsed, sizeof(elapsed));
  centered(78, modeTitle(selectedMode), C_WHITE, 2);
  centered(118, elapsed, C_WHITE, 3);

  if (selectedMode == Mode::ROUND) {
    char p[32];
    snprintf(p, sizeof(p), "PATIENT %u", patientNumber);
    centered(286, p, C_CYAN, 2);
  } else {
    char m[32];
    snprintf(m, sizeof(m), "MARKERS %u", markerCount);
    centered(286, m, C_MUTED, 2);
  }

  button(REC_PRIVACY, "PRIVACY", C_PANEL, C_AMBER, C_WHITE, 1);
  button(REC_MARKER, selectedMode == Mode::ROUND ? "VOLGENDE" : "MARKER", C_PANEL, C_CYAN, C_WHITE, 1);
  button(REC_STOP, "STOP - lang BOOT kan ook", C_RED, C_RED, C_WHITE, 1);
  drawVuDemo();
}

void drawPaused() {
  gfx->fillScreen(C_BG);
  drawHeader("PAUZE");
  centered(92, "PRIVACY PAUZE", C_AMBER, 2);
  centered(137, "GEEN AUDIO", C_RED, 3);
  centered(190, "In de echte recorder staat", C_MUTED, 1);
  centered(207, "de microfoon hier fysiek stil.", C_MUTED, 1);

  char elapsed[16];
  formatElapsed(elapsed, sizeof(elapsed));
  centered(251, elapsed, C_WHITE, 2);

  button(REC_PRIVACY, "HERVAT", C_GREEN, C_GREEN, C_BG, 1);
  button(REC_MARKER, selectedMode == Mode::ROUND ? "VOLGENDE" : "MARKER", C_PANEL, C_CYAN, C_WHITE, 1);
  button(REC_STOP, "STOP - lang BOOT kan ook", C_RED, C_RED, C_WHITE, 1);
}

void drawFinished() {
  gfx->fillScreen(C_BG);
  drawHeader(sdOk ? "SD OK" : "SD --");
  centered(112, "OPGESLAGEN", C_GREEN, 2);
  centered(153, "DEMO SESSIE KLAAR", C_WHITE, 2);

  char info[48];
  if (selectedMode == Mode::ROUND) {
    snprintf(info, sizeof(info), "%u patienten / %u grenzen", patientNumber, markerCount);
  } else {
    snprintf(info, sizeof(info), "%u markers", markerCount);
  }
  centered(210, info, C_MUTED, 1);
  centered(242, sdOk ? "events staan op microSD" : "geen SD-log geschreven", sdOk ? C_GREEN : C_AMBER, 1);

  button(DONE_BACK, "NIEUWE OPNAME", C_PANEL, C_BLUE, C_WHITE, 1);
}

void render(bool force = false) {
  uint32_t now = millis();
  bool animated = state == AppState::RECORDING;
  if (!force && !screenDirty && (!animated || now - lastFrameMs < 180)) return;

  lastFrameMs = now;
  screenDirty = false;

  switch (state) {
    case AppState::HOME: drawHome(); break;
    case AppState::CONFIRM: drawConfirm(); break;
    case AppState::RECORDING: drawRecording(); break;
    case AppState::PAUSED: drawPaused(); break;
    case AppState::FINISHED: drawFinished(); break;
  }
}

bool readTouch(uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;

  if (Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)5) != 5) return false;
  uint8_t b[5];
  for (int i = 0; i < 5; ++i) b[i] = Wire.read();

  uint8_t touches = b[0] & 0x0F;
  if (touches == 0) return false;

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

bool initSD() {
  sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, 10000000)) return false;
  if (SD.cardType() == CARD_NONE) return false;

  if (!SD.exists("/visitescribe")) SD.mkdir("/visitescribe");
  File boot = SD.open("/visitescribe/boot.log", FILE_APPEND);
  if (boot) {
    boot.printf("boot_ms=%lu board_rev=%d\n", (unsigned long)millis(), VISITESCRIBE_BOARD_REV);
    boot.close();
  }
  return true;
}

void logEvent(const char *eventName) {
  if (!sdOk || currentLogPath[0] == '\0') return;
  File f = SD.open(currentLogPath, FILE_APPEND);
  if (!f) return;
  f.printf("%lu,%s,%u,%u\n",
           (unsigned long)activeElapsedMs(),
           eventName,
           patientNumber,
           markerCount);
  f.flush();
  f.close();
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

  if (sdOk) {
    File f = SD.open(currentLogPath, FILE_WRITE);
    if (f) {
      f.println("elapsed_ms,event,patient,markers");
      f.close();
    }
  }

  state = AppState::RECORDING;
  logEvent("session_started");
  screenDirty = true;
}

void togglePrivacy() {
  if (state == AppState::RECORDING) {
    pauseStartedMs = millis();
    logEvent("privacy_pause_started");
    state = AppState::PAUSED;
  } else if (state == AppState::PAUSED) {
    uint32_t pausedFor = millis() - pauseStartedMs;
    totalPausedMs += pausedFor;
    state = AppState::RECORDING;
    logEvent("privacy_pause_ended");
  }
  screenDirty = true;
}

void addMarker() {
  markerCount++;
  if (selectedMode == Mode::ROUND) {
    patientNumber++;
    logEvent("patient_boundary");
  } else {
    logEvent("marker");
  }
  screenDirty = true;
}

void stopDemo() {
  if (state != AppState::RECORDING && state != AppState::PAUSED) return;
  logEvent("session_stopped");
  state = AppState::FINISHED;
  finishedAtMs = millis();
  screenDirty = true;
}

void selectMode(Mode mode) {
  selectedMode = mode;
  state = AppState::CONFIRM;
  screenDirty = true;
}

void handleTouchPress(uint16_t x, uint16_t y) {
  Serial.printf("touch x=%u y=%u state=%u\n", x, y, (unsigned)state);

  switch (state) {
    case AppState::HOME:
      if (HOME_VISIT.contains(x, y)) selectMode(Mode::VISIT);
      else if (HOME_ROUND.contains(x, y)) selectMode(Mode::ROUND);
      else if (HOME_MEETING.contains(x, y)) selectMode(Mode::MEETING);
      break;

    case AppState::CONFIRM:
      if (CONF_START.contains(x, y)) startDemo();
      else if (CONF_BACK.contains(x, y)) {
        state = AppState::HOME;
        screenDirty = true;
      }
      break;

    case AppState::RECORDING:
    case AppState::PAUSED:
      if (REC_PRIVACY.contains(x, y)) togglePrivacy();
      else if (REC_MARKER.contains(x, y)) addMarker();
      else if (REC_STOP.contains(x, y)) stopDemo();
      break;

    case AppState::FINISHED:
      if (DONE_BACK.contains(x, y)) {
        state = AppState::HOME;
        sessionStartedMs = 0;
        currentLogPath[0] = '\0';
        screenDirty = true;
      }
      break;
  }
}

void pollBootButton() {
  bool down = digitalRead(VISITESCRIBE_BOOT_GPIO) == LOW;
  uint32_t now = millis();

  if (down && !bootWasDown) bootPressedAtMs = now;

  if (down && bootWasDown && bootPressedAtMs != 0 && now - bootPressedAtMs > 1200) {
    if (state == AppState::RECORDING || state == AppState::PAUSED) {
      stopDemo();
    } else {
      state = AppState::HOME;
      screenDirty = true;
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
  Serial.println("VisiteScribe MINI - Waveshare AMOLED UI demo v0.2");
  Serial.printf("Board revision target: V%d\n", VISITESCRIBE_BOARD_REV);

  pinMode(VISITESCRIBE_BOOT_GPIO, INPUT_PULLUP);

  if (!gfx->begin()) {
    Serial.println("ERROR: display init failed");
  }
  gfx->fillScreen(C_BG);

  initTouch();
  sdOk = initSD();
  Serial.printf("microSD: %s\n", sdOk ? "OK" : "NOT FOUND");
  Serial.println("No microphone is present on this board; recording is simulated.");

  render(true);
}

void loop() {
  uint16_t x = 0, y = 0;
  bool touchDown = readTouch(x, y);
  if (touchDown && !touchWasDown) handleTouchPress(x, y);
  touchWasDown = touchDown;

  pollBootButton();
  render();
  delay(20);
}
