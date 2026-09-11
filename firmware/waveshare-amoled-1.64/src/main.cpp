#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

#ifndef VISITESCRIBE_BOARD_REV
#define VISITESCRIBE_BOARD_REV 2
#endif

// Waveshare ESP32-S3-Touch-AMOLED-1.64
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

// GPIO0 is the physical BOOT button. Arduino-ESP32 already owns BOOT_PIN.
static constexpr int VISITESCRIBE_BOOT_GPIO = 0;

// OurMind-inspired palette, based on the current public OurMind visual identity:
// cobalt blue wordmark, pale lavender surfaces, white cards, dark navy text.
static constexpr uint16_t C_OM_BG      = 0xDEDF;  // pale lavender
static constexpr uint16_t C_OM_BLUE    = 0x225D;  // cobalt
static constexpr uint16_t C_OM_NAVY    = 0x1084;  // deep navy
static constexpr uint16_t C_OM_WHITE   = 0xFFFF;
static constexpr uint16_t C_OM_CARD    = 0xF7BF;  // warm white
static constexpr uint16_t C_OM_SOFT    = 0xEF5F;  // soft lavender-white
static constexpr uint16_t C_OM_LINE    = 0xADBF;  // periwinkle
static constexpr uint16_t C_OM_GREEN   = 0x35CF;
static constexpr uint16_t C_OM_RED     = 0xEA4B;
static constexpr uint16_t C_OM_AMBER   = 0xF527;
static constexpr uint16_t C_OM_MUTED   = 0x6B6D;

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

// Home: compact branded masthead + three large equal cards.
static const Rect HOME_VISIT   {14, 78, 252, 112};
static const Rect HOME_ROUND   {14, 200, 252, 112};
static const Rect HOME_MEETING {14, 322, 252, 112};

static const Rect CONF_START   {14, 300, 252, 72};
static const Rect CONF_BACK    {14, 386, 252, 54};
static const Rect REC_PRIVACY  {14, 300, 120, 70};
static const Rect REC_MARKER   {146, 300, 120, 70};
static const Rect REC_STOP     {14, 382, 252, 60};
static const Rect DONE_BACK    {14, 346, 252, 72};

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
uint32_t sessionStartedMs = 0;
uint32_t pauseStartedMs = 0;
uint32_t totalPausedMs = 0;
uint32_t finishedAtMs = 0;
uint32_t bootPressedAtMs = 0;
bool bootWasDown = false;
uint16_t markerCount = 0;
uint16_t patientNumber = 1;
char currentLogPath[96] = {0};

// Only dynamic recording fields are refreshed while recording. Full-screen
// repaints caused visible AMOLED flicker in the first prototype.
uint32_t lastDisplayedSecond = 0xFFFFFFFFUL;

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

// Built-in Arduino_GFX font has no bold weight. Layering the same glyph a
// pixel to the right/down gives a distinctly heavier, still crisp label.
void textAtBold(int x, int y, const char *text, uint16_t color, uint8_t size = 1) {
  textAt(x, y, text, color, size);
  textAt(x + 1, y, text, color, size);
  if (size >= 2) textAt(x, y + 1, text, color, size);
}

void centered(int y, const char *text, uint16_t color, uint8_t size = 1) {
  int width = (int)strlen(text) * 6 * size;
  int x = (SCREEN_W - width) / 2;
  if (x < 2) x = 2;
  textAt(x, y, text, color, size);
}

void centeredBold(int y, const char *text, uint16_t color, uint8_t size = 1) {
  int width = (int)strlen(text) * 6 * size;
  int x = (SCREEN_W - width) / 2;
  if (x < 2) x = 2;
  textAtBold(x, y, text, color, size);
}

void roundedCard(const Rect &r, uint16_t fill, uint16_t border) {
  gfx->fillRoundRect(r.x, r.y, r.w, r.h, 18, fill);
  gfx->drawRoundRect(r.x, r.y, r.w, r.h, 18, border);
  gfx->drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 17, border);
}

void button(const Rect &r, const char *label, uint16_t fill, uint16_t border,
            uint16_t textColor, uint8_t textSize = 2) {
  roundedCard(r, fill, border);
  int width = (int)strlen(label) * 6 * textSize;
  int x = r.x + (r.w - width) / 2;
  int y = r.y + (r.h - 8 * textSize) / 2;
  textAtBold(x, y, label, textColor, textSize);
}

