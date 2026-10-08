#pragma once
// Crash trail and recording watchdog (firmware 0.13.0-test2), both boards.
//
// 8 Oct 2026: a CoreS3 restarted 5.5 minutes into a recording with
// reset_reason=6 (task watchdog: core 0 got no idle time for 5 s). Core 0 runs
// the Opus encoder, which also writes the Ogg pages to the SD card, and the
// microphone task. The logbook lines of that recording were lost with the
// restart (they are written to the card only while Brian is idle), so the
// cause could not be read back. This file does two things:
//
// 1. A recording is no longer killed by a few seconds of slow storage. The task
//    watchdog gets 10 s instead of 5 s, the length of the audio queue. A shorter
//    stall is absorbed without loss; a longer hang restarts Brian, the boot
//    recovery saves what was recorded and the recording starts again (3.).
// 2. What happened survives a restart. Every loop pass writes a small trail to
//    RTC memory that a watchdog or panic reset leaves intact: app state,
//    whether a recording runs, the encoder stage, frames encoded and when they
//    last advanced, the audio queue depth. After such a reset the first boot
//    line is `CRASH: ...`, which reaches the admin logbook with the next sync.
//    While recording, an encoder that stops advancing for 2 s is logged
//    (`AUDIO: encoder stalled ...` / `AUDIO: encoder resumed after ...`).
// 3. test3: a recording that a watchdog or panic restart cut off starts again
//    by itself as soon as Brian is up (boot + recovery ~15 s), in the same mode
//    (patient or meeting; still in the chooser = the chooser again). It is a new
//    recording in the list, with a `resumed_after_restart` event. The screen
//    shows "Hervat na herstart". Bootloop guard: at most 2 resumes in a row
//    that crash again within 5 minutes; the third time Brian stays on an error
//    screen and records nothing until the doctor presses the button. A brownout
//    (empty battery), a power-on, an update restart or a USB reset never resume.

#include <esp_task_wdt.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <soc/soc_memory_layout.h>

static constexpr uint32_t VS_TRAIL_MAGIC = 0x56535452;   // "VSTR"
// 10 s: as long as the audio queue (512 frames = 10.24 s). A shorter stall is
// absorbed without loss; a longer one would overflow the queue anyway, and a
// restart with an automatic resume (below) then loses the least.
static constexpr uint32_t VS_TASK_WDT_SECONDS = 10;
static constexpr uint32_t VS_STALL_LOG_MS = 2000;

struct VsCrashTrail {
  uint32_t magic;
  uint32_t loopMs;          // millis() of the last loop pass
  uint32_t framesMs;        // millis() when the encoded-frame count last changed
  uint32_t frames;
  uint32_t dropped;
  uint32_t stagePtr;        // vsDoStage (a string literal in flash)
  uint32_t recordingMs;     // millis() when this recording started (0 = none)
  uint16_t queue;
  uint16_t queueHigh;
  uint8_t appState;
  uint8_t recording;
  uint8_t syncing;
  uint8_t mode;            // Mode (VISIT/MEETING)
  uint8_t modeChosen;      // 0 = still in the ten-second chooser
  uint8_t resumeStreak;    // resumes in a row that crashed again within 5 min
  uint8_t pad[2];
};

RTC_NOINIT_ATTR static VsCrashTrail vsTrail;
static bool vsTrailStallLogged = false;
static uint32_t vsTrailStallStarted = 0;
static constexpr uint32_t VS_RESUME_STABLE_MS = 5UL * 60UL * 1000UL;
static constexpr uint8_t VS_RESUME_MAX_STREAK = 2;
static constexpr uint32_t VS_RESUME_GIVE_UP_MS = 120000UL;   // Brian never reached HOME
enum class VsResume : uint8_t { NONE, PENDING, BLOCKED };
static VsResume vsResume = VsResume::NONE;
static uint8_t vsResumeMode = 0;
static bool vsResumeModeChosen = false;

static const char* vsTrailResetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return nullptr;
  }
}

static const char* vsTrailStage(uint32_t p) {
  const void* ptr = reinterpret_cast<const void*>(p);
  if (!p || !esp_ptr_in_drom(ptr)) return "?";
  const char* s = static_cast<const char*>(ptr);
  return strnlen(s, 32) < 32 ? s : "?";
}

