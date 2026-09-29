#pragma once
#include <vector>

static void (*vsSessionScanProgressHook)() = nullptr;

// A call-scoped snapshot of regular files, shared by sync maintenance/inventory.
// Audio/metadata contents are still opened freshly by their respective readers.
static bool vsReadSessionDirectory(std::vector<String>& files) {
  files.clear();
  File dir = SD.open("/visitescribe");
  if (!dir) return false;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      String name = f.name();
      files.push_back(name.substring(name.lastIndexOf('/') + 1));
    }
    f.close();
    if (vsSessionScanProgressHook) vsSessionScanProgressHook();
  }
  dir.close();
  return true;
}
