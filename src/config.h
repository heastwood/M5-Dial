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

// ── Devices (Home Assistant MQTT lights) ───────────────────────────────────────
// In HA, configure an MQTT light for each device with these topics (see README).
#define LAMP1_NAME        "Lamp 1"
#define LAMP1_CMD_TOPIC   "home/lamp1/set"
#define LAMP1_STATE_TOPIC "home/lamp1/state"
#define LAMP1_GET_TOPIC   "home/lamp1/get"

#define LAMP2_NAME        "Lamp 2"
#define LAMP2_CMD_TOPIC   "home/lamp2/set"
#define LAMP2_STATE_TOPIC "home/lamp2/state"
#define LAMP2_GET_TOPIC   "home/lamp2/get"

// ── Dial behaviour ────────────────────────────────────────────────────────────
#define BRIGHTNESS_STEP   5     // brightness units changed per encoder detent
#define PUBLISH_DEBOUNCE_MS 150 // min ms between MQTT publishes while spinning