// First thing at boot (after the logbook starts).
static void vsTrailBoot() {
  const esp_reset_reason_t reason = esp_reset_reason();
  const char* name = vsTrailResetName(reason);
  const bool crash = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
                     reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
  uint8_t streak = 0;
  if (name && vsTrail.magic == VS_TRAIL_MAGIC) {
    const uint32_t end = vsTrail.loopMs;
    Serial.printf(
        "CRASH: previous run ended by %s; state=%u recording=%u (for %lus) syncing=%u "
        "encoder_stage=%s frames=%lu last_frame=%ldms_before_last_loop queue=%u high=%u "
        "dropped=%lu uptime=%lus\n",
        name, (unsigned)vsTrail.appState, (unsigned)vsTrail.recording,
        (unsigned long)(vsTrail.recordingMs ? (end - vsTrail.recordingMs) / 1000 : 0),
        (unsigned)vsTrail.syncing, vsTrailStage(vsTrail.stagePtr),
        (unsigned long)vsTrail.frames, (long)(end - vsTrail.framesMs),
        (unsigned)vsTrail.queue, (unsigned)vsTrail.queueHigh,
        (unsigned long)vsTrail.dropped, (unsigned long)(end / 1000));
    if (crash && vsTrail.recording) {
      streak = vsTrail.resumeStreak;
      const uint32_t ran = vsTrail.recordingMs ? end - vsTrail.recordingMs : 0;
      if (ran >= VS_RESUME_STABLE_MS) streak = 0;     // it had been running fine
      if (streak >= VS_RESUME_MAX_STREAK) {
        vsResume = VsResume::BLOCKED;
        Serial.printf("RESUME: not restarting the recording; %u restarts in a row\n",
                      (unsigned)(streak + 1));
      } else {
        vsResume = VsResume::PENDING;
        vsResumeMode = vsTrail.mode;
        vsResumeModeChosen = vsTrail.modeChosen != 0;
        ++streak;
        Serial.printf("RESUME: will restart the recording (mode=%u chosen=%u, resume %u of max %u)\n",
                      (unsigned)vsResumeMode, vsResumeModeChosen ? 1 : 0, (unsigned)streak,
                      (unsigned)VS_RESUME_MAX_STREAK);
      }
    }
  } else if (name) {
    Serial.printf("CRASH: previous run ended by %s; no trail\n", name);
  }
  memset(&vsTrail, 0, sizeof(vsTrail));
  vsTrail.magic = VS_TRAIL_MAGIC;
  vsTrail.resumeStreak = streak;

  const esp_err_t wdt = esp_task_wdt_init(VS_TASK_WDT_SECONDS, true);
  Serial.printf("WDT: task watchdog %lus (%s)\n", (unsigned long)VS_TASK_WDT_SECONDS,
                wdt == ESP_OK ? "ok" : "unchanged");
}

// Every loop pass: a handful of stores, no I/O.
static void vsTrailService() {
  const uint32_t now = millis();
  const bool recording = sessionOpen || captureRunning;
  if (recording && !vsTrail.recording) {
    vsTrail.recordingMs = now;
    vsTrail.framesMs = now;
    vsTrailStallLogged = false;
  }
  if (!recording && vsTrail.recording) vsTrail.resumeStreak = 0;   // ended normally
  if (!recording) vsTrail.recordingMs = 0;
  vsTrail.recording = recording ? 1 : 0;
  vsTrail.appState = (uint8_t)state;
  if (recording) {
    vsTrail.mode = (uint8_t)selectedMode;
    vsTrail.modeChosen = quickModeChoiceActive ? 0 : 1;
    if (vsTrail.resumeStreak && now - vsTrail.recordingMs >= VS_RESUME_STABLE_MS)
      vsTrail.resumeStreak = 0;
  }
  vsTrail.syncing = vsServerSyncRunning ? 1 : 0;
  vsTrail.loopMs = now;
#ifdef VISITESCRIBE_DIRECT_OPUS
  const uint32_t frames = vsDoEncodedFrames;
  if (frames != vsTrail.frames) {
    if (vsTrailStallLogged) {
      Serial.printf("AUDIO: encoder resumed after %lums; queue=%u\n",
                    (unsigned long)(now - vsTrailStallStarted),
                    vsDoQueue ? (unsigned)uxQueueMessagesWaiting(vsDoQueue) : 0);
      vsTrailStallLogged = false;
    }
    vsTrail.frames = frames;
    vsTrail.framesMs = now;
  }
  vsTrail.dropped = vsDoDroppedFrames;
  vsTrail.stagePtr = reinterpret_cast<uint32_t>(vsDoStage);
  vsTrail.queue = vsDoQueue ? (uint16_t)uxQueueMessagesWaiting(vsDoQueue) : 0;
  vsTrail.queueHigh = (uint16_t)vsDoQueueHighWater;
  if (captureRunning && !vsDoWorkerDone && !vsTrailStallLogged &&
      now - vsTrail.framesMs >= VS_STALL_LOG_MS) {
    vsTrailStallLogged = true;
    vsTrailStallStarted = vsTrail.framesMs;
    Serial.printf("AUDIO: encoder stalled %lums stage=%s queue=%u frames=%lu\n",
                  (unsigned long)(now - vsTrail.framesMs), vsTrailStage(vsTrail.stagePtr),
                  (unsigned)vsTrail.queue, (unsigned long)frames);
  }
#endif
}

