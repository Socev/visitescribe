#pragma once
// Brian on the M5Stack StickS3: every screen in portrait (135 x 240), and the
// two-button replacement for touch. Included by main_v02.cpp in place of the
// CoreS3 screen code (VS_STICK builds only), so it defines the same functions:
// centeredText, drawHeader, drawTouchButton, drawPwrHints, zone,
// showBrianBootAnimation, drawHome ... drawError, updateRecordingDynamic and
// render. The state machine, recorder and sync engine are the CoreS3 ones.
//
// Buttons (see board.h):
//   front button (BtnA)  = the CoreS3 PWR key: 1x start/stop, 2x menu /
//                          next patient / marker, cycles the 10 s chooser.
//   side button (BtnB)   = replaces touch. On screens with choices it moves
//                          the selection; the front button then chooses.
//
// A choice on a StickS3 screen is the same action as tapping the matching
// CoreS3 touch button: vsStickActivate() hands the centre of that button to
// the CoreS3 touch handler. Nothing about what a choice does is duplicated.

// ---------------------------------------------------------------------------
// basic drawing helpers
// ---------------------------------------------------------------------------

static constexpr int STICK_HINT_Y = 204;          // hint block: 204..240

void centeredText(int y, const char* text, uint16_t color, uint8_t size = 1) {
  M5.Display.setTextColor(color);
  M5.Display.setTextSize(size);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(text, SCREEN_W / 2, y);
}

// Word-wrapped, centred text. Returns the y below the last line drawn.
static int stickWrapped(int y, const char* text, uint16_t color, uint8_t size = 1,
                        uint8_t maxLines = 3) {
  if (!text || !text[0]) return y;
  const int charW = 6 * size;
  const int lineH = 8 * size + 4;
  const size_t perLine = (SCREEN_W - 6) / charW;
  String rest(text);
  uint8_t lines = 0;
  while (rest.length() && lines < maxLines) {
    String line;
    if (rest.length() <= perLine) {
      line = rest;
      rest = "";
    } else {
      int cut = -1;
      for (int i = (int)perLine; i > 0; --i) {
        if (rest[i] == ' ') { cut = i; break; }
      }
      if (cut <= 0) cut = (int)perLine;   // one long word (an e-mail address)
      line = rest.substring(0, cut);
      rest = rest.substring(cut);
      rest.trim();
      if (lines + 1 == maxLines && rest.length()) {
        if (line.length() > perLine - 2) line = line.substring(0, perLine - 2);
        line += "..";
        rest = "";
      }
    }
    centeredText(y + lineH / 2, line.c_str(), color, size);
    y += lineH;
    ++lines;
  }
  return y;
}

static void stickClearBody() {
  M5.Display.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_WHITE);
}

// Kept for the few inherited callers; the StickS3 never shows these zones.
void zone(const Rect& r, const char* title, uint16_t fill, uint16_t fg,
          const char* subtitle = nullptr) {
  (void)r; (void)title; (void)fill; (void)fg; (void)subtitle;
}

static void drawTouchButton(const Rect& r, const char* title,
                            const char* subtitle = nullptr,
                            bool primary = false, bool selected = false) {
  (void)r; (void)title; (void)subtitle; (void)primary; (void)selected;
}

// The two hint lines at the bottom: what the front button does.
static void drawPwrHints(const char* first, const char* second = nullptr) {
  M5.Display.fillRect(0, STICK_HINT_Y, SCREEN_W, SCREEN_H - STICK_HINT_Y, C_WHITE);
  M5.Display.drawFastHLine(8, STICK_HINT_Y, SCREEN_W - 16, C_LINE);
  if (second) {
    centeredText(STICK_HINT_Y + 11, first, C_NAVY, 1);
    centeredText(STICK_HINT_Y + 26, second, C_GREY, 1);
  } else {
    centeredText(STICK_HINT_Y + 18, first, C_NAVY, 1);
  }
}

