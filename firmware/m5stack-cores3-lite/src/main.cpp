#include <Arduino.h>
#include <M5Unified.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <atomic>
#include <math.h>

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

// -----------------------------------------------------------------------------
// VisiteScribe for M5Stack CoreS3-Lite
// - 48 kHz / 16-bit / stereo WAV from the onboard ES7210 dual-mic codec
// - microSD streaming, no fixed recording duration
// - visit / patient-round / meeting modes
// - patient rounds are physically split into separate WAV files per patient
// - privacy pause truly stops microphone capture
// - Wi-Fi is OFF except while the user explicitly opens SYNC
// -----------------------------------------------------------------------------

static constexpr int SCREEN_W = 320;
static constexpr int SCREEN_H = 240;
static constexpr int HEADER_H = 40;

static constexpr int SD_SCK  = 36;
static constexpr int SD_MISO = 35;
static constexpr int SD_MOSI = 37;
static constexpr int SD_CS   = 4;
static constexpr uint32_t SD_HZ = 25000000;

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
static constexpr uint32_t WIFI_ATTEMPT_MS = 6500;

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

enum class Mode : uint8_t { VISIT, ROUND, MEETING };
enum class SyncPhase : uint8_t {
  NOT_STARTED,
  NO_CREDENTIALS,
  CONNECTING_1,
  CONNECTING_2,
  CONNECTED,
  FAILED
};

enum class DisplayPower : uint8_t { ACTIVE, DIMMED, OFF };

AppState state = AppState::HOME;
Mode selectedMode = Mode::VISIT;
SyncPhase syncPhase = SyncPhase::NOT_STARTED;
DisplayPower displayPower = DisplayPower::ACTIVE;

bool sdOk = false;
bool screenDirty = true;
bool sessionOpen = false;
bool captureRunning = false;
bool audioError = false;

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
uint16_t meterLeft = 0;
uint16_t meterRight = 0;

struct AudioDone {
  int16_t* data;
  size_t samples;
};
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

void zone(const Rect& r, const char* title, uint16_t fill, uint16_t fg,
          const char* subtitle = nullptr) {
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
  batteryCharging = M5.Power.isCharging();
  lastBatteryRefreshMs = millis();
}

