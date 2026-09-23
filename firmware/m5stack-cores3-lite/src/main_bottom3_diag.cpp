#include <Arduino.h>
#include <M5Unified.h>

static void showColor(uint32_t color, const char* name) {
  Serial.printf("BOTTOM3 DIAG: draw %s\n", name);
  Serial.flush();
  M5.Display.fillScreen(color);
  M5.Display.setTextColor(TFT_BLACK, color);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);
  M5.Display.drawString(name, M5.Display.width() / 2, M5.Display.height() / 2);
  delay(1200);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("BOTTOM3 DIAG: boot");
  Serial.printf("BOTTOM3 DIAG: before M5.begin free_heap=%u psram=%u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize());
  Serial.flush();

  auto cfg = M5.config();
  Serial.println("BOTTOM3 DIAG: calling M5.begin");
  Serial.flush();
  M5.begin(cfg);

  Serial.println("BOTTOM3 DIAG: M5.begin returned");
  Serial.printf("BOTTOM3 DIAG: board=%d display=%dx%d rotation=%u battery=%d%% charging=%d\n",
                (int)M5.getBoard(),
                M5.Display.width(), M5.Display.height(),
                (unsigned)M5.Display.getRotation(),
                M5.Power.getBatteryLevel(),
                M5.Power.isCharging() ? 1 : 0);
  Serial.flush();

  M5.Display.setRotation(1);
  M5.Display.setBrightness(255);
  Serial.printf("BOTTOM3 DIAG: display forced rotation=1 brightness=255 size=%dx%d\n",
                M5.Display.width(), M5.Display.height());
  Serial.flush();

  showColor(TFT_RED,   "RED");
  showColor(TFT_GREEN, "GREEN");
  showColor(TFT_BLUE,  "BLUE");
  showColor(TFT_WHITE, "WHITE");

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);
  M5.Display.drawString("BOTTOM3 DIAG OK", M5.Display.width() / 2, 100);
  M5.Display.setTextSize(1);
  M5.Display.drawString("serial blijft loggen", M5.Display.width() / 2, 135);

  Serial.println("BOTTOM3 DIAG: completed display test; entering loop");
  Serial.flush();
}

void loop() {
  static uint32_t last = 0;
  M5.update();
  if (millis() - last >= 2000) {
    last = millis();
    Serial.printf("BOTTOM3 DIAG: alive t=%lus battery=%d%% charging=%d display=%dx%d\n",
                  (unsigned long)(millis() / 1000),
                  M5.Power.getBatteryLevel(),
                  M5.Power.isCharging() ? 1 : 0,
                  M5.Display.width(), M5.Display.height());
    Serial.flush();
  }
  delay(20);
}
