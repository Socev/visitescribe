#pragma once
// Which Brian hardware this build is for.
//
//   (default)                       M5Stack CoreS3-Lite: 320x240 touch, microSD,
//                                   ES7210 stereo mics, AXP2101 power key.
//   -DVISITESCRIBE_BOARD_STICKS3=1  M5Stack StickS3: 135x240 portrait, no touch,
//                                   two buttons, ES8311 mono mic, no microSD.
//
// The recorder, the Opus encoder, the upload engine and the whole fleet layer
// are shared. What differs per board lives here and in ui_sticks3.h:
//
// * Storage. The StickS3 has no card slot. Its recordings live in the LittleFS
//   partition of the 8 MB flash (partitions_sticks3.csv). Every older layer
//   talks to `SD`; on the StickS3 `SD` is a LittleFS instance with the few
//   SD-only calls (begin(cs, spi, hz), cardType, cardSize) mapped onto it, so
//   the same paths (/visitescribe/..., /brianlog/...) and the same code work.
// * Input. The front button (BtnA) plays the CoreS3 PWR key exactly: 1x / 2x /
//   the chooser cycle. The side button (BtnB) replaces touch: it moves the
//   selection in menus, BtnA then chooses. See ui_sticks3.h.
// * Screens: every screen has a portrait version in ui_sticks3.h.
//
// Include after the library headers (Arduino, M5Unified, SD).

#if defined(VISITESCRIBE_BOARD_STICKS3)
#define VS_STICK 1

#include <SD.h>        // CARD_* constants, and the real SD header is now guarded
#include <LittleFS.h>
#include <Preferences.h>

class VsFlashStorage : public fs::LittleFSFS {
 public:
  // SD-style signature, so ensureStorage() in main_v02 works unchanged.
  bool begin(int, SPIClass&, uint32_t) { return mount(); }
  // Formats the partition only on the very first start of this Brian (NVS
  // vsmarker/fsready not yet set). Once it has held recordings, a mount that
  // fails is reported as a storage error instead: formatting then would
  // silently erase recordings that were never synced.
  bool mount() {
    if (mounted_) return true;
    mounted_ = fs::LittleFSFS::begin(false, "/vsflash", 12, "spiffs");
    Preferences prefs;
    bool ready = false;
    if (prefs.begin("vsmarker", true)) {
      ready = prefs.getBool("fsready", false);
      prefs.end();
    }
    if (!mounted_ && !ready) {
      Serial.println("FLASH: first start; formatting the recordings partition");
      mounted_ = fs::LittleFSFS::begin(true, "/vsflash", 12, "spiffs");
    }
    if (mounted_ && !ready && prefs.begin("vsmarker", false)) {
      prefs.putBool("fsready", true);
      prefs.end();
    }
    if (!mounted_) Serial.println("FLASH: recordings partition does not mount; NOT formatted");
    return mounted_;
  }
  void end() {}   // never unmounted at runtime; the old SD retry loop calls this
  sdcard_type_t cardType() { return mounted_ ? CARD_SD : CARD_NONE; }
  uint64_t cardSize() { return totalBytes(); }
  bool mounted() const { return mounted_; }

 private:
  bool mounted_ = false;
};

static VsFlashStorage vsFlashStorage;
#define SD vsFlashStorage

#else
#define VS_STICK 0
#endif
