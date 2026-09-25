#include <Arduino.h>
#include <M5Unified.h>
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
static constexpr uint8_t BRIGHTNESS_DIM = 24;
static constexpr uint32_t DISPLAY_DIM_MS = 12000;
static constexpr uint32_t DISPLAY_OFF_MS = 60000;
static constexpr uint32_t FINISHED_AUTO_HOME_MS = 8000;
static constexpr uint32_t WIFI_ATTEMPT_MS = 15000;

static constexpr uint16_t C_BG      = 0xE71C;
static constexpr uint16_t C_WHITE   = 0xFFFF;
static constexpr uint16_t C_NAVY    = 0x10A5;
static constexpr uint16_t C_BLUE    = 0x225D;
static constexpr uint16_t C_VIOLET  = 0x633D;
static constexpr uint16_t C_TEAL    = 0x2575;
static constexpr uint16_t C_GREEN   = 0x35CF;
static constexpr uint16_t C_RED     = 0xEA4B;
static constexpr uint16_t C_AMBER   = 0xFD47;
static constexpr uint16_t C_GREY    = 0x6B6D;
static constexpr uint16_t C_LINE    = 0xBDF7;

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
static const Rect STATUS_BACK  {0,190,320,50};
static const Rect SYNC_RETRY   {0,140,320,50};
static const Rect SYNC_BACK    {0,190,320,50};

enum class AppState : uint8_t { HOME, MODE_CONFIRM, RECORDING, PAUSED, FINISHED, MENU, STATUS, SYNC };
enum class Mode : uint8_t { VISIT, ROUND, MEETING };
enum class SyncPhase : uint8_t { NOT_STARTED, NO_CREDENTIALS, CONNECTING_1, CONNECTING_2, CONNECTED, FAILED };
enum class DisplayPower : uint8_t { ACTIVE, DIMMED, OFF };

AppState state = AppState::HOME;
Mode selectedMode = Mode::VISIT;
SyncPhase syncPhase = SyncPhase::NOT_STARTED;
DisplayPower displayPower = DisplayPower::ACTIVE;

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
uint32_t quickModeChoiceStartedMs = 0;
static constexpr uint32_t QUICK_MODE_CHOICE_MS = 10000;

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

void zone(const Rect& r, const char* title, uint16_t fill, uint16_t fg, const char* subtitle = nullptr) {
  M5.Display.fillRect(r.x, r.y, r.w, r.h, fill);
  M5.Display.drawFastHLine(r.x, r.y, r.w, C_WHITE);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(fg);
  uint8_t size = strlen(title) <= 8 ? 3 : (strlen(title) <= 14 ? 2 : 1);
  M5.Display.setTextSize(size);
  int cy = r.y + r.h / 2 - (subtitle ? 7 : 0);
  M5.Display.drawString(title, r.x + r.w / 2, cy);
  if (subtitle) {
    M5.Display.setTextSize(1);
    M5.Display.drawString(subtitle, r.x + r.w / 2, cy + 20);
  }
}

void refreshBattery() {
  batteryPct = M5.Power.getBatteryLevel();
  batteryCharging = M5.Power.isCharging() == m5::Power_Class::is_charging;
  lastBatteryRefreshMs = millis();
}

