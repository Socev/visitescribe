#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"

// -----------------------------------------------------------------------------
// VisiteScribe / OurMind IM73D122 dual-PDM laboratory firmware
// Target: Waveshare ESP32-S3-Touch-AMOLED-1.64 V2
//
// Two IM73D122 microphones share one PDM clock and one PDM data line:
//   Mic L: SELECT -> GND
//   Mic R: SELECT -> 3V3
// The ESP32-S3 hardware PDM->PCM converter produces 48 kHz / 16-bit stereo PCM.
// 48 kHz with I2S_PDM_DSR_8S yields a 3.072 MHz PDM clock, the IM73D122 mode
// in which Infineon specifies its best 73 dB(A) SNR.
// -----------------------------------------------------------------------------

static constexpr int SCREEN_W = 280;
static constexpr int SCREEN_H = 456;

// Waveshare AMOLED V2 pins.
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

// Free, header-exposed GPIOs on the Waveshare V2 board.
static constexpr int PDM_CLK_GPIO = 5;
static constexpr int PDM_DATA_GPIO = 6;
static constexpr int VISITESCRIBE_BOOT_GPIO = 0;

static constexpr uint32_t AUDIO_RATE = 48000;
static constexpr uint16_t AUDIO_CHANNELS = 2;
static constexpr uint16_t AUDIO_BITS = 16;
static constexpr uint32_t AUDIO_BYTES_PER_SECOND =
    AUDIO_RATE * AUDIO_CHANNELS * (AUDIO_BITS / 8);

// OurMind-inspired palette used by the demo firmware.
static constexpr uint16_t C_BG = 0xDEDF;
static constexpr uint16_t C_BLUE = 0x225D;
static constexpr uint16_t C_NAVY = 0x1084;
static constexpr uint16_t C_WHITE = 0xFFFF;
static constexpr uint16_t C_CARD = 0xF7BF;
static constexpr uint16_t C_SOFT = 0xEF5F;
static constexpr uint16_t C_LINE = 0xADBF;
static constexpr uint16_t C_GREEN = 0x35CF;
static constexpr uint16_t C_RED = 0xEA4B;
static constexpr uint16_t C_AMBER = 0xF527;
static constexpr uint16_t C_MUTED = 0x6B6D;

Arduino_DataBus *displayBus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_GFX *gfx = new Arduino_CO5300(
    displayBus, LCD_RST, 0, SCREEN_W, SCREEN_H, 20, 0, 180, 24);

SPIClass sdSPI(HSPI);

i2s_chan_handle_t pdmRx = nullptr;
TaskHandle_t audioTaskHandle = nullptr;
SemaphoreHandle_t wavMutex = nullptr;
portMUX_TYPE levelMux = portMUX_INITIALIZER_UNLOCKED;

File wavFile;
char wavTmpPath[96] = {0};
char wavFinalPath[96] = {0};
bool recording = false;          // protected by wavMutex
uint32_t wavDataBytes = 0;       // protected by wavMutex
volatile bool pdmReady = false;
volatile bool audioTaskRunning = false;

float levelLeftDb = -90.0f;       // protected by levelMux
float levelRightDb = -90.0f;      // protected by levelMux
uint32_t recordStartedMs = 0;
uint32_t lastMeterDrawMs = 0;
uint32_t lastTimerSecond = 0xFFFFFFFFUL;
bool touchWasDown = false;
uint32_t bootDownSince = 0;
bool bootWasDown = false;
bool sdOk = false;

