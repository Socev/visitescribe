#pragma once

// The one place the Brian firmware version lives.
//
// The heartbeat reports this string as software_version, and the server
// counts an over-the-air update as installed only when a recorder comes back
// reporting exactly the version of the image it was sent. The server reads the
// version out of the uploaded image through the marker below, so the two can
// never disagree. Bump this for every image you upload.
#ifndef VISITESCRIBE_FW_VERSION
#define VISITESCRIBE_FW_VERSION "0.12.0"
#endif

// The board travels in the image marker and in every heartbeat; the server
// only hands an update to a recorder of the same board.
#if defined(VISITESCRIBE_BOARD_STICKS3)
#define VISITESCRIBE_FW_BOARD "sticks3"
#else
#define VISITESCRIBE_FW_BOARD "cores3-lite"
#endif

// Searched for by the server (app/fleet.py, inspect_image). Keep the format.
#define VISITESCRIBE_FW_MARKER \
  "VSFW|version=" VISITESCRIBE_FW_VERSION "|board=" VISITESCRIBE_FW_BOARD "|"