void drawHeader(const char* status, const char* sub = nullptr) {
  M5.Display.fillRect(0, 0, SCREEN_W, HEADER_H, C_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(C_BLUE);
  M5.Display.setTextSize(1);
  M5.Display.drawString("VisiteScribe", 8, 5);
  char batt[24];
  if (batteryPct < 0) snprintf(batt, sizeof(batt), "--%%");
  else snprintf(batt, sizeof(batt), "%s%d%%", batteryCharging ? "+" : "", batteryPct);
  M5.Display.setTextDatum(top_right);
  M5.Display.setTextColor((batteryPct >= 0 && batteryPct <= 15) ? C_RED : C_NAVY);
  M5.Display.drawString(batt, 312, 5);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(C_NAVY);
  M5.Display.setTextSize(strlen(status) <= 21 ? 2 : 1);
  M5.Display.drawString(status, SCREEN_W / 2, sub ? 23 : 27);
  if (sub) {
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(C_GREY);
    M5.Display.drawString(sub, SCREEN_W / 2, 35);
  }
  M5.Display.drawFastHLine(0, HEADER_H - 1, SCREEN_W, C_LINE);
}

const char* modeTitle(Mode m) {
  if (m == Mode::VISIT) return "VISITE";
  if (m == Mode::ROUND) return "PATIENTRONDE";
  return "VERGADERING";
}

uint32_t activeElapsedMs() {
  if (!sessionOpen || sessionStartedMs == 0) return 0;
  uint32_t paused = totalPausedMs;
  if (state == AppState::PAUSED) paused += millis() - pauseStartedMs;
  return millis() - sessionStartedMs - paused;
}

void formatElapsed(char* out, size_t len) {
  uint32_t sec = activeElapsedMs() / 1000;
  snprintf(out, len, "%02lu:%02lu:%02lu",
           (unsigned long)(sec / 3600),
           (unsigned long)((sec % 3600) / 60),
           (unsigned long)(sec % 60));
}

void drawHome() {
  drawHeader("GEREED", "bediening met PWR");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_BG);
  centeredText(86, "PWR", C_BLUE, 3);
  centeredText(122, "start opname", C_NAVY, 2);
  centeredText(168, "PWR PWR", C_TEAL, 2);
  centeredText(196, "menu", C_GREY, 1);
}

void drawModeConfirm() {
  drawHeader(modeTitle(selectedMode), "PWR = START");
  zone(TWO_TOP, "START", C_GREEN, C_WHITE);
  zone(TWO_BOTTOM, "TERUG", C_NAVY, C_WHITE);
}

void drawRecording() {
  char elapsed[16];
  formatElapsed(elapsed, sizeof(elapsed));

  if (quickModeChoiceActive) {
    const uint32_t used = millis() - quickModeChoiceStartedMs;
    const uint32_t remaining =
        used >= QUICK_MODE_CHOICE_MS ? 0 : (QUICK_MODE_CHOICE_MS - used + 999) / 1000;
    char sub[40];
    snprintf(sub, sizeof(sub), "opname loopt - nog %lus", (unsigned long)remaining);
    drawHeader("KIES TYPE", sub);
    zone(TWO_TOP, "VISITE", C_BLUE, C_WHITE, "patient / patientronde");
    zone(TWO_BOTTOM, "VERGADERING", C_TEAL, C_WHITE, "dubbel PWR = marker");
    lastUiSecond = activeElapsedMs() / 1000;
    return;
  }

  char title[40];
  if (selectedMode != Mode::MEETING && patientNumber > 1) {
    snprintf(title, sizeof(title), "VISITE - PATIENT %u", patientNumber);
  } else {
    snprintf(title, sizeof(title), "%s",
             selectedMode == Mode::MEETING ? "VERGADERING" : "VISITE");
  }

  drawHeader(title, elapsed);
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_BG);
  centeredText(91, "OPNAME LOOPT", C_RED, 2);
  centeredText(133, "PWR = STOP", C_NAVY, 2);
  centeredText(174,
               selectedMode == Mode::MEETING ? "PWR PWR = MARKER"
                                             : "PWR PWR = VOLGENDE PATIENT",
               C_BLUE, 1);
  centeredText(207, "touch uitgeschakeld", C_GREY, 1);
  lastUiSecond = activeElapsedMs() / 1000;
}

void drawPaused() {
  char elapsed[16];
  formatElapsed(elapsed, sizeof(elapsed));
  drawHeader("PRIVACY PAUZE", elapsed);
  zone(THREE_TOP, "HERVAT", C_GREEN, C_WHITE, "microfoons weer aan");
  zone(THREE_MIDDLE, selectedMode == Mode::ROUND ? "VOLGENDE" : "MARKER", C_BLUE, C_WHITE);
  zone(THREE_BOTTOM, "STOP", C_RED, C_WHITE);
}

void drawFinished() {
  drawHeader("OPNAME OPGESLAGEN");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_BG);
  centeredText(102, "KLAAR", C_GREEN, 3);
  centeredText(154, "PWR = nieuwe opname", C_NAVY, 1);
  centeredText(184, "PWR PWR = menu", C_GREY, 1);
}

