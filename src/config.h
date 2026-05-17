#pragma once

// ── WiFi ─────────────────────────────────────────────────────────────────────
#define WIFI_SSID        "YOUR_WIFI_SSID"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"

// ── MQTT broker (e.g. Mosquitto running alongside Home Assistant) ─────────────
#define MQTT_SERVER      "192.168.1.100"   // IP of your HA / Mosquitto host
#define MQTT_PORT        1883
#define MQTT_USER        "mqtt_user"       // leave "" if no auth
#define MQTT_PASSWORD    "mqtt_pass"
#define MQTT_CLIENT_ID   "m5dial_cync"

// ── Home Assistant MQTT Light topics ─────────────────────────────────────────
// In HA, configure an MQTT light with these topics (see README).
#define LIGHT_CMD_TOPIC   "home/cync/set"
#define LIGHT_STATE_TOPIC "home/cync/state"

// ── Dial behaviour ────────────────────────────────────────────────────────────
#define BRIGHTNESS_STEP   5     // brightness units changed per encoder detent
#define PUBLISH_DEBOUNCE_MS 150 // min ms between MQTT publishes while spinning
