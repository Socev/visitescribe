#pragma once

// The one place the Brian firmware version lives.
//
// The heartbeat reports this string as software_version, and the server
// counts an over-the-air update as installed only when a recorder comes back
// reporting exactly the version of the image it was sent. The server reads the
// version out of the uploaded image through the marker below, so the two can
// never disagree. Bump this for every image you upload.
#ifndef VISITESCRIBE_FW_VERSION
#define VISITESCRIBE_FW_VERSION "0.8.2"
#endif

#define VISITESCRIBE_FW_BOARD "cores3-lite"

// Searched for by the server (app/fleet.py, inspect_image). Keep the format.
#define VISITESCRIBE_FW_MARKER \
  "VSFW|version=" VISITESCRIBE_FW_VERSION "|board=" VISITESCRIBE_FW_BOARD "|"