void drawHeader(const char* status, const char* sub = nullptr) {
  M5.Display.fillRect(0, 0, SCREEN_W, HEADER_H, C_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(C_BLUE);
  M5.Display.setTextSize(1);
  M5.Display.drawString("OurMind", 8, 5);

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
  switch (m) {
    case Mode::VISIT: return "VISITE";
    case Mode::ROUND: return "PATIENTRONDE";
    case Mode::MEETING: return "VERGADERING";
  }
  return "?";
}

uint32_t activeElapsedMs() {
  if (!sessionOpen || sessionStartedMs == 0) return 0;
  uint32_t now = millis();
  uint32_t paused = totalPausedMs;
  if (state == AppState::PAUSED) paused += now - pauseStartedMs;
  return now - sessionStartedMs - paused;
}

void formatElapsed(char* out, size_t len) {
  uint32_t sec = activeElapsedMs() / 1000;
  snprintf(out, len, "%02lu:%02lu:%02lu",
           (unsigned long)(sec / 3600),
           (unsigned long)((sec % 3600) / 60),
           (unsigned long)(sec % 60));
}

void recordingTitle(char* out, size_t len) {
  if (selectedMode == Mode::ROUND) snprintf(out, len, "OPNAME PATIENT %u", patientNumber);
  else if (selectedMode == Mode::VISIT) snprintf(out, len, "OPNAME VISITE");
  else snprintf(out, len, "OPNAME VERGADERING");
}

void pausedTitle(char* out, size_t len) {
  if (selectedMode == Mode::ROUND) snprintf(out, len, "PRIVACY PAUZE P%u", patientNumber);
  else snprintf(out, len, "PRIVACY PAUZE");
}

void drawHome() {
  drawHeader("HOOFDMENU", "PWR = snelle VISITE");
  zone(HOME_VISIT, "VISITE", C_BLUE, C_WHITE, "1 patient");
  zone(HOME_ROUND, "PATIENTRONDE", C_VIOLET, C_WHITE, "aparte audio per patient");
  zone(HOME_MEETING, "VERGADERING", C_TEAL, C_WHITE, "overleg / bespreking");
  zone(HOME_MENU, "MENU", C_NAVY, C_WHITE, "status en sync");
}

void drawModeConfirm() {
  drawHeader(modeTitle(selectedMode), "PWR = START");
  zone(TWO_TOP, "START", C_GREEN, C_WHITE);
  zone(TWO_BOTTOM, "TERUG", C_NAVY, C_WHITE);
}

void drawRecording() {
  char title[32], elapsed[16];
  recordingTitle(title, sizeof(title));
  formatElapsed(elapsed, sizeof(elapsed));
  drawHeader(title, elapsed);
  zone(THREE_TOP, "PRIVACY", C_AMBER, C_NAVY, "microfoons echt uit");
  zone(THREE_MIDDLE, selectedMode == Mode::ROUND ? "VOLGENDE" : "MARKER",
       C_BLUE, C_WHITE,
       selectedMode == Mode::ROUND ? "nieuw audiobestand" : "markeer dit moment");
  zone(THREE_BOTTOM, "STOP", C_RED, C_WHITE, "of korte druk op PWR");
  lastUiSecond = activeElapsedMs() / 1000;
}

void drawPaused() {
  char title[32], elapsed[16];
  pausedTitle(title, sizeof(title));
  formatElapsed(elapsed, sizeof(elapsed));
  drawHeader(title, elapsed);
  zone(THREE_TOP, "HERVAT", C_GREEN, C_WHITE, "microfoons weer aan");
  zone(THREE_MIDDLE, selectedMode == Mode::ROUND ? "VOLGENDE" : "MARKER",
       C_BLUE, C_WHITE);
  zone(THREE_BOTTOM, "STOP", C_RED, C_WHITE, "of korte druk op PWR");
}

void drawFinished() {
  drawHeader("OPNAME OPGESLAGEN", "PWR = VUL AAN");
  zone(TWO_TOP, "VUL AAN", C_BLUE, C_WHITE, "zelfde sessie, nieuw segment");
  zone(TWO_BOTTOM, "KLAAR", C_GREEN, C_WHITE, "terug naar hoofdmenu");
}

void drawMenu() {
  drawHeader("MENU");
  zone(THREE_TOP, "STATUS", C_BLUE, C_WHITE, "accu / opslag / microfoon");
  zone(THREE_MIDDLE, "SYNC", C_TEAL, C_WHITE, "Wi-Fi alleen op verzoek");
  zone(THREE_BOTTOM, "TERUG", C_NAVY, C_WHITE);
}

uint16_t countFinalWavs() {
  if (!sdOk) return 0;
  uint16_t count = 0;
  File dir = SD.open("/visitescribe");
  if (!dir) return 0;
  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String n = f.name();
      if (n.endsWith(".wav")) ++count;
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  return count;
}

void drawStatus() {
  drawHeader("STATUS");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, 150, C_BG);
  char line[64];
  snprintf(line, sizeof(line), "ACCU  %s%d%%", batteryCharging ? "laden  " : "", batteryPct);
  centeredText(62, line, batteryPct <= 15 ? C_RED : C_NAVY, 2);
  centeredText(91, sdOk ? "MICROSD  OK" : "MICROSD  FOUT", sdOk ? C_GREEN : C_RED, 2);
  snprintf(line, sizeof(line), "AUDIO  48kHz / 16-bit / stereo");
  centeredText(120, line, audioError ? C_RED : C_NAVY, 1);
  snprintf(line, sizeof(line), "WAV-BESTANDEN  %u", countFinalWavs());
  centeredText(145, line, C_BLUE, 1);
  centeredText(168, WiFi.status() == WL_CONNECTED ? "WIFI  AAN" : "WIFI  UIT", C_GREY, 1);
  zone(STATUS_BACK, "TERUG", C_NAVY, C_WHITE);
}

