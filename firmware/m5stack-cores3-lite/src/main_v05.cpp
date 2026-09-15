// VisiteScribe CoreS3-Lite v0.5
//
// v0.5 keeps the working v0.4 recorder/SD stack and adds a conservative
// speech-oriented microphone profile for the onboard ES7210 codec:
// - MIC1/MIC2 analog PGA: 37.5 dB (from M5Unified's 33 dB default)
// - ADC1/ADC2 digital gain: +6 dB
//
// The ES7210 is reconfigured every time M5.Mic.begin() is called, so this
// profile is applied immediately after each transition into active capture
// (initial start, privacy resume, next patient, append/resume).

#define VISITESCRIBE_V04_SETUP_NAME setup_v04
#define VISITESCRIBE_V04_LOOP_NAME loop_v04
#include "main_v04.cpp"
#undef VISITESCRIBE_V04_SETUP_NAME
#undef VISITESCRIBE_V04_LOOP_NAME

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

void setup() {
  setup_v04();
  Serial.println("VisiteScribe CoreS3-Lite v0.5; speech profile=37.5dB analog +6dB digital");
}

void loop() {
  loop_v04();

  // M5.Mic.begin() happens inside the inherited recorder actions. Apply our
  // codec overrides once per active capture epoch, after that initialization.
  if (captureRunning) {
    if (!speechGainApplied) {
      speechGainOk = applySpeechMicProfile();
      speechGainApplied = true;
    }
  } else {
    speechGainApplied = false;
  }
}