struct Rect {
  int x;
  int y;
  int w;
  int h;
  bool contains(uint16_t px, uint16_t py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

static const Rect ACTION_BUTTON{16, 364, 248, 72};

void textAt(int x, int y, const char *text, uint16_t color, uint8_t size = 1) {
  gfx->setTextColor(color);
  gfx->setTextSize(size);
  gfx->setCursor(x, y);
  gfx->print(text);
}

void textAtBold(int x, int y, const char *text, uint16_t color, uint8_t size = 1) {
  textAt(x, y, text, color, size);
  textAt(x + 1, y, text, color, size);
  if (size >= 2) textAt(x, y + 1, text, color, size);
}

void centeredBold(int y, const char *text, uint16_t color, uint8_t size = 1) {
  int width = (int)strlen(text) * 6 * size;
  int x = (SCREEN_W - width) / 2;
  if (x < 2) x = 2;
  textAtBold(x, y, text, color, size);
}

void drawOurMindMark(int cx, int cy, uint16_t color) {
  gfx->drawRoundRect(cx - 5, cy - 15, 10, 18, 5, color);
  gfx->drawRoundRect(cx - 5, cy - 2, 10, 18, 5, color);
  gfx->drawRoundRect(cx - 15, cy - 5, 18, 10, 5, color);
  gfx->drawRoundRect(cx - 2, cy - 5, 18, 10, 5, color);
  gfx->fillRect(cx - 1, cy - 7, 3, 14, color);
  gfx->fillRect(cx - 7, cy - 1, 14, 3, color);
}

void drawHeader() {
  gfx->fillRect(0, 0, SCREEN_W, 65, C_WHITE);
  drawOurMindMark(75, 24, C_BLUE);
  textAtBold(96, 14, "OurMind", C_BLUE, 2);
  centeredBold(43, "IM73D122 DUAL MIC LAB", C_NAVY, 1);
  gfx->drawFastHLine(0, 64, SCREEN_W, C_LINE);
}

bool isRecording() {
  if (!wavMutex) return false;
  bool value = false;
  if (xSemaphoreTake(wavMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    value = recording;
    xSemaphoreGive(wavMutex);
  }
  return value;
}

void drawStaticUi() {
  gfx->fillScreen(C_BG);
  drawHeader();

  gfx->fillRoundRect(14, 80, 252, 70, 16, C_CARD);
  gfx->drawRoundRect(14, 80, 252, 70, 16, C_LINE);
  centeredBold(94, "48 kHz / 16-bit / stereo", C_NAVY, 1);
  centeredBold(114, "PDM clock: 3.072 MHz", C_BLUE, 1);
  centeredBold(132, "L=SELECT GND   R=SELECT 3V3", C_MUTED, 1);

  gfx->fillRoundRect(14, 162, 252, 176, 16, C_CARD);
  gfx->drawRoundRect(14, 162, 252, 176, 16, C_LINE);
  textAtBold(28, 182, "LEFT", C_NAVY, 2);
  textAtBold(28, 244, "RIGHT", C_NAVY, 2);

  centeredBold(314,
               pdmReady ? (sdOk ? "PDM READY  -  SD READY" : "PDM READY  -  GEEN SD")
                        : "PDM INITIALISATIE FOUT",
               pdmReady && sdOk ? C_GREEN : C_AMBER, 1);

  bool rec = isRecording();
  uint16_t buttonColor = rec ? C_RED : (pdmReady && sdOk ? C_BLUE : C_MUTED);
  gfx->fillRoundRect(ACTION_BUTTON.x, ACTION_BUTTON.y, ACTION_BUTTON.w,
                     ACTION_BUTTON.h, 18, buttonColor);
  gfx->drawRoundRect(ACTION_BUTTON.x, ACTION_BUTTON.y, ACTION_BUTTON.w,
                     ACTION_BUTTON.h, 18, buttonColor);
  centeredBold(ACTION_BUTTON.y + 22,
               rec ? "STOP WAV" : "START WAV",
               C_WHITE, 2);

  lastMeterDrawMs = 0;
  lastTimerSecond = 0xFFFFFFFFUL;
}

float clampDb(float db) {
  if (!isfinite(db)) return -90.0f;
  if (db < -90.0f) return -90.0f;
  if (db > 0.0f) return 0.0f;
  return db;
}

void readLevels(float &left, float &right) {
  portENTER_CRITICAL(&levelMux);
  left = levelLeftDb;
  right = levelRightDb;
  portEXIT_CRITICAL(&levelMux);
}

void drawOneMeter(int y, float db, uint16_t color) {
  static constexpr int X = 28;
  static constexpr int W = 224;
  static constexpr int H = 20;

  gfx->fillRoundRect(X, y, W, H, 8, C_SOFT);
  float norm = (clampDb(db) + 60.0f) / 60.0f;
  if (norm < 0.0f) norm = 0.0f;
  if (norm > 1.0f) norm = 1.0f;
  int fillW = (int)(norm * W);
  if (fillW > 3) gfx->fillRoundRect(X, y, fillW, H, 8, color);

  char dbText[20];
  snprintf(dbText, sizeof(dbText), "%5.1f dBFS", db);
  gfx->fillRect(164, y - 21, 88, 17, C_CARD);
  textAtBold(176, y - 19, dbText, C_NAVY, 1);
}

void drawDynamicUi() {
  uint32_t now = millis();
  if (now - lastMeterDrawMs >= 80) {
    lastMeterDrawMs = now;
    float left, right;
    readLevels(left, right);
    drawOneMeter(215, left, C_BLUE);
    drawOneMeter(277, right, C_GREEN);
  }

  bool rec = isRecording();
  uint32_t sec = rec ? (now - recordStartedMs) / 1000 : 0;
  if (sec != lastTimerSecond) {
    lastTimerSecond = sec;
    gfx->fillRect(74, 340, 132, 18, C_BG);
    if (rec) {
      char timer[20];
      snprintf(timer, sizeof(timer), "REC %02lu:%02lu:%02lu",
               (unsigned long)(sec / 3600),
               (unsigned long)((sec % 3600) / 60),
               (unsigned long)(sec % 60));
      centeredBold(344, timer, C_RED, 1);
    } else {
      centeredBold(344, "LIVE LEVELS", C_MUTED, 1);
    }
  }
}

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

bool initSD() {
  sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, 10000000)) return false;
  if (SD.cardType() == CARD_NONE) return false;
  if (!SD.exists("/visitescribe")) SD.mkdir("/visitescribe");
  if (!SD.exists("/visitescribe/mic-lab")) SD.mkdir("/visitescribe/mic-lab");
  return true;
}

void writeU16(File &f, uint16_t v) {
  uint8_t b[2] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF)};
  f.write(b, sizeof(b));
}

