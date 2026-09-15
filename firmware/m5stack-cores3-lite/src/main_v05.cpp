// VisiteScribe CoreS3-Lite v0.5.1
//
// v0.5 keeps the working v0.4 recorder/SD stack and adds a conservative
// speech-oriented microphone profile for the onboard ES7210 codec:
// - MIC1/MIC2 analog PGA: 37.5 dB (from M5Unified's 33 dB default)
// - ADC1/ADC2 digital gain: +6 dB
//
// v0.5.1 also extends manual SYNC to three Wi-Fi profiles. The local
// wifi_secrets.h may define VISITESCRIBE_WIFI_SSID_3 and
// VISITESCRIBE_WIFI_PASSWORD_3; empty profiles are skipped automatically.

#ifndef VISITESCRIBE_V05_SETUP_NAME
#define VISITESCRIBE_V05_SETUP_NAME setup
#define VISITESCRIBE_V05_SETUP_NAME_LOCAL 1
#endif
#ifndef VISITESCRIBE_V05_LOOP_NAME
#define VISITESCRIBE_V05_LOOP_NAME loop
#define VISITESCRIBE_V05_LOOP_NAME_LOCAL 1
#endif

#define VISITESCRIBE_V04_SETUP_NAME setup_v04
#define VISITESCRIBE_V04_LOOP_NAME loop_v04
#include "main_v04.cpp"
#undef VISITESCRIBE_V04_SETUP_NAME
#undef VISITESCRIBE_V04_LOOP_NAME

#ifndef VISITESCRIBE_WIFI_SSID_3
#define VISITESCRIBE_WIFI_SSID_3 ""
#endif
#ifndef VISITESCRIBE_WIFI_PASSWORD_3
#define VISITESCRIBE_WIFI_PASSWORD_3 ""
#endif

static constexpr uint8_t ES7210_ADDR = 0x40;
static constexpr uint8_t ES7210_MIC_GAIN_37_5DB = 0x1E; // 0x10 | gain code 14
static constexpr uint8_t ES7210_ADC_PLUS_6DB = 0xCB;    // 0xBF = 0 dB, 0.5 dB/step

static bool speechGainApplied = false;
static bool speechGainOk = false;

static bool applySpeechMicProfile() {
  bool ok = true;

  // Analog PGA for the two onboard microphone channels.
  ok &= M5.In_I2C.writeRegister8(ES7210_ADDR, 0x43, ES7210_MIC_GAIN_37_5DB, I2C_HZ); // MIC1_GAIN
  ok &= M5.In_I2C.writeRegister8(ES7210_ADDR, 0x44, ES7210_MIC_GAIN_37_5DB, I2C_HZ); // MIC2_GAIN

  // Conservative digital boost. Keep substantial headroom for close speech
  // and handling transients; further normalisation can happen downstream.
  ok &= M5.In_I2C.writeRegister8(ES7210_ADDR, 0x1B, ES7210_ADC_PLUS_6DB, I2C_HZ); // ADC1 volume
  ok &= M5.In_I2C.writeRegister8(ES7210_ADDR, 0x1C, ES7210_ADC_PLUS_6DB, I2C_HZ); // ADC2 volume

  const uint8_t mic1 = M5.In_I2C.readRegister8(ES7210_ADDR, 0x43, I2C_HZ);
  const uint8_t mic2 = M5.In_I2C.readRegister8(ES7210_ADDR, 0x44, I2C_HZ);
  const uint8_t adc1 = M5.In_I2C.readRegister8(ES7210_ADDR, 0x1B, I2C_HZ);
  const uint8_t adc2 = M5.In_I2C.readRegister8(ES7210_ADDR, 0x1C, I2C_HZ);

  ok &= (mic1 == ES7210_MIC_GAIN_37_5DB);
  ok &= (mic2 == ES7210_MIC_GAIN_37_5DB);
  ok &= (adc1 == ES7210_ADC_PLUS_6DB);
  ok &= (adc2 == ES7210_ADC_PLUS_6DB);

  Serial.printf("AUDIO: speech gain %s mic1=0x%02X mic2=0x%02X adc1=0x%02X adc2=0x%02X (37.5dB analog, +6dB digital)\n",
                ok ? "OK" : "FAIL", mic1, mic2, adc1, adc2);
  return ok;
}

static const char* wifiSsidV05(uint8_t index) {
  switch (index) {
    case 0: return VISITESCRIBE_WIFI_SSID_1;
    case 1: return VISITESCRIBE_WIFI_SSID_2;
    case 2: return VISITESCRIBE_WIFI_SSID_3;
    default: return "";
  }
}

static const char* wifiPasswordV05(uint8_t index) {
  switch (index) {
    case 0: return VISITESCRIBE_WIFI_PASSWORD_1;
    case 1: return VISITESCRIBE_WIFI_PASSWORD_2;
    case 2: return VISITESCRIBE_WIFI_PASSWORD_3;
    default: return "";
  }
}

