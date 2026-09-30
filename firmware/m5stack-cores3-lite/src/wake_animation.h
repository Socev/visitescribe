#pragma once
// "Brian wordt wakker" (firmware 0.12.0), both boards.
//
// The first press on a dark screen only wakes Brian. At rest that is now a
// short animation (2 s): eyes closed, a yawn, arms up to stretch, eyes open.
// During a recording it is a 1 s "listening" Brian, then the recording screen.
// Every press during the animation is swallowed, so a double click meant as
// "menu" can no longer start a recording.
//
// Included by main_v03.cpp: needs the v0.2 drawing helpers and serviceAudio().
// While recording, the microphone keeps running; serviceAudio() is called
// between frames so no audio is ever lost to the animation.

static void vsWakeDrawBrian(int cx, int cy, float s, float stretch, float eyes,
                            float mouth, bool listening, int t) {
  const uint16_t ink = 0x18E3, accent = C_BLUE, pale = 0xEF7D;
  auto S = [s](float v) { return (int)(v * s + 0.5f); };

  // Clear the figure area only.
  M5.Display.fillRect(cx - S(80), cy - S(52), S(160), S(170), C_WHITE);

  const int lift = S(4 * stretch);                 // whole body rises a little
  const int headY = cy - lift;

  // Antenna.
  M5.Display.drawLine(cx, headY - S(12), cx, headY - S(4), ink);
  M5.Display.fillCircle(cx, headY - S(15), S(4), accent);

  // Head.
  M5.Display.fillRoundRect(cx - S(40), headY, S(80), S(52), S(14), pale);
  M5.Display.drawRoundRect(cx - S(40), headY, S(80), S(52), S(14), ink);
  M5.Display.fillRoundRect(cx - S(31), headY + S(9), S(62), S(28), S(9), C_WHITE);
  M5.Display.drawRoundRect(cx - S(31), headY + S(9), S(62), S(28), S(9), ink);

  // Eyes: 0 = closed line, 1 = fully open.
  const int ex = S(15), ey = headY + S(23);
  if (eyes < 0.25f) {
    M5.Display.drawFastHLine(cx - ex - S(6), ey, S(12), ink);
    M5.Display.drawFastHLine(cx + ex - S(6), ey, S(12), ink);
  } else {
    const int r = std::max(1, S(5 * eyes));
    M5.Display.fillCircle(cx - ex, ey, r, accent);
    M5.Display.fillCircle(cx + ex, ey, r, accent);
  }

  // Mouth: a yawn is an open oval, otherwise a small smile.
  const int my = headY + S(44);
  if (mouth > 0.1f) {
    M5.Display.fillEllipse(cx, my - S(2), S(6), std::max(1, S(6 * mouth)), ink);
  } else {
    M5.Display.drawLine(cx - S(7), my - S(3), cx, my, ink);
    M5.Display.drawLine(cx, my, cx + S(7), my - S(3), ink);
  }

  // Body.
  const int bodyY = headY + S(56);
  M5.Display.fillRoundRect(cx - S(38), bodyY, S(76), S(46), S(9), C_WHITE);
  M5.Display.drawRoundRect(cx - S(38), bodyY, S(76), S(46), S(9), ink);
  M5.Display.drawLine(cx, bodyY + S(2), cx, bodyY + S(42), C_LINE);
  M5.Display.drawCircle(cx, bodyY + S(28), S(6), accent);

  // Arms: down at rest, up and out while stretching.
  const float a = stretch;                          // 0 = down, 1 = up
  const int sx = S(38), sy = bodyY + S(8);
  const int armLen = S(34);
  const float ang = 1.4f - 2.6f * a;                // radians below horizontal
  const int dx = (int)(armLen * cosf(ang)), dy = (int)(armLen * sinf(ang));
  for (int w = -1; w <= 1; ++w) {
    M5.Display.drawLine(cx - sx, sy + w, cx - sx - dx, sy + dy + w, ink);
    M5.Display.drawLine(cx + sx, sy + w, cx + sx + dx, sy + dy + w, ink);
  }
  M5.Display.fillCircle(cx - sx - dx, sy + dy, S(4), pale);
  M5.Display.fillCircle(cx + sx + dx, sy + dy, S(4), pale);

  // Listening: a small pulse under Brian.
  if (listening) {
    const int y = bodyY + S(58);
    const int x0 = cx - S(50), w = S(100);
    M5.Display.drawFastHLine(x0, y, w, C_LINE);
    const int px = x0 + (t * S(7)) % w;
    M5.Display.drawLine(px - S(6), y, px - S(2), y, accent);
    M5.Display.drawLine(px - S(2), y, px, y - S(7), accent);
    M5.Display.drawLine(px, y - S(7), px + S(3), y + S(6), accent);
    M5.Display.drawLine(px + S(3), y + S(6), px + S(6), y, accent);
  }
}

// Swallows every button press that arrives while the animation plays.
static void vsWakeSwallowPresses() {
#if VS_STICK
  vsStickPollButtons();
  vsStickTakeA();
  vsStickTakeB();
#else
  if (axp2101DirectOk) M5.Power.Axp2101.getPekPress();
#endif
}

static void vsPlayWakeAnimation(bool recording) {
  const uint32_t totalMs = recording ? 1000 : 2000;
  const int frames = recording ? 10 : 20;
  const uint32_t frameMs = totalMs / frames;
#if VS_STICK
  const float s = 0.85f;
  const int cx = SCREEN_W / 2, cy = 70;
#else
  const float s = 1.0f;
  const int cx = SCREEN_W / 2, cy = 72;
#endif
  M5.Display.fillScreen(C_WHITE);
  if (recording) {
    M5.Display.fillCircle(14, 12, 5, C_RED);
    M5.Display.setTextDatum(middle_left);
    M5.Display.setTextColor(C_RED);
    M5.Display.setTextSize(1);
    M5.Display.drawString("Neemt op", 24, 12);
  }
  const uint32_t started = millis();
  for (int f = 0; f < frames; ++f) {
    const float p = (float)f / (float)(frames - 1);   // 0..1
    float stretch = 0, eyes = 1, mouth = 0;
    if (!recording) {
      // 0-30 % asleep, 30-55 % yawn, 55-85 % stretch, then awake and smiling.
      if (p < 0.30f) { eyes = 0; }
      else if (p < 0.55f) { eyes = 0.2f; mouth = (p - 0.30f) / 0.25f; }
      else if (p < 0.85f) { eyes = 0.6f; mouth = 0.3f; stretch = (p - 0.55f) / 0.30f; }
      else { eyes = 1; stretch = 1.0f - (p - 0.85f) / 0.15f; }
    }
    vsWakeDrawBrian(cx, cy, s, stretch, eyes, mouth, recording, f);
    if (!recording && p >= 0.55f) centeredText(SCREEN_H - 20, "Goeiemorgen!", C_VIOLET, 1);
    const uint32_t until = started + (uint32_t)(f + 1) * frameMs;
    while ((int32_t)(millis() - until) < 0) {
      if (captureRunning) serviceAudio();     // a recording never waits
      vsWakeSwallowPresses();
      delay(4);
    }
  }
  vsWakeSwallowPresses();
  noteActivity();
  screenDirty = true;
}
