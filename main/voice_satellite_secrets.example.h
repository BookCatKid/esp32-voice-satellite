#pragma once

/* Copy this file to voice_satellite_secrets.h and fill in local values.
 * The real file is gitignored. SATELLITE_TOKEN must exactly match the Pi
 * receiver's SATELLITE_TOKEN value. */
#define WIFI_SSID "your-wifi"
#define WIFI_PASSWORD "your-wifi-password"
#define SATELLITE_TOKEN "generate-a-long-random-token"

/* The receiver exposes its WebSocket on the Pi's LAN address. */
#define SATELLITE_SERVER_URI "ws://192.168.1.100:8766/ws/satellite"