// Small vector recreation of the public OurMind knot mark. For a final
// branded production/demo unit this can be replaced by the official supplied
// logo asset without changing the rest of the UI.
void drawOurMindMark(int cx, int cy, uint16_t color) {
  const int t = 3;
  // upper loop
  gfx->drawRoundRect(cx - 5, cy - 15, 10, 18, 5, color);
  gfx->drawRoundRect(cx - 4, cy - 14, 8, 16, 4, color);
  // lower loop
  gfx->drawRoundRect(cx - 5, cy - 2, 10, 18, 5, color);
  gfx->drawRoundRect(cx - 4, cy - 1, 8, 16, 4, color);
  // left loop
  gfx->drawRoundRect(cx - 15, cy - 5, 18, 10, 5, color);
  gfx->drawRoundRect(cx - 14, cy - 4, 16, 8, 4, color);
  // right loop
  gfx->drawRoundRect(cx - 2, cy - 5, 18, 10, 5, color);
  gfx->drawRoundRect(cx - 1, cy - 4, 16, 8, 4, color);
  gfx->fillRect(cx - t / 2, cy - 7, t, 14, color);
  gfx->fillRect(cx - 7, cy - t / 2, 14, t, color);
}

void drawBrandHeader(const char *status = nullptr) {
  gfx->fillRect(0, 0, SCREEN_W, 66, C_OM_WHITE);

  // Keep logo + wordmark centered as one visual unit.
  const int groupX = 79;
  drawOurMindMark(groupX + 13, 24, C_OM_BLUE);
  textAtBold(groupX + 31, 14, "OurMind", C_OM_BLUE, 2);
  centeredBold(42, "VISITESCRIBE DEMO", C_OM_NAVY, 1);

  if (status) {
    int width = (int)strlen(status) * 6;
    int x = SCREEN_W - 8 - width;
    gfx->fillRoundRect(x - 5, 5, width + 10, 20, 8, C_OM_SOFT);
    textAtBold(x, 11, status, C_OM_NAVY, 1);
  }

  gfx->drawFastHLine(0, 65, SCREEN_W, C_OM_LINE);
}

void drawHomeMode(const Rect &r, const char *label, const char *subtitle,
                  const char *number) {
  roundedCard(r, C_OM_CARD, C_OM_LINE);

  gfx->fillRoundRect(r.x + 12, r.y + 14, 36, 28, 12, C_OM_BLUE);
  textAtBold(r.x + 23, r.y + 22, number, C_OM_WHITE, 1);

  centeredBold(r.y + 48, label, C_OM_BLUE, 3);
  centeredBold(r.y + 84, subtitle, C_OM_NAVY, 1);
}

void drawHome() {
  gfx->fillScreen(C_OM_BG);
  drawBrandHeader(sdOk ? "SD" : "--");

  drawHomeMode(HOME_VISIT, "VISITE", "1 patient", "1");
  drawHomeMode(HOME_ROUND, "PATIENTRONDE", "meerdere patienten", "2");
  drawHomeMode(HOME_MEETING, "VERGADERING", "overleg / bespreking", "3");
}

void drawConfirm() {
  gfx->fillScreen(C_OM_BG);
  drawBrandHeader(sdOk ? "SD" : "--");

  Rect card{14, 82, 252, 196};
  roundedCard(card, C_OM_CARD, C_OM_LINE);
  centeredBold(108, modeTitle(selectedMode), C_OM_BLUE, 2);
  centeredBold(150, "DEMO-OPNAME", C_OM_NAVY, 2);
  centered(195, "Nog geen microfoon aangesloten.", C_OM_MUTED, 1);
  centered(214, "Touch, workflow en microSD", C_OM_MUTED, 1);
  centered(232, "worden wel echt getest.", C_OM_MUTED, 1);

  button(CONF_START, "START DEMO", C_OM_BLUE, C_OM_BLUE, C_OM_WHITE, 2);
  button(CONF_BACK, "TERUG", C_OM_CARD, C_OM_LINE, C_OM_NAVY, 2);
}

void formatElapsed(char *out, size_t len) {
  uint32_t seconds = activeElapsedMs() / 1000;
  uint32_t h = seconds / 3600;
  uint32_t m = (seconds % 3600) / 60;
  uint32_t s = seconds % 60;
  snprintf(out, len, "%02lu:%02lu:%02lu",
           (unsigned long)h, (unsigned long)m, (unsigned long)s);
}

void drawRecordingDynamic(bool force = false) {
  uint32_t second = activeElapsedMs() / 1000;
  if (!force && second == lastDisplayedSecond) return;
  lastDisplayedSecond = second;

  // Only repaint the timer interior; the rest remains untouched and flicker-free.
  gfx->fillRect(28, 120, 224, 42, C_OM_CARD);
  char elapsed[16];
  formatElapsed(elapsed, sizeof(elapsed));
  centeredBold(123, elapsed, C_OM_NAVY, 3);
}