void writeU32(File &f, uint32_t v) {
  uint8_t b[4] = {
      (uint8_t)(v & 0xFF),
      (uint8_t)((v >> 8) & 0xFF),
      (uint8_t)((v >> 16) & 0xFF),
      (uint8_t)((v >> 24) & 0xFF)};
  f.write(b, sizeof(b));
}

void writeWavHeader(File &f, uint32_t dataBytes) {
  f.seek(0);
  f.write((const uint8_t *)"RIFF", 4);
  writeU32(f, 36 + dataBytes);
  f.write((const uint8_t *)"WAVE", 4);
  f.write((const uint8_t *)"fmt ", 4);
  writeU32(f, 16);
  writeU16(f, 1);  // PCM
  writeU16(f, AUDIO_CHANNELS);
  writeU32(f, AUDIO_RATE);
  writeU32(f, AUDIO_BYTES_PER_SECOND);
  writeU16(f, AUDIO_CHANNELS * (AUDIO_BITS / 8));
  writeU16(f, AUDIO_BITS);
  f.write((const uint8_t *)"data", 4);
  writeU32(f, dataBytes);
}

bool startRecording() {
  if (!sdOk || !pdmReady || !wavMutex) return false;
  if (xSemaphoreTake(wavMutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;

  if (recording) {
    xSemaphoreGive(wavMutex);
    return true;
  }

  uint32_t id = esp_random();
  snprintf(wavTmpPath, sizeof(wavTmpPath),
           "/visitescribe/mic-lab/im73_%08lx.wav.tmp", (unsigned long)id);
  snprintf(wavFinalPath, sizeof(wavFinalPath),
           "/visitescribe/mic-lab/im73_%08lx.wav", (unsigned long)id);

  wavFile = SD.open(wavTmpPath, FILE_WRITE);
  if (!wavFile) {
    xSemaphoreGive(wavMutex);
    return false;
  }

  wavDataBytes = 0;
  writeWavHeader(wavFile, 0);
  wavFile.flush();
  recording = true;
  recordStartedMs = millis();

  Serial.printf("Recording started: %s\n", wavFinalPath);
  xSemaphoreGive(wavMutex);
  drawStaticUi();
  return true;
}

void stopRecording() {
  if (!wavMutex) return;
  if (xSemaphoreTake(wavMutex, pdMS_TO_TICKS(1500)) != pdTRUE) return;

  if (!recording) {
    xSemaphoreGive(wavMutex);
    return;
  }

  // The audio task checks recording only while holding the same mutex, so once
  // we own it no further PCM can be appended after this point.
  recording = false;
  if (wavFile) {
    writeWavHeader(wavFile, wavDataBytes);
    wavFile.flush();
    wavFile.close();
    SD.remove(wavFinalPath);
    if (!SD.rename(wavTmpPath, wavFinalPath)) {
      Serial.println("WARNING: WAV rename failed; .wav.tmp retained");
    }
  }

  float seconds = (float)wavDataBytes / (float)AUDIO_BYTES_PER_SECOND;
  Serial.printf("Recording stopped: %lu bytes, %.2f s\n",
                (unsigned long)wavDataBytes, seconds);

  xSemaphoreGive(wavMutex);
  drawStaticUi();
}

bool initPdm() {
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  esp_err_t err = i2s_new_channel(&chanCfg, nullptr, &pdmRx);
  if (err != ESP_OK) {
    Serial.printf("i2s_new_channel failed: %s\n", esp_err_to_name(err));
    return false;
  }

  i2s_pdm_rx_clk_config_t clkCfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(AUDIO_RATE);
  // DSR_8S => PDM CLK = PCM rate * 64 = 3.072 MHz at 48 kHz.
  clkCfg.dn_sample_mode = I2S_PDM_DSR_8S;

  i2s_pdm_rx_slot_config_t slotCfg =
      I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);

  i2s_pdm_rx_config_t pdmCfg = {};
  pdmCfg.clk_cfg = clkCfg;
  pdmCfg.slot_cfg = slotCfg;
  pdmCfg.gpio_cfg.clk = (gpio_num_t)PDM_CLK_GPIO;
  pdmCfg.gpio_cfg.dins[0] = (gpio_num_t)PDM_DATA_GPIO;
  pdmCfg.gpio_cfg.invert_flags.clk_inv = false;

  err = i2s_channel_init_pdm_rx_mode(pdmRx, &pdmCfg);
  if (err != ESP_OK) {
    Serial.printf("i2s_channel_init_pdm_rx_mode failed: %s\n", esp_err_to_name(err));
    i2s_del_channel(pdmRx);
    pdmRx = nullptr;
    return false;
  }

  err = i2s_channel_enable(pdmRx);
  if (err != ESP_OK) {
    Serial.printf("i2s_channel_enable failed: %s\n", esp_err_to_name(err));
    i2s_del_channel(pdmRx);
    pdmRx = nullptr;
    return false;
  }

  Serial.printf("PDM RX ready: CLK GPIO%d, DATA GPIO%d, PCM %lu Hz stereo\n",
                PDM_CLK_GPIO, PDM_DATA_GPIO, (unsigned long)AUDIO_RATE);
  Serial.println("Expected PDM clock: 3.072 MHz");
  return true;
}

void audioTask(void *parameter) {
  (void)parameter;
  // 4096 bytes = ~21.3 ms of 48k/16-bit/stereo PCM.
  alignas(4) static int16_t pcm[2048];
  audioTaskRunning = true;

  for (;;) {
    size_t bytesRead = 0;
    esp_err_t err = i2s_channel_read(
        pdmRx, pcm, sizeof(pcm), &bytesRead, pdMS_TO_TICKS(500));
    if (err != ESP_OK || bytesRead < 4) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    size_t sampleCount = bytesRead / sizeof(int16_t);
    sampleCount &= ~((size_t)1);  // whole stereo frames only
    size_t frames = sampleCount / 2;

    uint64_t sumSqL = 0;
    uint64_t sumSqR = 0;

    // ESP-IDF PDM RX stereo returns the RIGHT slot first. Normalize the WAV to
    // conventional interleaved LEFT,RIGHT. With our wiring LEFT=SELECT GND and
    // RIGHT=SELECT 3V3.
    for (size_t i = 0; i < sampleCount; i += 2) {
      int16_t right = pcm[i];
      int16_t left = pcm[i + 1];
      pcm[i] = left;
      pcm[i + 1] = right;

      int32_t l = left;
      int32_t r = right;
      sumSqL += (uint64_t)((int64_t)l * l);
      sumSqR += (uint64_t)((int64_t)r * r);
    }

    float rmsL = frames ? sqrtf((float)sumSqL / (float)frames) : 0.0f;
    float rmsR = frames ? sqrtf((float)sumSqR / (float)frames) : 0.0f;
    float dbL = rmsL > 0.5f ? 20.0f * log10f(rmsL / 32768.0f) : -90.0f;
    float dbR = rmsR > 0.5f ? 20.0f * log10f(rmsR / 32768.0f) : -90.0f;

    portENTER_CRITICAL(&levelMux);
    levelLeftDb = clampDb(dbL);
    levelRightDb = clampDb(dbR);
    portEXIT_CRITICAL(&levelMux);

    // Keep the check and the write under the same lock; stopRecording() uses
    // this lock too, so WAV finalization can never race a late audio write.
    if (xSemaphoreTake(wavMutex, portMAX_DELAY) == pdTRUE) {
      if (recording && wavFile) {
        size_t pcmBytes = sampleCount * sizeof(int16_t);
        size_t written = wavFile.write((uint8_t *)pcm, pcmBytes);
        wavDataBytes += (uint32_t)written;
        if (written != pcmBytes) {
          Serial.printf("WARNING: SD short write %u/%u\n",
                        (unsigned)written, (unsigned)pcmBytes);
        }
      }
      xSemaphoreGive(wavMutex);
    }
  }
}

void handleTouch(uint16_t x, uint16_t y) {
  if (!ACTION_BUTTON.contains(x, y)) return;
  if (isRecording()) stopRecording();
  else startRecording();
}

void pollBootButton() {
  bool down = digitalRead(VISITESCRIBE_BOOT_GPIO) == LOW;
  uint32_t now = millis();
  if (down && !bootWasDown) bootDownSince = now;
  if (down && bootWasDown && bootDownSince && now - bootDownSince > 1200) {
    if (isRecording()) stopRecording();
    bootDownSince = 0;
  }
  if (!down) bootDownSince = 0;
  bootWasDown = down;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("VisiteScribe IM73D122 dual-PDM laboratory firmware");

  pinMode(VISITESCRIBE_BOOT_GPIO, INPUT_PULLUP);
  wavMutex = xSemaphoreCreateMutex();

  if (!gfx->begin()) Serial.println("ERROR: AMOLED init failed");
  gfx->fillScreen(C_BG);

  initTouch();
  sdOk = initSD();
  Serial.printf("microSD: %s\n", sdOk ? "OK" : "NOT FOUND");

  pdmReady = initPdm();
  if (pdmReady) {
    BaseType_t taskOk = xTaskCreatePinnedToCore(
        audioTask, "pdm-audio", 8192, nullptr, 5, &audioTaskHandle, 0);
    if (taskOk != pdPASS) {
      pdmReady = false;
      Serial.println("ERROR: could not create PDM audio task");
    }
  }

  drawStaticUi();
}

void loop() {
  uint16_t x = 0, y = 0;
  bool touchDown = readTouch(x, y);
  if (touchDown && !touchWasDown) handleTouch(x, y);
  touchWasDown = touchDown;

  pollBootButton();
  drawDynamicUi();
  delay(10);
}
