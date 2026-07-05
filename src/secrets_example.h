// Copy this file to secrets.h and fill in your real WiFi credentials.
// secrets.h is gitignored — never commit real credentials (global
// knowledge-base convention #6).
#pragma once

#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_PASS      "YOUR_WIFI_PASSWORD"

// Moonraker on the Flashforge AD5M (community Klipper) — homelab-infra
#define MOONRAKER_HOST "10.0.0.114"
#define MOONRAKER_PORT 7125

// Webcam (screen 8) defaults to MOONRAKER_HOST:8080/stream — uncomment
// to override if your camera serves elsewhere:
// #define WEBCAM_PORT 8080
// #define WEBCAM_PATH "/stream"