void drawHeader(const char* status, const char* sub = nullptr) {
  M5.Display.fillRect(0, 0, SCREEN_W, HEADER_H, C_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(1);

  char batt[12];
  if (batteryPct < 0) snprintf(batt, sizeof(batt), "--%%");
  else snprintf(batt, sizeof(batt), "%s%d%%", batteryCharging ? "+" : "", batteryPct);
  const int battW = (int)strlen(batt) * 6;

  // Top-left: the linked OurMind account on HOME, otherwise Brian. Shortened
  // so it never runs into the battery figure.
  uint16_t nameColor = C_NAVY;
  const char* name = (state == AppState::HOME && vsHomeNameHook)
      ? vsHomeNameHook(nameColor) : nullptr;
  if (!name || !name[0]) { name = "Brian"; nameColor = C_NAVY; }
  const size_t maxChars = (size_t)((SCREEN_W - battW - 10) / 6);
  char shown[40];
  if (strlen(name) <= maxChars) snprintf(shown, sizeof(shown), "%s", name);
  else snprintf(shown, sizeof(shown), "%.*s..", (int)(maxChars - 2), name);
  M5.Display.setTextColor(nameColor);
  M5.Display.drawString(shown, 3, 3);

  M5.Display.setTextDatum(top_right);
  M5.Display.setTextColor((batteryPct >= 0 && batteryPct <= 15) ? C_AMBER : C_GREY);
  M5.Display.drawString(batt, SCREEN_W - 3, 3);
  M5.Display.drawFastHLine(0, HEADER_H - 1, SCREEN_W, C_LINE);

  // The CoreS3 puts a title in its (taller) header; here it is the first line
  // of the body.
  if (status && status[0]) {
    M5.Display.fillRect(0, HEADER_H, SCREEN_W, 22, C_WHITE);
    centeredText(HEADER_H + 11, status, C_NAVY, strlen(status) <= 11 ? 2 : 1);
  }
  if (sub && sub[0]) centeredText(HEADER_H + 26, sub, C_GREY, 1);
}

// Below a screen title drawn at HEADER_H + 11.
static constexpr int STICK_BODY_Y = 42;

// ---------------------------------------------------------------------------
// free space (the StickS3 records into ~4 MB of flash)
// ---------------------------------------------------------------------------

// 16 kbit/s Opus plus Ogg framing, rounded up.
static constexpr uint32_t VS_STICK_BYTES_PER_SECOND = 2200;
// Kept free for the sync's own files, the logbook and the file system.
static constexpr uint32_t VS_STICK_RESERVE_BYTES = 96U * 1024U;
static int64_t vsStickFreeCache = -1;
static uint32_t vsStickFreeAt = 0;

static int64_t vsStickFreeBytes(bool refresh = false) {
  if (!sdOk) return -1;
  if (refresh || vsStickFreeCache < 0 || millis() - vsStickFreeAt >= 10000UL) {
    vsStickFreeCache = (int64_t)SD.totalBytes() - (int64_t)SD.usedBytes();
    vsStickFreeAt = millis();
  }
  return vsStickFreeCache;
}

// Whole minutes of recording that still fit.
static uint32_t vsStickMinutesLeft(bool refresh = false) {
  const int64_t free = vsStickFreeBytes(refresh);
  if (free <= (int64_t)VS_STICK_RESERVE_BYTES) return 0;
  return (uint32_t)((free - VS_STICK_RESERVE_BYTES) / (VS_STICK_BYTES_PER_SECOND * 60U));
}

static bool vsStickStoppedForSpace = false;   // shown on the Opgeslagen screen

// Recordings waiting to be sent, as shown on HOME. -1 = not counted yet.
static int32_t vsStickPendingCache = -1;
static uint32_t vsStickPendingDueAt = 0;
static void vsStickPendingInvalidate(uint32_t delayMs = 1500) {
  vsStickPendingCache = -1;
  vsStickPendingDueAt = millis() + delayMs;
}

// ---------------------------------------------------------------------------
// choices on a screen (replaces touch)
// ---------------------------------------------------------------------------

struct VsStickOption {
  const char* label;
  Rect target;          // the CoreS3 touch button this choice stands for
  bool primary;
};

static uint8_t vsStickFocus = 0;
static uint32_t vsStickFocusKey = 0xFFFFFFFFUL;

// The choices of the current screen, in the order the side button walks them.
static uint8_t vsStickOptions(VsStickOption* out) {
  uint8_t n = 0;
  auto add = [&](const char* label, const Rect& r, bool primary = false) {
    out[n++] = VsStickOption{label, r, primary};
  };
  switch (state) {
    case AppState::MENU:
      add("Synchroniseer", THREE_TOP, true);
      add("Apparaatstatus", THREE_MIDDLE);
      add("Terug", THREE_BOTTOM);
      break;
    case AppState::STATUS:
      add("Details", STATUS_DETAILS);
      add("Terug", STATUS_BACK);
      break;
    case AppState::DETAILS: {
      const bool a1 = vsDetailsActionHook && vsDetailsActionLabel;
      const bool a2 = vsDetailsAction2Hook && vsDetailsAction2Label;
      if (a1 && a2) {
        add(vsDetailsActionLabel, DETAILS_LEFT);
        add(vsDetailsAction2Label, DETAILS_RIGHT);
      } else if (a1) {
        add(vsDetailsActionLabel, STATUS_DETAILS);
      }
      add("Terug", STATUS_BACK);
      break;
    }
    case AppState::ERROR:
      add("Terug", STATUS_BACK, true);
      add("Apparaatstatus", STATUS_DETAILS);
      break;
    case AppState::CHARGE_SYNC:
      add("Afbreken", SYNC_BACK, true);
      break;
    case AppState::SYNC: {
      if (vsSyncTouchLockedHook && vsSyncTouchLockedHook()) break;   // uploading
      const bool retry = syncPhase == SyncPhase::FAILED ||
                         (vsSyncRetryAllowedHook && vsSyncRetryAllowedHook());
      const bool done = vsSyncDoneHook && vsSyncDoneHook();
      if (done) {
        add("Gereed", SYNC_BACK, true);
      } else if (retry) {
        add("Opnieuw", SYNC_RETRY, true);
        add("Later", SYNC_BACK);
      } else if (syncPhase != SyncPhase::CONNECTED) {
        add("Later", SYNC_BACK);
      }
      break;
    }
    default:
      break;
  }
  return n;
}

static bool vsStickOptionScreen() {
  VsStickOption o[4];
  return vsStickOptions(o) > 0;
}

// A new screen (or a new set of choices) starts on its first choice.
static void vsStickSyncFocus(uint8_t count) {
  const uint32_t key = ((uint32_t)state << 16) | ((uint32_t)syncPhase << 8) | count;
  if (key != vsStickFocusKey) {
    vsStickFocusKey = key;
    vsStickFocus = 0;
  }
  if (count && vsStickFocus >= count) vsStickFocus = 0;
}

static void vsStickFocusNext() {
  VsStickOption o[4];
  const uint8_t n = vsStickOptions(o);
  vsStickSyncFocus(n);
  if (n) vsStickFocus = (uint8_t)((vsStickFocus + 1) % n);
  screenDirty = true;
}

void handleTouch(int x, int y);   // main_v02, CoreS3 touch dispatch

// Choose the selected option: exactly what tapping that CoreS3 button does.
// MENU goes through handleMenuTouchV03 in main_v03 (same targets).
static void (*vsStickMenuTouch)(int, int) = nullptr;
static void vsStickActivate() {
  VsStickOption o[4];
  const uint8_t n = vsStickOptions(o);
  vsStickSyncFocus(n);
  if (!n) return;
  const Rect& r = o[vsStickFocus].target;
  const int x = r.x + r.w / 2, y = r.y + r.h / 2;
  Serial.printf("STICK: choose '%s' on screen %u\n", o[vsStickFocus].label, (unsigned)state);
  if (state == AppState::MENU && vsStickMenuTouch) vsStickMenuTouch(x, y);
  else handleTouch(x, y);
  screenDirty = true;
}

// The choices, stacked from `y` down, the selected one filled.
static void stickDrawOptions(int y) {
  VsStickOption o[4];
  const uint8_t n = vsStickOptions(o);
  vsStickSyncFocus(n);
  const int h = 26, gap = 5;
  for (uint8_t i = 0; i < n; ++i) {
    const bool sel = i == vsStickFocus;
    const int yy = y + i * (h + gap);
    const uint16_t fill = sel ? C_NAVY : C_WHITE;
    M5.Display.fillRoundRect(6, yy, SCREEN_W - 12, h, 6, fill);
    M5.Display.drawRoundRect(6, yy, SCREEN_W - 12, h, 6, sel ? C_NAVY : C_LINE);
    const uint8_t size = strlen(o[i].label) <= 10 ? 2 : 1;
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextSize(size);
    M5.Display.setTextColor(sel ? C_WHITE : C_NAVY);
    M5.Display.drawString(o[i].label, SCREEN_W / 2, yy + h / 2 + 1);
  }
  if (n > 1) drawPwrHints("Zijknop: volgende", "Knop: kiezen");
  else if (n == 1) drawPwrHints("Knop: kiezen");
}

// Where the choices start so that they end just above the hint block.
static int stickOptionsTop() {
  VsStickOption o[4];
  const uint8_t n = vsStickOptions(o);
  return STICK_HINT_Y - 6 - n * 31 + 5;
}

// ---------------------------------------------------------------------------
// boot
// ---------------------------------------------------------------------------

static void drawStickBrian(int cx, int cy, int frame) {
  const int bob = (frame % 8 < 4) ? 0 : 2;
  const int headY = cy + bob;
  const uint16_t ink = 0x18E3, accent = C_BLUE, pale = 0xEF7D;
  M5.Display.fillRect(cx - 50, cy - 24, 100, 128, C_WHITE);
  M5.Display.drawLine(cx, headY - 12, cx, headY - 4, ink);
  M5.Display.fillCircle(cx, headY - 15, 4, accent);
  M5.Display.fillRoundRect(cx - 36, headY, 72, 48, 14, pale);
  M5.Display.drawRoundRect(cx - 36, headY, 72, 48, 14, ink);
  M5.Display.fillRoundRect(cx - 28, headY + 8, 56, 26, 8, C_WHITE);
  M5.Display.drawRoundRect(cx - 28, headY + 8, 56, 26, 8, ink);
  const bool blink = (frame == 5 || frame == 6 || frame == 13);
  if (blink) {
    M5.Display.drawFastHLine(cx - 19, headY + 21, 10, ink);
    M5.Display.drawFastHLine(cx + 9, headY + 21, 10, ink);
  } else {
    M5.Display.fillCircle(cx - 14, headY + 21, 4, accent);
    M5.Display.fillCircle(cx + 14, headY + 21, 4, accent);
  }
  const int bodyY = headY + 52;
  M5.Display.fillRoundRect(cx - 38, bodyY, 76, 44, 9, C_WHITE);
  M5.Display.drawRoundRect(cx - 38, bodyY, 76, 44, 9, ink);
  M5.Display.drawLine(cx, bodyY + 2, cx, bodyY + 40, C_LINE);
  M5.Display.drawCircle(cx - 13, bodyY + 12, 3, accent);
  M5.Display.drawCircle(cx + 13, bodyY + 12, 3, accent);
  M5.Display.drawCircle(cx, bodyY + 29, 6, accent);
  M5.Display.fillCircle(cx, bodyY + 29, 2, accent);
}

static void showBrianBootAnimation() {
  M5.Display.wakeup();
  M5.Display.setBrightness(BRIGHTNESS_ACTIVE);
  M5.Display.fillScreen(C_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(C_GREY);
  M5.Display.setTextSize(1);
  M5.Display.drawString("VisiteScribe", 4, 6);
  for (int frame = 0; frame < 14; ++frame) {
    drawStickBrian(SCREEN_W / 2, 58, frame);
    delay(90);
  }
  centeredText(196, "BRIAN", C_NAVY, 3);
  centeredText(222, "ready to listen", C_GREY, 1);
  delay(2500);
}

// ---------------------------------------------------------------------------
// screens
// ---------------------------------------------------------------------------

void drawHome() {
  drawHeader("");
  stickClearBody();
  centeredText(58, "Klaar voor", C_NAVY, 2);
  centeredText(80, "opname", C_NAVY, 2);
  centeredText(104, "Standaard: Patient", C_GREY, 1);

  int y = 128;
#ifdef VISITESCRIBE_DIRECT_OPUS
  if (vsDoRecoveredOnBoot) {
    y = stickWrapped(y, "Audio hersteld - kies Synchroniseer", C_VIOLET, 1, 2);
  }
#endif
  // Cached: counting walks the whole recordings folder, and on the Stick a
  // button press that falls entirely inside that walk is never seen.
  // main_v080 refreshes it a moment after arriving at HOME.
  const int32_t pending = vsStickPendingCache;
  if (pending > 0) {
    char line[32];
    snprintf(line, sizeof(line), "%ld nog te verzenden", (long)pending);
    centeredText(y + 6, line, C_VIOLET, 1);
    y += 16;
  }
  if (sdOk) {
    const uint32_t minutes = vsStickMinutesLeft();
    char line[32];
    snprintf(line, sizeof(line), "Ruimte: %lu min", (unsigned long)minutes);
    centeredText(y + 6, line, minutes < 5 ? C_AMBER : C_GREY, 1);
  }
  drawPwrHints("1x knop: start", "2x knop: menu");
}

void drawModeConfirm() {
  // Not reachable in the pocket flow; kept for completeness.
  drawHeader("");
  stickClearBody();
  centeredText(HEADER_H + 11, modeTitle(selectedMode), C_NAVY, 1);
  centeredText(110, "Klaar om op", C_NAVY, 1);
  centeredText(124, "te nemen", C_NAVY, 1);
  drawPwrHints("1x knop: start");
}

static void stickRecordingStatusLine() {
  M5.Display.fillRect(0, 150, SCREEN_W, 44, C_WHITE);
  const bool toast = recordingToast[0] &&
      static_cast<int32_t>(recordingToastUntilMs - millis()) > 0;
  if (toast) {
    stickWrapped(154, recordingToast, C_VIOLET, 1, 2);
  } else if (selectedMode == Mode::MEETING && markerCount > 0) {
    char marks[40];
    snprintf(marks, sizeof(marks), "%u markering%s",
             markerCount, markerCount == 1 ? "" : "en");
    centeredText(160, marks, C_GREY, 1);
  }
  const uint32_t minutes = vsStickMinutesLeft();
  if (minutes < 5) {
    char line[32];
    snprintf(line, sizeof(line), "Opslag bijna vol: %lu min", (unsigned long)minutes);
    centeredText(184, line, C_AMBER, 1);
  }
}

void drawRecording() {
  char elapsed[16];
  formatCompactElapsed(recordingDisplayElapsedMs(), elapsed, sizeof(elapsed));
  drawHeader("");
  stickClearBody();

  M5.Display.fillCircle(14, 36, 5, C_RED);
  M5.Display.setTextDatum(middle_left);
  M5.Display.setTextColor(C_RED);
  M5.Display.setTextSize(2);
  M5.Display.drawString("Neemt op", 26, 37);

  if (quickModeChoiceActive) {
    const uint32_t used = millis() - quickModeChoiceStartedMs;
    const uint32_t remaining = used >= QUICK_MODE_CHOICE_MS
        ? 0 : (QUICK_MODE_CHOICE_MS - used + 999) / 1000;
    const char* labels[3] = {"Patient", "Vergadering", "STOP"};
    const QuickChoice values[3] = {QuickChoice::PATIENT, QuickChoice::MEETING,
                                   QuickChoice::STOP};
    for (int i = 0; i < 3; ++i) {
      const bool sel = quickChoice == values[i];
      const bool stop = values[i] == QuickChoice::STOP;
      const int yy = 58 + i * 34;
      const uint16_t edge = stop ? C_RED : (sel ? C_VIOLET : C_LINE);
      const uint16_t fill = sel ? (stop ? C_RED : C_VIOLET_SOFT) : C_WHITE;
      M5.Display.fillRoundRect(6, yy, SCREEN_W - 12, 28, 6, fill);
      M5.Display.drawRoundRect(6, yy, SCREEN_W - 12, 28, 6, edge);
      if (sel) M5.Display.drawRoundRect(7, yy + 1, SCREEN_W - 14, 26, 5, edge);
      M5.Display.setTextDatum(middle_center);
      M5.Display.setTextSize(strlen(labels[i]) <= 10 ? 2 : 1);
      M5.Display.setTextColor(sel && stop ? C_WHITE : (stop ? C_RED : C_NAVY));
      M5.Display.drawString(labels[i], SCREEN_W / 2, yy + 15);
    }
    char line[32];
    snprintf(line, sizeof(line), "Keuze over %lus", (unsigned long)remaining);
    centeredText(170, line, C_GREY, 1);
    drawPwrHints("Knop: volgende keuze");
    lastUiSecond = activeElapsedMs() / 1000;
    return;
  }

  char context[40];
  if (selectedMode == Mode::MEETING) snprintf(context, sizeof(context), "Vergadering");
  else snprintf(context, sizeof(context), "Patient %u", patientNumber);
  centeredText(72, context, C_NAVY, strlen(context) <= 11 ? 2 : 1);

  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(C_NAVY);
  M5.Display.setTextSize(elapsed[5] ? 2 : 3);     // h:mm:ss needs the smaller size
  M5.Display.drawString(elapsed, SCREEN_W / 2, 118);

  stickRecordingStatusLine();
  drawPwrHints("1x knop: stop",
               selectedMode == Mode::MEETING ? "2x knop: markeer" : "2x: volgende patient");
  lastUiSecond = activeElapsedMs() / 1000;
}

void drawPaused() {
  drawHeader("");
  stickClearBody();
  centeredText(HEADER_H + 11, "Pauze", C_NAVY, 2);
  centeredText(100, "Opname", C_AMBER, 2);
  centeredText(122, "gepauzeerd", C_AMBER, 2);
  drawPwrHints("1x knop: stop");
}

static void drawSaving() {
  drawHeader("");
  stickClearBody();
  centeredText(96, "Opname", C_NAVY, 2);
  centeredText(118, "bewaren...", C_NAVY, 2);
  centeredText(146, "Een ogenblik", C_GREY, 1);
  centeredText(162, "Knop even uit", C_GREY, 1);
}

void drawFinished() {
  drawHeader("");
  stickClearBody();
  if (lastSessionFalseStart) {
    centeredText(80, "Valse", C_AMBER, 3);
    centeredText(108, "start", C_AMBER, 3);
    centeredText(138, "Opname verwijderd", C_GREY, 1);
    centeredText(152, "korter dan 10 s", C_GREY, 1);
  } else {
    M5.Display.drawCircle(SCREEN_W / 2, 60, 14, C_GREEN);
    M5.Display.drawLine(SCREEN_W / 2 - 7, 60, SCREEN_W / 2 - 2, 65, C_GREEN);
    M5.Display.drawLine(SCREEN_W / 2 - 2, 65, SCREEN_W / 2 + 8, 53, C_GREEN);
    centeredText(94, "Opgeslagen", C_GREEN, 2);
    char context[40];
    if (selectedMode == Mode::MEETING) snprintf(context, sizeof(context), "Vergadering");
    else if (patientNumber <= 1) snprintf(context, sizeof(context), "Patient - 1");
    else snprintf(context, sizeof(context), "Patienten - %u", patientNumber);
    centeredText(120, context, C_NAVY, 1);
    centeredText(138, "Nog te verzenden", C_VIOLET, 1);
    if (vsStickStoppedForSpace) {
      centeredText(162, "Opslag vol:", C_AMBER, 1);
      centeredText(176, "synchroniseer eerst", C_AMBER, 1);
    }
  }
  drawPwrHints("1x knop: nieuwe", "2x knop: menu");
}

void drawMenu() {
  drawHeader("");
  stickClearBody();
  centeredText(HEADER_H + 11, "Menu", C_NAVY, 2);
  stickDrawOptions(stickOptionsTop());
}

static void stickKv(int y, const char* key, const char* value, uint16_t color) {
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(middle_left);
  M5.Display.setTextColor(C_GREY);
  M5.Display.drawString(key, 6, y);
  M5.Display.setTextDatum(middle_right);
  M5.Display.setTextColor(color);
  M5.Display.drawString(value, SCREEN_W - 6, y);
}

void drawStatus() {
  drawHeader("");
  stickClearBody();
  centeredText(HEADER_H + 11, "Status", C_NAVY, 2);
  int y = 48;
  char v[32];
  snprintf(v, sizeof(v), "%s%d%%", batteryCharging ? "+" : "", batteryPct);
  stickKv(y, "Batterij", v, (batteryPct >= 0 && batteryPct <= 15) ? C_AMBER : C_NAVY);
  y += 14;
  if (sdOk) {
    snprintf(v, sizeof(v), "%lu min vrij", (unsigned long)vsStickMinutesLeft(true));
    stickKv(y, "Opslag", v, vsStickMinutesLeft() < 5 ? C_AMBER : C_NAVY);
  } else {
    stickKv(y, "Opslag", "Fout", C_RED);
  }
  y += 14;
  stickKv(y, "Audio", audioError ? "Fout" : "Gereed", audioError ? C_RED : C_NAVY);
  y += 14;
  stickKv(y, "Wifi", WiFi.status() == WL_CONNECTED ? "Verbonden" : "Uit", C_NAVY);
  y += 14;
  uint16_t accountColor = C_NAVY;
  const char* account = vsStatusAccountHook ? vsStatusAccountHook(accountColor) : nullptr;
  if (account) {
    stickKv(y, "OurMind", "", C_NAVY);
    y += 8;
    stickWrapped(y, account, accountColor, 1, 2);
  }
  stickDrawOptions(stickOptionsTop());
}

static void drawDetails() {
  drawHeader("");
  stickClearBody();
  centeredText(HEADER_H + 11, "Details", C_NAVY, 2);
  int y = STICK_BODY_Y;
  y = stickWrapped(y, sdOk ? "Flash-opslag: OK" : "Flash-opslag: FOUT", sdOk ? C_NAVY : C_RED, 1, 1);
  y = stickWrapped(y, audioError ? "Audio: FOUT" : "Audio: Opus 16 kbit/s",
                   audioError ? C_RED : C_GREY, 1, 1);
  if (vsDetailsInfoHook) {
    y = stickWrapped(y, vsDetailsInfoHook(), C_GREY, 1, 2);
  } else {
    char line[32];
    snprintf(line, sizeof(line), "Board ID: %d", (int)M5.getBoard());
    y = stickWrapped(y, line, C_GREY, 1, 1);
  }
  char line[64];
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(line, sizeof(line), "%s", WiFi.SSID().c_str());
    y = stickWrapped(y, line, C_GREY, 1, 1);
  } else {
    y = stickWrapped(y, "Wifi: uit", C_GREY, 1, 1);
  }
  stickDrawOptions(stickOptionsTop());
}

// Set by main_v066: draws the server-sync screen when the upload engine owns
// the SYNC state, and says so.
static bool (*vsStickServerSyncDraw)() = nullptr;

void drawSync() {
  if (vsStickServerSyncDraw && vsStickServerSyncDraw()) return;
  drawHeader("");
  stickClearBody();
  centeredText(HEADER_H + 11, "Synchroniseer", C_NAVY, 1);
  if (syncPhase == SyncPhase::NO_CREDENTIALS) {
    centeredText(74, "Geen wifi", C_AMBER, 2);
    centeredText(94, "ingesteld", C_AMBER, 2);
    stickWrapped(114, "Opnames blijven op dit apparaat", C_GREY, 1, 2);
  } else if (syncPhase == SyncPhase::CONNECTING_1 ||
             syncPhase == SyncPhase::CONNECTING_2) {
    centeredText(80, "Wifi", C_NAVY, 2);
    centeredText(100, "verbinden", C_NAVY, 2);
    centeredText(124, "Even geduld", C_GREY, 1);
  } else if (syncPhase == SyncPhase::CONNECTED) {
    centeredText(80, "Opnames", C_NAVY, 2);
    centeredText(100, "voorbereiden", C_NAVY, 1);
    centeredText(124, "Wifi verbonden", C_GREY, 1);
  } else if (syncPhase == SyncPhase::FAILED) {
    centeredText(76, "Geen", C_AMBER, 2);
    centeredText(96, "verbinding", C_AMBER, 2);
    stickWrapped(114, "Opnames blijven op dit apparaat", C_GREY, 1, 2);
  } else {
    centeredText(90, "Klaar om te", C_NAVY, 1);
    centeredText(104, "verzenden", C_NAVY, 1);
  }
  stickDrawOptions(stickOptionsTop());
}

static void drawChargeSync() {
  drawHeader("");
  stickClearBody();
  centeredText(HEADER_H + 11, "Opladen", C_NAVY, 2);
  centeredText(70, "Sync wordt", C_NAVY, 1);
  centeredText(84, "gestart over", C_NAVY, 1);
  const uint32_t used = millis() - chargeSyncStartedMs;
  const uint32_t seconds = used < CHARGE_SYNC_DELAY_MS
      ? (CHARGE_SYNC_DELAY_MS - used + 999) / 1000 : 0;
  char text[12];
  snprintf(text, sizeof(text), "%lu", (unsigned long)seconds);
  centeredText(116, text, C_VIOLET, 3);
  stickDrawOptions(stickOptionsTop());
}

static void drawError() {
  drawHeader("");
  stickClearBody();
  int y = 34;
  y = stickWrapped(y, uiErrorTitle.length() ? uiErrorTitle.c_str() : "Er ging iets mis",
                   C_RED, 2, 3);
  if (uiErrorDetail.length()) y = stickWrapped(y + 4, uiErrorDetail.c_str(), C_GREY, 1, 3);
  stickDrawOptions(stickOptionsTop());
}

static void updateRecordingDynamic() {
  if (state != AppState::RECORDING) return;
  if (quickModeChoiceActive) {
    const uint32_t used = millis() - quickModeChoiceStartedMs;
    const uint32_t remaining = used >= QUICK_MODE_CHOICE_MS
        ? 0 : (QUICK_MODE_CHOICE_MS - used + 999) / 1000;
    char line[32];
    snprintf(line, sizeof(line), "Keuze over %lus", (unsigned long)remaining);
    M5.Display.fillRect(0, 162, SCREEN_W, 16, C_WHITE);
    centeredText(170, line, C_GREY, 1);
    lastUiSecond = activeElapsedMs() / 1000;
    return;
  }
  char elapsed[16];
  formatCompactElapsed(recordingDisplayElapsedMs(), elapsed, sizeof(elapsed));
  M5.Display.fillRect(0, 100, SCREEN_W, 38, C_WHITE);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(C_NAVY);
  M5.Display.setTextSize(elapsed[5] ? 2 : 3);
  M5.Display.drawString(elapsed, SCREEN_W / 2, 118);
  stickRecordingStatusLine();
  lastUiSecond = activeElapsedMs() / 1000;
}

void render(bool force = false) {
  if (!force && displayPower == DisplayPower::OFF) return;
  if (!force && !screenDirty) {
    if (state == AppState::RECORDING && activeElapsedMs() / 1000 != lastUiSecond) {
      updateRecordingDynamic();
    }
    return;
  }
  screenDirty = false;
  // Back on a screen without choices: the next menu starts on its first one.
  if (!vsStickOptionScreen()) vsStickFocusKey = 0xFFFFFFFFUL;
  switch (state) {
    case AppState::HOME: drawHome(); break;
    case AppState::MODE_CONFIRM: drawModeConfirm(); break;
    case AppState::RECORDING: drawRecording(); break;
    case AppState::PAUSED: drawPaused(); break;
    case AppState::SAVING: drawSaving(); break;
    case AppState::FINISHED: drawFinished(); break;
    case AppState::MENU: drawMenu(); break;
    case AppState::STATUS: drawStatus(); break;
    case AppState::DETAILS: drawDetails(); break;
    case AppState::SYNC: drawSync(); break;
    case AppState::CHARGE_SYNC: drawChargeSync(); break;
    case AppState::ERROR: drawError(); break;
  }
}

// ---------------------------------------------------------------------------
// buttons
// ---------------------------------------------------------------------------

// Clicks are latched here and taken by whoever handles them, so an edge seen
// by one M5.update() can never be lost to the next.
static bool vsStickAClick = false;
static bool vsStickBClick = false;

static void vsStickPollButtons() {
  M5.update();
  if (M5.BtnA.wasClicked()) vsStickAClick = true;
  if (M5.BtnB.wasClicked()) vsStickBClick = true;
}
static bool vsStickTakeA() { const bool v = vsStickAClick; vsStickAClick = false; return v; }
static bool vsStickTakeB() { const bool v = vsStickBClick; vsStickBClick = false; return v; }
