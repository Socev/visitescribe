#pragma once
// Action markers (firmware 0.10.0).
//
// A tiny file on the SD card says "an action is under way":
//   /visitescribe/_busy_rec.txt   a recording is open
//   /visitescribe/_busy_sync.txt  a server sync is running
//   /visitescribe/_busy_del.txt   confirmed recordings are being deleted
// It is written BEFORE the action starts and removed when it has finished.
// A marker still there after a reboot means the recorder was reset in the
// middle of that action, and only then does the boot do the slow full check.
//
// The marker only ever makes the recorder do MORE work, never less: every
// action is also safe to repeat without one (an interrupted recording is also
// recovered at the next sync; deletion only touches server-confirmed
// recordings and keeps their receipt until last).
//
// Names start with '_', so they never match a session prefix (sNNNNN).

static String vsMarkerPath(const char* kind) {
  return String("/visitescribe/_busy_") + kind + ".txt";
}

static bool vsMarkerExists(const char* kind) {
  return SD.exists(vsMarkerPath(kind));
}

static bool vsMarkerSet(const char* kind, const String& info) {
  File f = SD.open(vsMarkerPath(kind), FILE_WRITE);
  if (!f) return false;
  const String body = String("action=") + kind + "\ninfo=" + info + "\n";
  const bool ok = f.print(body) == body.length();
  f.flush();
  f.close();
  return ok;
}

static void vsMarkerClear(const char* kind) {
  const String path = vsMarkerPath(kind);
  if (SD.exists(path)) SD.remove(path);
}
