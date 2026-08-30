#pragma once

/* Copy this file to voice_satellite_secrets.h and fill in local values.
 * The real file is gitignored. SATELLITE_TOKEN must exactly match the Pi
 * receiver's SATELLITE_TOKEN value. */
#define WIFI_SSID "your-wifi"
#define WIFI_PASSWORD "your-wifi-password"
#define SATELLITE_TOKEN "generate-a-long-random-token"

/* Current firmware uses inet_pton(), so use the Pi's LAN IPv4 address. */
#define SATELLITE_SERVER_HOST "192.168.1.100"
#define SATELLITE_SERVER_PORT 8765
