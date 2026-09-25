#pragma once

// Copy this file to server_secrets.h and keep the real values local.
// server_secrets.h is ignored by git.

#define VISITESCRIBE_SERVER_BASE_URL "https://scribe.primumnonnocere.olares.com"

// For the current one-user prototype it is fine to reuse the same registered
// API device identity/token as the CoreS3-Lite. If you later provision this
// board separately, give it its own device ID/token here.
#define VISITESCRIBE_DEVICE_ID "visitescribe-waveshare-001"
#define VISITESCRIBE_DEVICE_TOKEN "PASTE_DEVICE_TOKEN_HERE"