void drawMenu() {
  drawHeader("MENU");
  zone(THREE_TOP, "STATUS", C_BLUE, C_WHITE);
  zone(THREE_MIDDLE, "SYNC", C_TEAL, C_WHITE, "Wi-Fi alleen op verzoek");
  zone(THREE_BOTTOM, "TERUG", C_NAVY, C_WHITE);
}

void drawStatus() {
  drawHeader("STATUS");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, 150, C_BG);
  char line[64];
  snprintf(line, sizeof(line), "ACCU  %s%d%%", batteryCharging ? "laden  " : "", batteryPct);
  centeredText(62, line, batteryPct <= 15 ? C_RED : C_NAVY, 2);
  centeredText(91, sdOk ? "MICROSD  OK" : "MICROSD  FOUT", sdOk ? C_GREEN : C_RED, 2);
  centeredText(120, touchOk ? "TOUCH  FT6336 OK" : "TOUCH  FOUT", touchOk ? C_GREEN : C_RED, 1);
  snprintf(line, sizeof(line), "AUDIO  %s", audioError ? "FOUT" : "48k stereo gereed");
  centeredText(145, line, audioError ? C_RED : C_NAVY, 1);
  snprintf(line, sizeof(line), "BOARD ID  %d", (int)M5.getBoard());
  centeredText(168, line, C_GREY, 1);
  zone(STATUS_BACK, "TERUG", C_NAVY, C_WHITE);
}

void drawSync() {
  const char* title = "SYNC";
  if (syncPhase == SyncPhase::NO_CREDENTIALS) title = "WIFI NIET INGESTELD";
  else if (syncPhase == SyncPhase::CONNECTING_1 || syncPhase == SyncPhase::CONNECTING_2) title = "VERBINDEN...";
  else if (syncPhase == SyncPhase::CONNECTED) title = "NETWERK OK";
  else if (syncPhase == SyncPhase::FAILED) title = "GEEN VERBINDING";
  drawHeader(title);
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, 100, C_BG);
  if (syncPhase == SyncPhase::CONNECTED) {
    centeredText(80, WiFi.SSID().c_str(), C_NAVY, 1);
    centeredText(110, WiFi.localIP().toString().c_str(), C_BLUE, 2);
  } else {
    centeredText(92, "Alleen netwerkverbinding", C_GREY, 1);
    centeredText(116, "server-upload volgt later", C_GREY, 1);
  }
  zone(SYNC_RETRY, "OPNIEUW", C_TEAL, C_WHITE);
  zone(SYNC_BACK, "TERUG", C_NAVY, C_WHITE);
}

void render(bool force = false) {
  // Do not redraw timers or UI into a sleeping LCD controller.
  if (!force && displayPower == DisplayPower::OFF) return;

  if (!force && !screenDirty) {
    if ((state == AppState::RECORDING || state == AppState::PAUSED) && activeElapsedMs() / 1000 != lastUiSecond) screenDirty = true;
    else return;
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
  makeAudioPaths();
  if (SD.exists(wavTmpPath)) SD.remove(wavTmpPath);
  wavFile = SD.open(wavTmpPath, FILE_WRITE);
  if (!wavFile) return false;
  WAVHeader h;
  if (wavFile.write(reinterpret_cast<const uint8_t*>(&h), sizeof(h)) != sizeof(h)) { wavFile.close(); return false; }
  wavDataBytes = 0;
  audioError = false;
  return true;
}

void finalizeWavSegment() {
  if (!wavFile) return;
  writeHeader(wavFile, wavDataBytes);
  wavFile.close();
  if (SD.exists(wavFinalPath)) SD.remove(wavFinalPath);
  if (!SD.rename(wavTmpPath, wavFinalPath)) audioError = true;
}

void logEvent(const char* eventName, uint32_t offsetMs) {
  if (!sdOk || eventsPath[0] == '\0') return;
  File f = SD.open(eventsPath, FILE_APPEND);
  if (!f) return;
  f.printf("%lu,%s,%u,%u,%u,%s\n", (unsigned long)offsetMs, eventName, patientNumber, segmentNumber, markerCount, wavFinalPath);
  f.close();
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
    if (wavFile && done.data && done.samples) {
      size_t bytes = done.samples * sizeof(int16_t);
      size_t written = wavFile.write(reinterpret_cast<uint8_t*>(done.data), bytes);
      if (written != bytes) audioError = true;
      wavDataBytes += written;
    }
    if (captureRunning && done.data && !queueAudio(done.data)) audioError = true;
  }
}