// Restart the cut-off recording once Brian is up and at HOME. Called every loop
// pass (main_v080.cpp loop(), before the auto sync); does nothing otherwise.
static void vsServiceResume() {
  if (vsResume == VsResume::NONE) return;
  if (millis() > VS_RESUME_GIVE_UP_MS) {
    Serial.println("RESUME: Brian did not reach HOME in time; not restarting");
    vsResume = VsResume::NONE;
    return;
  }
  if (state != AppState::HOME || vs080Mode != Vs080Mode::NONE || sessionOpen ||
      captureRunning || vsServerSyncRunning || vsUsbSyncActive)
    return;
#ifdef VISITESCRIBE_DIRECT_OPUS
  if (!vsDoWorkerDone) return;
#endif
  const VsResume what = vsResume;
  vsResume = VsResume::NONE;
  noteActivity();                 // screen on: the doctor should see what happens

  if (what == VsResume::BLOCKED) {
    uiErrorTitle = "Opname gestopt";
    uiErrorDetail = "Brian herstartte steeds; druk op de knop en start opnieuw";
    state = AppState::ERROR;
    screenDirty = true;
    return;
  }
  bool ok = sdOk;
#if VS_STICK
  if (ok && vsStickMinutesLeft(true) < 2) {
    ok = false;
    uiErrorTitle = "Opslag vol";
    uiErrorDetail = "Opname na herstart niet hervat; synchroniseer eerst";
  }
#endif
  if (ok) ok = startQuickSession();
  if (ok && vsResumeModeChosen) {
    // The same path as the chooser's own time-out, with the mode it had.
    if ((Mode)vsResumeMode == Mode::MEETING) selectQuickMode(Mode::MEETING);
    else selectQuickMode(Mode::VISIT);
    commitQuickRecordingMode();
  }
  if (!ok) {
    Serial.println("RESUME: could not restart the recording");
    state = AppState::ERROR;
    if (!uiErrorTitle.length()) uiErrorTitle = "Opname niet hervat";
    if (!uiErrorDetail.length()) uiErrorDetail = "Start de opname opnieuw met de knop";
    screenDirty = true;
    return;
  }
  logEvent("resumed_after_restart", 0);
  setRecordingToast("Hervat na herstart", 6000);
  Serial.printf("RESUME: recording restarted after a crash; mode=%u chooser=%u\n",
                (unsigned)vsResumeMode, vsResumeModeChosen ? 0 : 1);
  pwrClickPendingV03 = false;
  pwrFirstClickMsV03 = 0;
  lastUserActivityMs = millis();
  screenDirty = true;
}

// Test hook (USB serial, 115200): `VSTEST HANG` during a recording blocks core 0
// with a busy task, exactly like a storage hang. After 10 s the task watchdog
// restarts Brian; the CRASH line, the boot recovery and the automatic resume can
// then be checked. Ignored when not recording.
static void vsTrailHangTask(void*) {
  for (;;) { }
}
static void vsTrailUsbTest(const String& line) {
  if (line != "VSTEST HANG") return;
  if (!(sessionOpen && captureRunning)) {
    Serial.println("VSTEST: only during a recording");
    return;
  }
  Serial.println("VSTEST: hanging core 0 now; the task watchdog restarts Brian in ~10 s");
  xTaskCreatePinnedToCore(vsTrailHangTask, "vstest_hang", 2048, nullptr, 5, nullptr, 0);
}
struct VsTrailTestInstaller {
  VsTrailTestInstaller() { vsUsbTestHook = &vsTrailUsbTest; }
};
static VsTrailTestInstaller vsTrailTestInstaller;