void drawRecording() {
  gfx->fillScreen(C_OM_BG);
  drawBrandHeader("REC");

  Rect timerCard{14, 82, 252, 120};
  roundedCard(timerCard, C_OM_CARD, C_OM_LINE);
  centeredBold(96, modeTitle(selectedMode), C_OM_BLUE, 2);

  Rect infoCard{14, 214, 252, 66};
  roundedCard(infoCard, C_OM_SOFT, C_OM_LINE);
  if (selectedMode == Mode::ROUND) {
    char p[32];
    snprintf(p, sizeof(p), "PATIENT %u", patientNumber);
    centeredBold(236, p, C_OM_BLUE, 2);
  } else {
    char m[32];
    snprintf(m, sizeof(m), "MARKERS %u", markerCount);
    centeredBold(236, m, C_OM_BLUE, 2);
  }

  button(REC_PRIVACY, "PRIVACY", C_OM_CARD, C_OM_AMBER, C_OM_NAVY, 2);
  button(REC_MARKER,
         selectedMode == Mode::ROUND ? "VOLGENDE" : "MARKER",
         C_OM_BLUE, C_OM_BLUE, C_OM_WHITE, 2);
  button(REC_STOP, "STOP", C_OM_RED, C_OM_RED, C_OM_WHITE, 2);

  lastDisplayedSecond = 0xFFFFFFFFUL;
  drawRecordingDynamic(true);
}

void drawPaused() {
  gfx->fillScreen(C_OM_BG);
  drawBrandHeader("PAUZE");

  Rect card{14, 82, 252, 198};
  roundedCard(card, C_OM_CARD, C_OM_LINE);
  centeredBold(108, "PRIVACY PAUZE", C_OM_AMBER, 2);
  centeredBold(148, "GEEN AUDIO", C_OM_RED, 3);
  centered(194, "De microfoon staat hier", C_OM_MUTED, 1);
  centered(212, "in de echte recorder fysiek stil.", C_OM_MUTED, 1);

  char elapsed[16];
  formatElapsed(elapsed, sizeof(elapsed));
  centeredBold(242, elapsed, C_OM_NAVY, 2);

  button(REC_PRIVACY, "HERVAT", C_OM_GREEN, C_OM_GREEN, C_OM_WHITE, 2);
  button(REC_MARKER,
         selectedMode == Mode::ROUND ? "VOLGENDE" : "MARKER",
         C_OM_BLUE, C_OM_BLUE, C_OM_WHITE, 2);
  button(REC_STOP, "STOP", C_OM_RED, C_OM_RED, C_OM_WHITE, 2);
}

void drawFinished() {
  gfx->fillScreen(C_OM_BG);
  drawBrandHeader(sdOk ? "SD" : "--");

  Rect card{14, 92, 252, 220};
  roundedCard(card, C_OM_CARD, C_OM_LINE);
  centeredBold(122, "OPGESLAGEN", C_OM_GREEN, 2);
  centeredBold(160, "DEMO SESSIE KLAAR", C_OM_NAVY, 2);

  char info[48];
  if (selectedMode == Mode::ROUND) {
    snprintf(info, sizeof(info), "%u patienten / %u grenzen",
             patientNumber, markerCount);
  } else {
    snprintf(info, sizeof(info), "%u markers", markerCount);
  }
  centeredBold(215, info, C_OM_BLUE, 1);
  centered(248,
           sdOk ? "events staan op microSD" : "geen SD-log geschreven",
           sdOk ? C_OM_GREEN : C_OM_AMBER, 1);

  button(DONE_BACK, "NIEUWE OPNAME", C_OM_BLUE, C_OM_BLUE, C_OM_WHITE, 2);
}

void render(bool force = false) {
  if (state == AppState::RECORDING && !screenDirty && !force) {
    drawRecordingDynamic(false);
    return;
  }

  if (!force && !screenDirty) return;
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
    boot.printf("boot_ms=%lu board_rev=%d\n",
                (unsigned long)millis(), VISITESCRIBE_BOARD_REV);
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

  // Immediate visual response before SD I/O.
  state = AppState::RECORDING;
  screenDirty = true;
  render(true);

  if (sdOk) {
    File f = SD.open(currentLogPath, FILE_WRITE);
    if (f) {
      f.println("elapsed_ms,event,patient,markers");
      f.close();
    }
  }

  logEvent("session_started");
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

  if (down && bootWasDown && bootPressedAtMs != 0 &&
      now - bootPressedAtMs > 1200) {
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
  Serial.println("VisiteScribe MINI - OurMind branded Waveshare demo v0.4");
  Serial.printf("Board revision target: V%d\n", VISITESCRIBE_BOARD_REV);

  pinMode(VISITESCRIBE_BOOT_GPIO, INPUT_PULLUP);

  if (!gfx->begin()) {
    Serial.println("ERROR: display init failed");
  }
  gfx->fillScreen(C_OM_BG);

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
  delay(12);
}