static bool wifiProfileConfiguredV05(uint8_t index) {
  const char* ssid = wifiSsidV05(index);
  return ssid && ssid[0] != '\0';
}

static int nextWifiProfileV05(uint8_t startIndex) {
  for (uint8_t i = startIndex; i < 3; ++i) {
    if (wifiProfileConfiguredV05(i)) return i;
  }
  return -1;
}

static void startWifiAttemptV05(uint8_t startIndex) {
#if VISITESCRIBE_WIFI_CONFIGURED
  const int index = nextWifiProfileV05(startIndex);
  if (index < 0) {
    wifiOff();
    syncPhase = SyncPhase::FAILED;
    screenDirty = true;
    Serial.println("WIFI: no further configured profiles");
    return;
  }

  syncNetwork = static_cast<uint8_t>(index);
  WiFi.disconnect(true, true);
  delay(20);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.begin(wifiSsidV05(syncNetwork), wifiPasswordV05(syncNetwork));

  // The inherited UI only has two generic CONNECTING enum values. Use the
  // first for profile 1 and the second for profiles 2/3; syncNetwork carries
  // the real zero-based profile index.
  syncPhase = syncNetwork == 0 ? SyncPhase::CONNECTING_1 : SyncPhase::CONNECTING_2;
  syncAttemptStartedMs = millis();
  screenDirty = true;
  Serial.printf("WIFI: trying profile %u SSID=%s\n",
                static_cast<unsigned>(syncNetwork + 1), wifiSsidV05(syncNetwork));
#else
  (void)startIndex;
  syncPhase = SyncPhase::NO_CREDENTIALS;
  screenDirty = true;
#endif
}

static void serviceSyncV05() {
  if (state != AppState::SYNC) return;
  if (syncPhase != SyncPhase::CONNECTING_1 && syncPhase != SyncPhase::CONNECTING_2) return;

#if VISITESCRIBE_WIFI_CONFIGURED
  // beginSync()/OPNIEUW are inherited from v0.2 and initially launch profile
  // 1. If that profile is empty, skip it immediately rather than waiting 6.5s.
  if (!wifiProfileConfiguredV05(syncNetwork)) {
    startWifiAttemptV05(syncNetwork + 1);
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    syncPhase = SyncPhase::CONNECTED;
    screenDirty = true;
    Serial.printf("WIFI: connected profile %u SSID=%s IP=%s\n",
                  static_cast<unsigned>(syncNetwork + 1),
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    return;
  }

  if (millis() - syncAttemptStartedMs >= WIFI_ATTEMPT_MS) {
    const int next = nextWifiProfileV05(syncNetwork + 1);
    if (next >= 0) {
      startWifiAttemptV05(static_cast<uint8_t>(next));
    } else {
      wifiOff();
      syncPhase = SyncPhase::FAILED;
      screenDirty = true;
      Serial.println("WIFI: all configured profiles failed");
    }
  }
#else
  syncPhase = SyncPhase::NO_CREDENTIALS;
  screenDirty = true;
#endif
}

void VISITESCRIBE_V05_SETUP_NAME() {
  setup_v04();
  Serial.printf("VisiteScribe CoreS3-Lite v0.5.1; speech=37.5dB+6dB; wifi profiles=%d%d%d\n",
                wifiProfileConfiguredV05(0) ? 1 : 0,
                wifiProfileConfiguredV05(1) ? 1 : 0,
                wifiProfileConfiguredV05(2) ? 1 : 0);
}

void VISITESCRIBE_V05_LOOP_NAME() {
  // Reproduce the proven v0.3/v0.4 runtime loop, but use the v0.5 Wi-Fi
  // sequencer instead of the inherited two-profile serviceSync().
  serviceInputsV03();

  // M5.Mic.begin() happens inside recorder actions reached by serviceInputsV03.
  // Apply the codec overrides immediately after each transition into capture.
  if (captureRunning) {
    if (!speechGainApplied) {
      speechGainOk = applySpeechMicProfile();
      speechGainApplied = true;
    }
  } else {
    speechGainApplied = false;
  }

  serviceAudio();
  serviceSyncV05();

  if (millis() - lastBatteryRefreshMs > 10000) {
    int old = batteryPct;
    refreshBattery();
    if (old != batteryPct && state == AppState::STATUS) screenDirty = true;
  }

  if (state == AppState::FINISHED && millis() - finishedAtMs >= FINISHED_AUTO_HOME_MS) {
    goHome();
  }

  render();
  serviceDisplayPower();
  delay(5);
}

#ifdef VISITESCRIBE_V05_SETUP_NAME_LOCAL
#undef VISITESCRIBE_V05_SETUP_NAME_LOCAL
#undef VISITESCRIBE_V05_SETUP_NAME
#endif
#ifdef VISITESCRIBE_V05_LOOP_NAME_LOCAL
#undef VISITESCRIBE_V05_LOOP_NAME_LOCAL
#undef VISITESCRIBE_V05_LOOP_NAME
#endif