bool startCapture() {
  if (!wavFile) return false;
  while (audioDoneQueue && uxQueueMessagesWaiting(audioDoneQueue)) { AudioDone d; xQueueReceive(audioDoneQueue, &d, 0); }
  M5.Speaker.end();
  auto cfg = M5.Mic.config();
  cfg.sample_rate = AUDIO_RATE;
  cfg.input_channel = m5::input_stereo;
  cfg.over_sampling = 1;
  cfg.noise_filter_level = 0;
  M5.Mic.config(cfg);
  M5.Mic.setBufferReleaseCallback(nullptr, audioReleased);
  if (!M5.Mic.begin()) { audioError = true; return false; }
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
  while (M5.Mic.isRecording() && (int32_t)(deadline - millis()) > 0) { serviceAudio(); delay(1); }
  M5.Mic.end();
  serviceAudio();
}

bool startNewSession(Mode mode) {
  if (!sdOk) return false;
  selectedMode = mode;
  sessionId = findNextSessionId();
  patientNumber = 1;
  segmentNumber = 1;
  markerCount = 0;
  totalPausedMs = 0;
  pauseStartedMs = 0;
  sessionStartedMs = millis();
  sessionOpen = true;
  snprintf(eventsPath, sizeof(eventsPath), "/visitescribe/s%05u_events.csv", sessionId);
  if (SD.exists(eventsPath)) SD.remove(eventsPath);
  File events = SD.open(eventsPath, FILE_WRITE);
  if (events) { events.println("elapsed_ms,event,patient,segment,markers,audio_file"); events.close(); }
  if (!openWavSegment()) { sessionOpen = false; return false; }
  logEvent("session_started", 0);
  if (!startCapture()) { finalizeWavSegment(); sessionOpen = false; return false; }
  state = AppState::RECORDING;
  screenDirty = true;
  return true;
}

bool startQuickSession() {
  // Capture starts immediately. VISITE is the default so doing nothing for ten
  // seconds still produces a valid patient recording.
  visitPatientFlow = true;
  quickModeChoiceActive = true;
  quickModeChoiceStartedMs = millis();

  if (!startNewSession(Mode::VISIT)) {
    quickModeChoiceActive = false;
    visitPatientFlow = false;
    quickModeChoiceStartedMs = 0;
    return false;
  }
  return true;
}

void selectQuickMode(Mode mode) {
  if (!quickModeChoiceActive || !sessionOpen) return;

  if (mode == Mode::MEETING) {
    selectedMode = Mode::MEETING;
    visitPatientFlow = false;

    // The open temporary WAV was intentionally started before the type choice.
    // Keep writing that same file and only alter its final rename target.
    snprintf(wavFinalPath, sizeof(wavFinalPath),
             "/visitescribe/s%05u_meeting.wav", sessionId);
    logEvent("mode_selected_meeting", activeElapsedMs());
  } else {
    selectedMode = Mode::VISIT;
    visitPatientFlow = true;
    logEvent("mode_selected_visit", activeElapsedMs());
  }

  quickModeChoiceActive = false;
  quickModeChoiceStartedMs = 0;
  lastUserActivityMs = millis();
  screenDirty = true;
}

void serviceQuickModeChoiceTimeout() {
  if (!quickModeChoiceActive) return;
  if (millis() - quickModeChoiceStartedMs < QUICK_MODE_CHOICE_MS) return;
  selectQuickMode(Mode::VISIT);
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
  uint32_t off = activeElapsedMs();
  const bool patientMode =
      selectedMode == Mode::ROUND || (selectedMode == Mode::VISIT && visitPatientFlow);
  if (patientMode) {
    if (state == AppState::RECORDING) stopCapture();
    logEvent("patient_boundary", off);
    finalizeWavSegment();
    ++patientNumber;
    segmentNumber = 1;
    if (!openWavSegment() || !startCapture()) audioError = true;
    state = AppState::RECORDING;
    logEvent("patient_started", off);
  } else {
    ++markerCount;
    logEvent("marker", off);
  }
  screenDirty = true;
}

