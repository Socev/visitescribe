// VisiteScribe CoreS3-Lite v0.4
//
// v0.3 fixed touch and PWR input. v0.4 hardens microSD startup on CoreS3-Lite:
// - explicitly enables the AXP2101 ALDO4 3.3V rail used by the TF slot
// - uses the official CoreS3 SPI pins (SCK36/MISO35/MOSI37/CS4)
// - retries at 25/10/4/1 MHz
// - verifies that the mounted card is writable before declaring SD OK
// - emits concise serial diagnostics when mounting fails

#define VISITESCRIBE_V03_SETUP_NAME setup_v03
#define VISITESCRIBE_V03_LOOP_NAME loop_v03
#include "main_v03.cpp"
#undef VISITESCRIBE_V03_SETUP_NAME
#undef VISITESCRIBE_V03_LOOP_NAME

static uint32_t sdMountedHz = 0;

static bool sdWriteProbe() {
  if (!SD.exists("/visitescribe")) {
    if (!SD.mkdir("/visitescribe")) {
      Serial.println("SD: mkdir /visitescribe failed");
      return false;
    }
  }

  const char* probe = "/visitescribe/.write_test";
  if (SD.exists(probe)) SD.remove(probe);
  File f = SD.open(probe, FILE_WRITE);
  if (!f) {
    Serial.println("SD: write probe open failed");
    return false;
  }
  const uint8_t marker[4] = {'V','S','0','4'};
  const size_t n = f.write(marker, sizeof(marker));
  f.flush();
  f.close();
  SD.remove(probe);
  if (n != sizeof(marker)) {
    Serial.printf("SD: write probe short write %u/%u\n", (unsigned)n, (unsigned)sizeof(marker));
    return false;
  }
  return true;
}

static bool trySdMount(uint32_t hz) {
  Serial.printf("SD: trying %lu MHz on SCK=%d MISO=%d MOSI=%d CS=%d\n",
                (unsigned long)(hz / 1000000UL), SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  SD.end();
  SPI.end();
  delay(20);

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(10);
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  delay(10);

  if (!SD.begin(SD_CS, SPI, hz)) {
    Serial.println("SD: SD.begin failed");
    return false;
  }

  const uint8_t type = SD.cardType();
  if (type == CARD_NONE) {
    Serial.println("SD: mounted but CARD_NONE");
    SD.end();
    return false;
  }

  if (!sdWriteProbe()) {
    SD.end();
    return false;
  }

  sdMountedHz = hz;
  const uint64_t bytes = SD.cardSize();
  Serial.printf("SD: OK type=%u size=%llu MB speed=%lu MHz\n",
                (unsigned)type,
                (unsigned long long)(bytes / (1024ULL * 1024ULL)),
                (unsigned long)(hz / 1000000UL));
  return true;
}

static bool robustSdMount() {
  // CoreS3 family: AXP2101 ALDO4 is the 3.3V rail for the TF slot.
  // Make this explicit because the generic esp32-s3-devkitc-1 target can make
  // board bring-up less deterministic than M5Stack's board package.
  const bool axpOk = M5.Power.Axp2101.begin();
  Serial.printf("SD: AXP2101=%s; enabling ALDO4 3.3V\n", axpOk ? "OK" : "FAIL");
  if (axpOk) {
    M5.Power.Axp2101.setALDO4(3300);
    delay(120);
  }

  Serial.printf("SD: M5 pins clk=%d miso=%d mosi=%d cs=%d hasSD=%d\n",
                (int)M5.getPin(m5::pin_name_t::sd_spi_sclk),
                (int)M5.getPin(m5::pin_name_t::sd_spi_miso),
                (int)M5.getPin(m5::pin_name_t::sd_spi_mosi),
                (int)M5.getPin(m5::pin_name_t::sd_spi_cs),
                M5.hasSD() ? 1 : 0);

  static constexpr uint32_t speeds[] = {
    25000000UL,
    10000000UL,
    4000000UL,
    1000000UL,
  };
  for (uint32_t hz : speeds) {
    if (trySdMount(hz)) return true;
  }

  Serial.println("SD: all mount attempts failed");
  return false;
}

void setup() {
  setup_v03();

  // The old path already tried once. Only do the slower/power-aware recovery
  // when that first official 25 MHz mount failed.
  if (!sdOk) {
    Serial.println("SD: initial mount failed; starting v0.4 recovery");
    sdOk = robustSdMount();
    screenDirty = true;
    render(true);
  } else {
    sdMountedHz = SD_HZ;
    Serial.println("SD: initial official 25 MHz mount already OK");
  }

  Serial.printf("VisiteScribe CoreS3-Lite v0.4; sd=%s speed=%lu MHz\n",
                sdOk ? "OK" : "FAIL",
                (unsigned long)(sdMountedHz / 1000000UL));
}

void loop() {
  loop_v03();
}