const char* syncTitle() {
  switch (syncPhase) {
    case SyncPhase::NO_CREDENTIALS: return "WIFI NIET INGESTELD";
    case SyncPhase::CONNECTING_1:
    case SyncPhase::CONNECTING_2: return "VERBINDEN...";
    case SyncPhase::CONNECTED: return "NETWERK OK";
    case SyncPhase::FAILED: return "GEEN VERBINDING";
    default: return "SYNC";
  }
}

void drawSync() {
  drawHeader("SYNC");
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, 100, C_BG);
  centeredText(70, syncTitle(), syncPhase == SyncPhase::CONNECTED ? C_GREEN : C_NAVY, 2);
  if (syncPhase == SyncPhase::CONNECTING_1 || syncPhase == SyncPhase::CONNECTING_2) {
    const char* ssid = syncPhase == SyncPhase::CONNECTING_1 ? VISITESCRIBE_WIFI_SSID_1 : VISITESCRIBE_WIFI_SSID_2;
    centeredText(100, ssid, C_BLUE, 1);
  } else if (syncPhase == SyncPhase::CONNECTED) {
    centeredText(100, WiFi.SSID().c_str(), C_BLUE, 1);
    centeredText(122, "upload naar server volgt later", C_GREY, 1);
  } else if (syncPhase == SyncPhase::NO_CREDENTIALS) {
    centeredText(105, "maak include/wifi_secrets.h", C_RED, 1);
  }
  zone(SYNC_RETRY, "OPNIEUW", C_BLUE, C_WHITE);
  zone(SYNC_BACK, "TERUG", C_NAVY, C_WHITE, "Wi-Fi weer uit");
}

void render(bool force = false) {
  if (!force && !screenDirty) {
    if (state == AppState::RECORDING || state == AppState::PAUSED) {
      uint32_t sec = activeElapsedMs() / 1000;
      if (sec != lastUiSecond && displayPower != DisplayPower::OFF) {
        lastUiSecond = sec;
        char elapsed[16];
        formatElapsed(elapsed, sizeof(elapsed));
        M5.Display.fillRect(120, 30, 80, 9, C_WHITE);
        M5.Display.setTextDatum(middle_center);
        M5.Display.setTextColor(C_GREY);
        M5.Display.setTextSize(1);
        M5.Display.drawString(elapsed, 160, 35);
      }
    }
    return;
  }

  if (displayPower == DisplayPower::OFF) return;
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

bool wakeFromOff() {
  if (displayPower != DisplayPower::OFF) return false;
  M5.Display.setBrightness(BRIGHTNESS_ACTIVE);
  displayPower = DisplayPower::ACTIVE;
  lastUserActivityMs = millis();
  screenDirty = true;
  render(true);
  return true;
}

void noteActivity() {
  lastUserActivityMs = millis();
  if (displayPower == DisplayPower::DIMMED) {
    M5.Display.setBrightness(BRIGHTNESS_ACTIVE);
    displayPower = DisplayPower::ACTIVE;
  }
}

void serviceDisplayPower() {
  if (state == AppState::SYNC &&
      (syncPhase == SyncPhase::CONNECTING_1 || syncPhase == SyncPhase::CONNECTING_2)) {
    if (displayPower != DisplayPower::ACTIVE) {
      M5.Display.setBrightness(BRIGHTNESS_ACTIVE);
      displayPower = DisplayPower::ACTIVE;
    }
    lastUserActivityMs = millis();
    return;
  }

  uint32_t idle = millis() - lastUserActivityMs;
  if (idle >= DISPLAY_OFF_MS && displayPower != DisplayPower::OFF) {
    M5.Display.setBrightness(0);
    displayPower = DisplayPower::OFF;
  } else if (idle >= DISPLAY_DIM_MS && displayPower == DisplayPower::ACTIVE) {
    M5.Display.setBrightness(BRIGHTNESS_DIM);
    displayPower = DisplayPower::DIMMED;
  }
}

bool ensureStorage() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, SD_HZ)) return false;
  if (SD.cardType() == CARD_NONE) return false;
  if (!SD.exists("/visitescribe")) SD.mkdir("/visitescribe");
  return true;
}