void stopSession() {
  if (!sessionOpen) return;
  uint32_t off = activeElapsedMs();
  bool paused = state == AppState::PAUSED;
  if (!paused) stopCapture();
  logEvent("session_stopped", off);
  finalizeWavSegment();
  if (paused) { totalPausedMs += millis() - pauseStartedMs; pauseStartedMs = 0; }
  state = AppState::FINISHED;
  finishedAtMs = millis();
  screenDirty = true;
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
  quickModeChoiceStartedMs = 0;
  eventsPath[0] = wavTmpPath[0] = wavFinalPath[0] = '\0';
  sessionStartedMs = pauseStartedMs = totalPausedMs = finishedAtMs = 0;
  patientNumber = segmentNumber = 1;
  markerCount = 0;
  state = AppState::HOME;
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
      if (HOME_VISIT.contains(x,y)) { selectedMode = Mode::VISIT; state = AppState::MODE_CONFIRM; screenDirty = true; }
      else if (HOME_ROUND.contains(x,y)) { selectedMode = Mode::ROUND; state = AppState::MODE_CONFIRM; screenDirty = true; }
      else if (HOME_MEETING.contains(x,y)) { selectedMode = Mode::MEETING; state = AppState::MODE_CONFIRM; screenDirty = true; }
      else if (HOME_MENU.contains(x,y)) { state = AppState::MENU; screenDirty = true; }
      break;
    case AppState::MODE_CONFIRM:
      if (TWO_TOP.contains(x,y)) startNewSession(selectedMode);
      else if (TWO_BOTTOM.contains(x,y)) goHome();
      break;
    case AppState::RECORDING:
      if (THREE_TOP.contains(x,y)) togglePrivacy();
      else if (THREE_MIDDLE.contains(x,y)) addMarkerOrNext();
      else if (THREE_BOTTOM.contains(x,y)) stopSession();
      break;
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
      if (THREE_TOP.contains(x,y)) { refreshBattery(); state = AppState::STATUS; screenDirty = true; }
      else if (THREE_MIDDLE.contains(x,y)) beginSync();
      else if (THREE_BOTTOM.contains(x,y)) goHome();
      break;
    case AppState::STATUS:
      if (STATUS_BACK.contains(x,y)) { state = AppState::MENU; screenDirty = true; }
      break;
    case AppState::SYNC:
      if (SYNC_RETRY.contains(x,y)) startWifiAttempt(0);
      else if (SYNC_BACK.contains(x,y)) { wifiOff(); syncPhase = SyncPhase::NOT_STARTED; state = AppState::MENU; screenDirty = true; }
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
  if (state == AppState::SYNC &&
      (syncPhase == SyncPhase::CONNECTING_1 || syncPhase == SyncPhase::CONNECTING_2)) {
    return;
  }

  // Keep the complete 10-second mode chooser visible.
  if (quickModeChoiceActive) return;

  const uint32_t idle = millis() - lastUserActivityMs;
  const bool recording =
      state == AppState::RECORDING || state == AppState::PAUSED;
  const bool menuLike =
      state == AppState::MENU || state == AppState::STATUS || state == AppState::SYNC;

  const uint32_t dimMs = recording ? 1500 : (menuLike ? 12000 : 4000);
  const uint32_t offMs = recording ? 3500 : (menuLike ? 30000 : 10000);

  if (idle >= offMs && displayPower != DisplayPower::OFF) {
    // Real LCD sleep saves more than brightness=0 and also stops invisible
    // display traffic until the next physical PWR wake.
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

  audioDoneQueue = xQueueCreate(4, sizeof(AudioDone));
  touchOk = ensureTouchController();
  sdOk = ensureStorage();
  refreshBattery();
  wifiOff();
  lastUserActivityMs = millis();

  Serial.println("VisiteScribe CoreS3-Lite v0.2");
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