void writeHeader(File& f, uint32_t dataBytes) {
  WAVHeader h;
  h.fileSize = 36 + dataBytes;
  h.dataSize = dataBytes;
  f.seek(0);
  f.write(reinterpret_cast<const uint8_t*>(&h), sizeof(h));
  f.flush();
}

void recoverTmpWavs() {
  if (!sdOk) return;
  File dir = SD.open("/visitescribe");
  if (!dir) return;
  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      String name = entry.name();
      if (name.endsWith(".wav.tmp") && entry.size() >= sizeof(WAVHeader)) {
        uint32_t dataBytes = (uint32_t)(entry.size() - sizeof(WAVHeader));
        writeHeader(entry, dataBytes);
        entry.close();
        String full = name.startsWith("/") ? name : String("/visitescribe/") + name;
        String target = full;
        target.replace(".wav.tmp", ".recovered.wav");
        SD.rename(full.c_str(), target.c_str());
        entry = dir.openNextFile();
        continue;
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
}

uint16_t findNextSessionId() {
  uint16_t maxId = 0;
  File dir = SD.open("/visitescribe");
  if (!dir) return 1;
  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String n = f.name();
      int slash = n.lastIndexOf('/');
      if (slash >= 0) n = n.substring(slash + 1);
      if (n.length() >= 6 && n[0] == 's') {
        uint16_t id = (uint16_t)n.substring(1, 6).toInt();
        if (id > maxId) maxId = id;
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  return maxId + 1;
}

void makeAudioPaths() {
  const char* tag = selectedMode == Mode::VISIT ? "visit" :
                    selectedMode == Mode::MEETING ? "meeting" : "round";
  if (selectedMode == Mode::ROUND) {
    if (segmentNumber <= 1) {
      snprintf(wavFinalPath, sizeof(wavFinalPath),
               "/visitescribe/s%05u_%s_p%03u.wav", sessionId, tag, patientNumber);
    } else {
      snprintf(wavFinalPath, sizeof(wavFinalPath),
               "/visitescribe/s%05u_%s_p%03u_add%02u.wav",
               sessionId, tag, patientNumber, segmentNumber - 1);
    }
  } else {
    if (segmentNumber <= 1) {
      snprintf(wavFinalPath, sizeof(wavFinalPath),
               "/visitescribe/s%05u_%s.wav", sessionId, tag);
    } else {
      snprintf(wavFinalPath, sizeof(wavFinalPath),
               "/visitescribe/s%05u_%s_add%02u.wav",
               sessionId, tag, segmentNumber - 1);
    }
  }
  snprintf(wavTmpPath, sizeof(wavTmpPath), "%s.tmp", wavFinalPath);
}

bool openWavSegment() {
  if (!sdOk) return false;
  makeAudioPaths();
  if (SD.exists(wavTmpPath)) SD.remove(wavTmpPath);
  wavFile = SD.open(wavTmpPath, FILE_WRITE);
  if (!wavFile) return false;
  WAVHeader blank;
  if (wavFile.write(reinterpret_cast<const uint8_t*>(&blank), sizeof(blank)) != sizeof(blank)) {
    wavFile.close();
    return false;
  }
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
  f.printf("%lu,%s,%u,%u,%u,%s\n",
           (unsigned long)offsetMs,
           eventName,
           patientNumber,
           segmentNumber,
           markerCount,
           wavFinalPath);
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

void calculateMeters(const int16_t* data, size_t samples) {
  uint32_t peakL = 0, peakR = 0;
  for (size_t i = 0; i + 1 < samples; i += 2) {
    uint32_t l = abs((int)data[i]);
    uint32_t r = abs((int)data[i + 1]);
    if (l > peakL) peakL = l;
    if (r > peakR) peakR = r;
  }
  uint32_t pctL = peakL * 100 / 32767;
  uint32_t pctR = peakR * 100 / 32767;
  meterLeft = (uint16_t)(pctL > 100 ? 100 : pctL);
  meterRight = (uint16_t)(pctR > 100 ? 100 : pctR);
}

void serviceAudio() {
  if (!audioDoneQueue) return;
  AudioDone done;
  while (xQueueReceive(audioDoneQueue, &done, 0) == pdTRUE) {
    if (wavFile && done.data && done.samples) {
      calculateMeters(done.data, done.samples);
      size_t bytes = done.samples * sizeof(int16_t);
      size_t written = wavFile.write(reinterpret_cast<uint8_t*>(done.data), bytes);
      if (written != bytes) audioError = true;
      wavDataBytes += written;
    }
    if (captureRunning && done.data) {
      if (!queueAudio(done.data)) audioError = true;
    }
  }
}

bool startCapture() {
  if (!wavFile) return false;
  while (audioDoneQueue && uxQueueMessagesWaiting(audioDoneQueue)) {
    AudioDone dummy;
    xQueueReceive(audioDoneQueue, &dummy, 0);
  }
  M5.Speaker.end();
  auto cfg = M5.Mic.config();
  cfg.sample_rate = AUDIO_RATE;
  cfg.input_channel = m5::input_stereo;
  cfg.over_sampling = 1;
  cfg.noise_filter_level = 0;
  M5.Mic.config(cfg);
  M5.Mic.setBufferReleaseCallback(nullptr, audioReleased);
  if (!M5.Mic.begin()) {
    audioError = true;
    return false;
  }
  captureRunning = true;
  bool a = queueAudio(audioBufA);
  bool b = queueAudio(audioBufB);
  if (!a || !b) {
    audioError = true;
    captureRunning = false;
    M5.Mic.end();
    return false;
  }
  return true;
}

void stopCapture() {
  if (!captureRunning && !M5.Mic.isRunning()) return;
  captureRunning = false;
  uint32_t deadline = millis() + 1000;
  while (M5.Mic.isRecording() && (int32_t)(deadline - millis()) > 0) {
    serviceAudio();
    delay(1);
  }
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
  if (events) {
    events.println("elapsed_ms,event,patient,segment,markers,audio_file");
    events.close();
  }

  if (!openWavSegment()) {
    sessionOpen = false;
    return false;
  }
  logEvent("session_started", 0);
  if (!startCapture()) {
    finalizeWavSegment();
    sessionOpen = false;
    return false;
  }
  state = AppState::RECORDING;
  screenDirty = true;
  return true;
}

void togglePrivacy() {
  if (state == AppState::RECORDING) {
    uint32_t offset = activeElapsedMs();
    stopCapture();
    pauseStartedMs = millis();
    state = AppState::PAUSED;
    logEvent("privacy_pause_started", offset);
    screenDirty = true;
  } else if (state == AppState::PAUSED) {
    uint32_t offset = activeElapsedMs();
    uint32_t paused = millis() - pauseStartedMs;
    totalPausedMs += paused;
    pauseStartedMs = 0;
    if (!startCapture()) audioError = true;
    state = AppState::RECORDING;
    logEvent("privacy_pause_ended", offset);
    screenDirty = true;
  }
}

void addMarkerOrNext() {
  uint32_t offset = activeElapsedMs();
  if (selectedMode == Mode::ROUND) {
    stopCapture();
    logEvent("patient_boundary", offset);
    finalizeWavSegment();
    ++patientNumber;
    segmentNumber = 1;
    if (!openWavSegment() || !startCapture()) audioError = true;
    logEvent("patient_started", offset);
  } else {
    ++markerCount;
    logEvent("marker", offset);
  }
  screenDirty = true;
}

void stopSession() {
  if (!sessionOpen) return;
  uint32_t offset = activeElapsedMs();
  bool wasPaused = state == AppState::PAUSED;
  if (!wasPaused) stopCapture();
  logEvent("session_stopped", offset);
  finalizeWavSegment();
  if (wasPaused) {
    totalPausedMs += millis() - pauseStartedMs;
    pauseStartedMs = 0;
  }
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
  eventsPath[0] = '\0';
  wavTmpPath[0] = '\0';
  wavFinalPath[0] = '\0';
  sessionStartedMs = 0;
  pauseStartedMs = 0;
  totalPausedMs = 0;
  finishedAtMs = 0;
  patientNumber = 1;
  segmentNumber = 1;
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
  const char* ssid = index == 0 ? VISITESCRIBE_WIFI_SSID_1 : VISITESCRIBE_WIFI_SSID_2;
  const char* pass = index == 0 ? VISITESCRIBE_WIFI_PASSWORD_1 : VISITESCRIBE_WIFI_PASSWORD_2;
  WiFi.begin(ssid, pass);
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
  if (WiFi.status() == WL_CONNECTED) {
    syncPhase = SyncPhase::CONNECTED;
    screenDirty = true;
    return;
  }
  if (millis() - syncAttemptStartedMs >= WIFI_ATTEMPT_MS) {
    if (syncPhase == SyncPhase::CONNECTING_1) startWifiAttempt(1);
    else {
      wifiOff();
      syncPhase = SyncPhase::FAILED;
      screenDirty = true;
    }
  }
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
    case AppState::HOME:
      startNewSession(Mode::VISIT);
      break;
    case AppState::MODE_CONFIRM:
      startNewSession(selectedMode);
      break;
    case AppState::RECORDING:
    case AppState::PAUSED:
      stopSession();
      break;
    case AppState::FINISHED:
      resumeSession();
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(250);

  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(BRIGHTNESS_ACTIVE);
  M5.Display.fillScreen(C_BG);
  M5.Speaker.end();

  audioDoneQueue = xQueueCreate(4, sizeof(AudioDone));
  sdOk = ensureStorage();
  if (sdOk) recoverTmpWavs();
  refreshBattery();
  wifiOff();
  lastUserActivityMs = millis();

  Serial.println("VisiteScribe CoreS3-Lite v0.1");
  Serial.printf("microSD: %s\n", sdOk ? "OK" : "NOT FOUND");
  Serial.printf("battery: %d%%\n", batteryPct);
  Serial.println("audio: 48kHz / 16-bit / stereo; patient rounds split per patient");
#if VISITESCRIBE_WIFI_CONFIGURED
  Serial.println("Wi-Fi credentials present; radio remains OFF until SYNC");
#else
  Serial.println("Wi-Fi not configured; recorder works fully offline");
#endif

  if (!sdOk) {
    drawHeader("MICROSD FOUT");
    centeredText(110, "Plaats FAT32 microSD en reset", C_RED, 2);
    centeredText(145, "Recorder kan niet starten", C_NAVY, 1);
  } else {
    render(true);
  }
}

void loop() {
  M5.update();
  serviceAudio();
  serviceSync();

  if (millis() - lastBatteryRefreshMs > 10000) {
    int old = batteryPct;
    refreshBattery();
    if (old != batteryPct && state == AppState::STATUS) screenDirty = true;
  }

  auto t = M5.Touch.getDetail();
  if (t.state == m5::touch_state_t::touch_begin) {
    if (wakeFromOff()) {
      // Wake-only: never accidentally STOP/NEXT/PRIVACY on a black screen.
    } else {
      noteActivity();
      if (sdOk) handleTouch(t.x, t.y);
    }
  }

  if (M5.BtnPWR.wasClicked()) {
    if (wakeFromOff()) {
      // Same safety rule as touch: first hardware click wakes only when black.
    } else {
      noteActivity();
      if (sdOk) handlePowerButton();
    }
  }

  if (state == AppState::FINISHED && millis() - finishedAtMs >= FINISHED_AUTO_HOME_MS) {
    goHome();
  }

  render();
  serviceDisplayPower();
  delay(5);
}
