# M5 Dial Multi-Device Home Menu Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a home-screen tile menu to the M5 Dial firmware so it can control two lamps (instead of one), each with its own MQTT topics, while reusing today's rotate/press/tap control screen per-device.

**Architecture:** Replace the single set of hardcoded lamp globals (`brightness`, `colorIndex`, `lightOn`, `hasSynced`) with a 2-entry `Device` array holding each lamp's name, MQTT topics, and cached state. Add a `ScreenState` (`HOME` / `DEVICE_CONTROL`) and `activeDeviceIndex` to track navigation. The home screen renders one tile per device; tapping a tile enters that device's control screen (today's existing UI, now reading/writing `devices[activeDeviceIndex]`); a small back icon returns to the home screen.

**Tech Stack:** PlatformIO, Arduino framework, ESP32-S3 (M5Stack Dial), M5Dial library, PubSubClient (MQTT), ArduinoJson.

**Testing note:** This project has no automated test suite (embedded firmware with hardware I/O — matches the existing codebase, which has none). Each code task is verified by a compile check; end-to-end behavior is verified manually on hardware in the final task, per the design spec.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/config.h` | WiFi/MQTT credentials + per-device (Lamp 1 / Lamp 2) MQTT topic definitions |
| `src/main.cpp` | All firmware logic: device state, screen state machine, MQTT, input handling, rendering |
| `ha_mqtt_light.yaml` | Home Assistant automations bridging MQTT topics to each Cync bulb entity |
| `README.md` | Setup instructions and screen/behavior description |

`main.cpp` stays a single file — it's small (under 400 lines after this change) and every piece (state, MQTT, input, rendering) is tightly coupled around the same per-frame `loop()`, which matches the existing structure of the codebase. No new files are needed.

---

### Task 1: Update `config.h` with per-lamp topic definitions

**Files:**
- Modify: `src/config.h`

- [ ] **Step 1: Replace the single-light topic block with Lamp 1 / Lamp 2 blocks**

Replace lines 14-18 (the `LIGHT_CMD_TOPIC` / `LIGHT_STATE_TOPIC` / `LIGHT_GET_TOPIC` block) with:

```cpp
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
```

The full file should now read:

```cpp
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
```

- [ ] **Step 2: Commit**

```bash
git add src/config.h
git commit -m "Add per-lamp MQTT topic definitions to config.h"
```

---

### Task 2: Rewrite `main.cpp` for multi-device support

This is one cohesive change — every function in the file touches the device state in some way, so it's written as a single full-file replacement rather than fragmented edits (avoids line-number drift across steps, and the file won't compile in a partial state anyway since `drawUI()`, `publishState()`, `mqttCallback()`, and `loop()` all share the same globals).

**Files:**
- Modify: `src/main.cpp` (full replacement)

- [ ] **Step 1: Replace the entire contents of `src/main.cpp`**

```cpp
#include <M5Dial.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"

// ── Colour presets cycled by touch ───────────────────────────────────────────
struct ColorPreset {
    const char* name;
    uint8_t r, g, b;
    uint32_t displayHex; // for the M5GFX colour (RGB888)
};

static const ColorPreset COLORS[] = {
    {"White",  255, 255, 255, 0xFFFFFF},
    {"Warm",   255, 147,  41, 0xFF9329},
    {"Red",    255,   0,   0, 0xFF0000},
    {"Orange", 255, 140,   0, 0xFF8C00},
    {"Yellow", 255, 200,   0, 0xFFC800},
    {"Green",    0, 220,   0, 0x00DC00},
    {"Cyan",     0, 220, 220, 0x00DCDC},
    {"Blue",     0,  80, 255, 0x0050FF},
    {"Purple", 160,   0, 255, 0xA000FF},
    {"Pink",   255,  20, 147, 0xFF1493},
};
static constexpr int NUM_COLORS = sizeof(COLORS) / sizeof(COLORS[0]);

// ── Devices ───────────────────────────────────────────────────────────────────
struct Device {
    const char* name;
    const char* cmdTopic;
    const char* stateTopic;
    const char* getTopic;
    bool        stateKnown; // false until first state message arrives from HA
    bool        on;
    int         brightness; // 0-255
    int         colorIndex; // index into COLORS
};

static constexpr int NUM_DEVICES = 2;
static Device devices[NUM_DEVICES] = {
    { LAMP1_NAME, LAMP1_CMD_TOPIC, LAMP1_STATE_TOPIC, LAMP1_GET_TOPIC, false, true, 128, 0 },
    { LAMP2_NAME, LAMP2_CMD_TOPIC, LAMP2_STATE_TOPIC, LAMP2_GET_TOPIC, false, true, 128, 0 },
};

// ── Screen state ──────────────────────────────────────────────────────────────
enum ScreenState { HOME, DEVICE_CONTROL };
static ScreenState screenState       = HOME;
static int         activeDeviceIndex = -1; // meaningful only in DEVICE_CONTROL

// ── State ─────────────────────────────────────────────────────────────────────
static bool     needsRedraw    = true;
static bool     pendingPublish = false;
static uint32_t lastPublishMs  = 0;
static long     lastEncoderVal = 0;
static uint32_t lastActivityMs = 0;
static bool     screenDimmed   = false;

static constexpr uint32_t DIM_TIMEOUT_MS   = 7000;
static constexpr uint32_t FADE_DURATION_MS = 1000;

static constexpr int BACK_ICON_X      = 24;
static constexpr int BACK_ICON_Y      = 24;
static constexpr int BACK_ICON_TOUCH_R = 22; // tap-target radius, bigger than the drawn icon

// ── Network objects ───────────────────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ── Forward declarations ──────────────────────────────────────────────────────
void drawUI();
void publishState();
void mqttCallback(char* topic, byte* payload, unsigned int len);
void reconnectMqtt();
void connectWifi();

// ─────────────────────────────────────────────────────────────────────────────
// Display helpers
// ─────────────────────────────────────────────────────────────────────────────

static void drawBrightnessArc(int pct, int colorIndex) {
    // pct: 0-100
    auto& d = M5Dial.Display;
    const int cx = 120, cy = 120, r = 100, thick = 14;
    const float startDeg = 135.0f, totalDeg = 270.0f;

    // Background arc (dark grey)
    d.drawArc(cx, cy, r, r - thick, startDeg, startDeg + totalDeg, 0x2104);

    if (pct > 0) {
        float endDeg = startDeg + totalDeg * pct / 100.0f;
        uint32_t col  = M5Dial.Display.color888(
            COLORS[colorIndex].r,
            COLORS[colorIndex].g,
            COLORS[colorIndex].b);
        d.drawArc(cx, cy, r, r - thick, startDeg, endDeg, col);
    }
}

static void drawCenterText(int pct, bool on, int colorIndex) {
    auto& d = M5Dial.Display;
    d.fillCircle(120, 120, 82, TFT_BLACK);

    if (!on) {
        d.setTextDatum(middle_center);
        d.setTextColor(TFT_DARKGREY);
        d.setFont(&fonts::FreeSansBold12pt7b);
        d.drawString("OFF", 120, 115);
        d.setFont(&fonts::FreeSans9pt7b);
        d.drawString("touch to set color", 120, 148);
        return;
    }

    // Big percentage
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    d.setTextDatum(middle_center);
    d.setTextColor(TFT_WHITE);
    d.setFont(&fonts::FreeSansBold18pt7b);
    d.drawString(buf, 120, 108);

    // Colour name below
    d.setFont(&fonts::FreeSans9pt7b);
    uint32_t nameCol = M5Dial.Display.color888(
        COLORS[colorIndex].r,
        COLORS[colorIndex].g,
        COLORS[colorIndex].b);
    d.setTextColor(nameCol);
    d.drawString(COLORS[colorIndex].name, 120, 148);
}

static void drawColorDots(int colorIndex) {
    auto& d = M5Dial.Display;
    // Ring of small dots showing all presets; active one is highlighted
    const int cx = 120, cy = 120, ringR = 80, dotR = 6;
    for (int i = 0; i < NUM_COLORS; i++) {
        float angleDeg = -90.0f + 360.0f * i / NUM_COLORS;
        float rad = angleDeg * M_PI / 180.0f;
        int x = cx + (int)(ringR * cosf(rad));
        int y = cy + (int)(ringR * sinf(rad));
        uint32_t col = d.color888(COLORS[i].r, COLORS[i].g, COLORS[i].b);
        if (i == colorIndex) {
            d.fillCircle(x, y, dotR + 2, TFT_WHITE);
        }
        d.fillCircle(x, y, dotR, col);
    }
}

static void drawBackIcon() {
    auto& d = M5Dial.Display;
    d.fillCircle(BACK_ICON_X, BACK_ICON_Y, 16, 0x2104);
    d.setTextDatum(middle_center);
    d.setTextColor(TFT_WHITE);
    d.setFont(&fonts::FreeSans9pt7b);
    d.drawString("<", BACK_ICON_X, BACK_ICON_Y + 1);
}

static bool isBackIconTouch(int x, int y) {
    int dx = x - BACK_ICON_X, dy = y - BACK_ICON_Y;
    return (dx * dx + dy * dy) <= (BACK_ICON_TOUCH_R * BACK_ICON_TOUCH_R);
}

static void drawSyncing() {
    auto& d = M5Dial.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextDatum(middle_center);
    d.setTextColor(TFT_DARKGREY);
    d.setFont(&fonts::FreeSans9pt7b);
    d.drawString("Syncing...", 120, 120);
}

static void drawHomeScreen() {
    auto& d = M5Dial.Display;
    d.fillScreen(TFT_BLACK);
    const int tileH = 120;
    d.setFont(&fonts::FreeSansBold12pt7b);
    for (int i = 0; i < NUM_DEVICES; i++) {
        int cy = i * tileH + tileH / 2;
        uint32_t dotCol = 0x39C7; // neutral grey (state not yet known)
        if (devices[i].stateKnown) {
            dotCol = devices[i].on ? d.color888(0, 220, 0) : d.color888(120, 120, 120);
        }
        d.setTextDatum(middle_center);
        d.setTextColor(TFT_WHITE);
        d.drawString(devices[i].name, 120, cy);
        d.fillCircle(200, cy, 8, dotCol);
    }
    d.drawFastHLine(20, tileH, 200, 0x39C7);
}

void drawUI() {
    if (screenState == HOME) {
        drawHomeScreen();
        needsRedraw = false;
        return;
    }

    Device& dev = devices[activeDeviceIndex];
    if (!dev.stateKnown) { drawSyncing(); needsRedraw = false; return; }

    int pct = dev.on ? dev.brightness * 100 / 255 : 0;
    drawBrightnessArc(pct, dev.colorIndex);
    drawCenterText(pct, dev.on, dev.colorIndex);
    if (dev.on) drawColorDots(dev.colorIndex);
    drawBackIcon();
    needsRedraw = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT
// ─────────────────────────────────────────────────────────────────────────────

void publishState() {
    if (!mqtt.connected() || activeDeviceIndex < 0) return;
    Device& dev = devices[activeDeviceIndex];

    JsonDocument doc;
    doc["state"]      = dev.on ? "ON" : "OFF";
    doc["brightness"] = dev.brightness;
    JsonObject color  = doc["color"].to<JsonObject>();
    color["r"] = COLORS[dev.colorIndex].r;
    color["g"] = COLORS[dev.colorIndex].g;
    color["b"] = COLORS[dev.colorIndex].b;

    char buf[128];
    serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(dev.cmdTopic, buf, /*retain=*/false);
    lastPublishMs  = millis();
    pendingPublish = false;
}

static int deviceIndexForStateTopic(const char* topic) {
    for (int i = 0; i < NUM_DEVICES; i++) {
        if (strcmp(topic, devices[i].stateTopic) == 0) return i;
    }
    return -1;
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
    // Parse state updates pushed from HA so the dial stays in sync
    int idx = deviceIndexForStateTopic(topic);
    if (idx < 0) return;
    Device& dev = devices[idx];

    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return;

    bool changed = false;

    if (doc["state"].is<const char*>()) {
        bool on = strcmp(doc["state"], "ON") == 0;
        if (on != dev.on) { dev.on = on; changed = true; }
    }
    if (doc["brightness"].is<int>()) {
        int b = doc["brightness"].as<int>();
        b = constrain(b, 0, 255);
        if (b != dev.brightness) { dev.brightness = b; changed = true; }
    }
    if (doc["color"].is<JsonObject>()) {
        uint8_t r = doc["color"]["r"] | 255;
        uint8_t g = doc["color"]["g"] | 255;
        uint8_t b = doc["color"]["b"] | 255;
        // Find closest preset by minimising squared distance
        int best = dev.colorIndex, bestDist = INT_MAX;
        for (int i = 0; i < NUM_COLORS; i++) {
            int dr = r - COLORS[i].r, dg = g - COLORS[i].g, db = b - COLORS[i].b;
            int d = dr*dr + dg*dg + db*db;
            if (d < bestDist) { bestDist = d; best = i; }
        }
        if (best != dev.colorIndex) { dev.colorIndex = best; changed = true; }
    }

    if (!dev.stateKnown) { dev.stateKnown = true; changed = true; }
    if (changed) needsRedraw = true;
}

// Connect, subscribe, and request current state for every device from HA.
// Returns true on success.
bool connectMqtt() {
    if (!mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) return false;
    for (int i = 0; i < NUM_DEVICES; i++) {
        mqtt.subscribe(devices[i].stateTopic);
        // Ask HA to publish its current state so tiles/screens are accurate on boot.
        // Retained messages on stateTopic also satisfy this if the broker has them;
        // either way we're covered.
        mqtt.publish(devices[i].getTopic, "");
    }
    return true;
}

void reconnectMqtt() {
    static uint32_t lastAttemptMs = 0;
    if (millis() - lastAttemptMs < 5000) return;
    lastAttemptMs = millis();
    connectMqtt();
}

void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    M5Dial.Display.setTextDatum(middle_center);
    M5Dial.Display.setTextColor(TFT_WHITE);
    M5Dial.Display.setFont(&fonts::FreeSans9pt7b);
    M5Dial.Display.drawString("Connecting WiFi...", 120, 120);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(250);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Arduino entry points
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    auto cfg = M5.config();
    M5Dial.begin(cfg, /*encoder=*/true, /*touch=*/true);

    M5Dial.Display.setBrightness(128);
    M5Dial.Display.fillScreen(TFT_BLACK);

    connectWifi();

    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(256);

    // Connect immediately — don't wait for loop()'s 5-second retry gate
    connectMqtt();

    M5Dial.Display.fillScreen(TFT_BLACK);
    lastEncoderVal = M5Dial.Encoder.read();
    lastActivityMs = millis();
    needsRedraw = true; // shows the home screen (or "Syncing..." if a device screen is active)
}

void loop() {
    M5Dial.update();

    // ── WiFi watchdog ─────────────────────────────────────────────────────────
    if (WiFi.status() != WL_CONNECTED) {
        connectWifi();
        return;
    }

    // ── MQTT ──────────────────────────────────────────────────────────────────
    if (!mqtt.connected()) reconnectMqtt();
    mqtt.loop();

    // ── Rotary encoder → brightness (device control screen only) ─────────────
    long encVal = M5Dial.Encoder.read();
    long delta  = encVal - lastEncoderVal;
    lastEncoderVal = encVal;
    if (delta != 0 && screenState == DEVICE_CONTROL) {
        lastActivityMs = millis();
        if (screenDimmed) { M5Dial.Display.setBrightness(128); screenDimmed = false; needsRedraw = true; }
        Device& dev = devices[activeDeviceIndex];
        if (!dev.on) dev.on = true; // first turn wakes the light

        dev.brightness = constrain(dev.brightness + (int)delta * BRIGHTNESS_STEP, 0, 255);
        needsRedraw    = true;
        pendingPublish = true;
    }

    // ── Encoder button → toggle on/off (device control screen only) ──────────
    if (M5Dial.BtnA.wasClicked() && screenState == DEVICE_CONTROL) {
        lastActivityMs = millis();
        if (screenDimmed) { M5Dial.Display.setBrightness(128); screenDimmed = false; needsRedraw = true; }
        Device& dev = devices[activeDeviceIndex];
        dev.on         = !dev.on;
        needsRedraw    = true;
        pendingPublish = true;
    }

    // ── Touch → tile select (home) / colour cycle or back (device screen) ────
    auto touch = M5Dial.Touch.getDetail();
    if (touch.wasClicked()) {
        lastActivityMs = millis();
        if (screenDimmed) { M5Dial.Display.setBrightness(128); screenDimmed = false; needsRedraw = true; }

        if (screenState == HOME) {
            activeDeviceIndex = (touch.y < 120) ? 0 : 1;
            screenState       = DEVICE_CONTROL;
            needsRedraw       = true;
        } else if (isBackIconTouch(touch.x, touch.y)) {
            screenState       = HOME;
            activeDeviceIndex = -1;
            needsRedraw       = true;
        } else {
            Device& dev = devices[activeDeviceIndex];
            dev.colorIndex = (dev.colorIndex + 1) % NUM_COLORS;
            needsRedraw    = true;
            pendingPublish = true;
        }
    }

    // ── Debounced publish ─────────────────────────────────────────────────────
    if (pendingPublish && (millis() - lastPublishMs >= PUBLISH_DEBOUNCE_MS)) {
        publishState();
    }

    // ── Redraw ────────────────────────────────────────────────────────────────
    if (needsRedraw) drawUI();

    // ── Screen fade after inactivity ──────────────────────────────────────────
    uint32_t idleMs = millis() - lastActivityMs;
    if (idleMs >= DIM_TIMEOUT_MS + FADE_DURATION_MS) {
        if (!screenDimmed) {
            M5Dial.Display.setBrightness(0);
            screenDimmed = true;
        }
    } else if (idleMs >= DIM_TIMEOUT_MS) {
        uint32_t fadeMs = idleMs - DIM_TIMEOUT_MS;
        uint8_t  bri    = (uint8_t)(128.0f * (FADE_DURATION_MS - fadeMs) / FADE_DURATION_MS);
        M5Dial.Display.setBrightness(bri);
    }
}
```

- [ ] **Step 2: Attempt a compile check**

Run:
```bash
"$HOME/.platformio/penv/Scripts/pio.exe" run -d "C:/Users/user/Documents/Claude/M5-Dial"
```

Expected: `SUCCESS` with no errors referencing `main.cpp`.

**Known pre-existing environment issue:** at the time this plan was written, this sandbox's installed `espressif32` PlatformIO platform (v5.2.0) predates the `m5stack-stamps3` board definition, so the build fails immediately with `Error: Unknown board ID 'm5stack-stamps3'` — before it even reaches compiling `main.cpp`. This is unrelated to this feature (it also fails on the unmodified pre-existing code) and was not introduced by this change. If you hit exactly that error:
- Build via the VS Code PlatformIO extension instead (it may have a newer platform installed), or
- Update the platform package: `"$HOME/.platformio/penv/Scripts/pio.exe" pkg update -p espressif32 -d "C:/Users/user/Documents/Claude/M5-Dial"` (downloads a newer toolchain — slow, and out of scope for this plan, so only do this if you actually need a working local build).

If the build gets past board resolution and fails with an actual C++ compiler error, fix the code before proceeding — that would indicate a real mistake in this task's code.

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "Add multi-device home screen and per-device control to M5 Dial firmware"
```

---

### Task 3: Duplicate Home Assistant automations for Lamp 2

**Files:**
- Modify: `ha_mqtt_light.yaml` (full replacement)

- [ ] **Step 1: Replace the entire contents of `ha_mqtt_light.yaml`**

```yaml
# Add these automations to Home Assistant (automations.yaml or via the UI).
# Replace light.your_lamp1_bulb / light.your_lamp2_bulb with your actual
# Cync entity IDs everywhere.
#
# Three automations per lamp:
#   1. Dial command  -> Cync bulb  (set brightness / colour)
#   2. Cync state    -> Dial       (keep dial in sync when bulb changes externally)
#   3. Dial boot     -> Dial       (answer the dial's startup state-poll request)

automation:
  # ── Lamp 1 ──────────────────────────────────────────────────────────────────
  - alias: "M5 Dial set Lamp 1 brightness and colour"
    trigger:
      - platform: mqtt
        topic: home/lamp1/set
    action:
      - variables:
          payload: "{{ trigger.payload_json }}"
      - service: light.turn_on
        target:
          entity_id: light.your_lamp1_bulb   # <- change to your entity_id
        data:
          brightness: "{{ payload.brightness | default(255) }}"
          rgb_color:
            - "{{ payload.color.r | default(255) }}"
            - "{{ payload.color.g | default(255) }}"
            - "{{ payload.color.b | default(255) }}"

  - alias: "Lamp 1 state publish to M5 Dial"
    # Fires whenever the Lamp 1 bulb state changes (HA dashboard, voice, etc.)
    # so the dial display stays in sync.
    trigger:
      - platform: state
        entity_id: light.your_lamp1_bulb     # <- same entity_id
    action:
      - service: mqtt.publish
        data:
          topic: home/lamp1/state
          retain: true
          payload: >
            {
              "state": "{{ states('light.your_lamp1_bulb') | upper }}",
              "brightness": {{ state_attr('light.your_lamp1_bulb','brightness') | default(255) | int }},
              "color": {
                "r": {{ (state_attr('light.your_lamp1_bulb','rgb_color') or [255,255,255])[0] }},
                "g": {{ (state_attr('light.your_lamp1_bulb','rgb_color') or [255,255,255])[1] }},
                "b": {{ (state_attr('light.your_lamp1_bulb','rgb_color') or [255,255,255])[2] }}
              }
            }

  - alias: "M5 Dial boot poll reply — Lamp 1"
    # The dial publishes to home/lamp1/get on every power-on to request the
    # current bulb state. This automation answers that request so the dial
    # shows the correct status immediately instead of waiting for a change.
    trigger:
      - platform: mqtt
        topic: home/lamp1/get
    action:
      - service: mqtt.publish
        data:
          topic: home/lamp1/state
          retain: true
          payload: >
            {
              "state": "{{ states('light.your_lamp1_bulb') | upper }}",
              "brightness": {{ state_attr('light.your_lamp1_bulb','brightness') | default(255) | int }},
              "color": {
                "r": {{ (state_attr('light.your_lamp1_bulb','rgb_color') or [255,255,255])[0] }},
                "g": {{ (state_attr('light.your_lamp1_bulb','rgb_color') or [255,255,255])[1] }},
                "b": {{ (state_attr('light.your_lamp1_bulb','rgb_color') or [255,255,255])[2] }}
              }
            }

  # ── Lamp 2 ──────────────────────────────────────────────────────────────────
  - alias: "M5 Dial set Lamp 2 brightness and colour"
    trigger:
      - platform: mqtt
        topic: home/lamp2/set
    action:
      - variables:
          payload: "{{ trigger.payload_json }}"
      - service: light.turn_on
        target:
          entity_id: light.your_lamp2_bulb   # <- change to your entity_id
        data:
          brightness: "{{ payload.brightness | default(255) }}"
          rgb_color:
            - "{{ payload.color.r | default(255) }}"
            - "{{ payload.color.g | default(255) }}"
            - "{{ payload.color.b | default(255) }}"

  - alias: "Lamp 2 state publish to M5 Dial"
    trigger:
      - platform: state
        entity_id: light.your_lamp2_bulb     # <- same entity_id
    action:
      - service: mqtt.publish
        data:
          topic: home/lamp2/state
          retain: true
          payload: >
            {
              "state": "{{ states('light.your_lamp2_bulb') | upper }}",
              "brightness": {{ state_attr('light.your_lamp2_bulb','brightness') | default(255) | int }},
              "color": {
                "r": {{ (state_attr('light.your_lamp2_bulb','rgb_color') or [255,255,255])[0] }},
                "g": {{ (state_attr('light.your_lamp2_bulb','rgb_color') or [255,255,255])[1] }},
                "b": {{ (state_attr('light.your_lamp2_bulb','rgb_color') or [255,255,255])[2] }}
              }
            }

  - alias: "M5 Dial boot poll reply — Lamp 2"
    trigger:
      - platform: mqtt
        topic: home/lamp2/get
    action:
      - service: mqtt.publish
        data:
          topic: home/lamp2/state
          retain: true
          payload: >
            {
              "state": "{{ states('light.your_lamp2_bulb') | upper }}",
              "brightness": {{ state_attr('light.your_lamp2_bulb','brightness') | default(255) | int }},
              "color": {
                "r": {{ (state_attr('light.your_lamp2_bulb','rgb_color') or [255,255,255])[0] }},
                "g": {{ (state_attr('light.your_lamp2_bulb','rgb_color') or [255,255,255])[1] }},
                "b": {{ (state_attr('light.your_lamp2_bulb','rgb_color') or [255,255,255])[2] }}
              }
            }
```

- [ ] **Step 2: Commit**

```bash
git add ha_mqtt_light.yaml
git commit -m "Duplicate HA MQTT automations for Lamp 2, rename Lamp 1 topics"
```

---

### Task 4: Update README for two-lamp home-menu behavior

**Files:**
- Modify: `README.md` (full replacement)

- [ ] **Step 1: Replace the entire contents of `README.md`**

```markdown
# M5 Dial – Multi-Lamp Cync Controller via Home Assistant

Control two Cync RGB bulbs from an M5 Dial:
- **Home screen** shows a tile per lamp (name + on/off indicator) — **tap a tile** to open that lamp's controls
- **Rotate** the dial → adjust brightness
- **Press** the dial button → toggle on / off
- **Tap** the screen → cycle through 10 colour presets
- **Tap the back icon** (top-left corner) → return to the home screen
- The display shows a brightness arc, percentage, and current colour name

## Prerequisites

| Requirement | Notes |
|---|---|
| M5 Dial | ESP32-S3 based |
| Home Assistant | With Cync integration already working for both bulbs |
| Mosquitto (or any MQTT broker) | Can run as an HA add-on |
| PlatformIO | VS Code extension or CLI |

## Setup

### 1. Edit `src/config.h`

Fill in your WiFi credentials, MQTT broker IP/credentials, and (optionally) each lamp's display name and topic names.

### 2. Flash the M5 Dial

```bash
pio run --target upload
pio device monitor   # to see serial logs
```

### 3. Configure Home Assistant

1. Install the **Mosquitto broker** HA add-on (or use an external broker).
2. Enable the **MQTT integration** in HA → Settings → Devices & Services.
3. Find each Cync bulb's entity ID (e.g. `light.living_room_cync`, `light.bedroom_cync`).
4. Copy the contents of `ha_mqtt_light.yaml` into your `automations.yaml`
   (or import via the HA automation UI) and replace `light.your_lamp1_bulb` /
   `light.your_lamp2_bulb` with your actual entity IDs.
5. Restart Home Assistant.

## Colour presets (in order, cycling on touch)

White → Warm → Red → Orange → Yellow → Green → Cyan → Blue → Purple → Pink

## Screens

**Home** — one tile per lamp (name + on/off dot). Tap a tile to control that lamp.

```
   ╭──────────────╮
   │   Lamp 1   ●  │
   ├──────────────┤
   │   Lamp 2   ●  │
   ╰──────────────╯
```

**Lamp control** — same as before, plus a back icon (`<`) in the top-left corner.

```
        ╭────────────╮
      ╱ <  brightness ╲
     │     arc (270°)   │
     │                  │
     │       78%        │  ← large text
     │       Blue       │  ← colour name (tinted)
     │  · · ● · · ·    │  ← colour dot ring
      ╲                ╱
       ╰──────────────╯
```

The arc colour matches the currently selected colour preset.
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "Update README for two-lamp home menu"
```

---

### Task 5: Manual hardware verification

There is no automated way to verify dial behavior — this task is a checklist to run on the actual M5 Dial after flashing.

**Files:** none (verification only)

- [ ] **Step 1: Flash the firmware**

```bash
cd "C:/Users/user/Documents/Claude/M5-Dial"
"$HOME/.platformio/penv/Scripts/pio.exe" run --target upload
```

If this environment's PlatformIO platform can't resolve the `m5stack-stamps3` board (see the note in Task 2, Step 2), flash from the VS Code PlatformIO extension instead.

- [ ] **Step 2: Verify boot and home screen**

Power on the dial. Confirm:
- It connects to WiFi and MQTT (no stuck "Connecting WiFi..." or "Syncing..." screen).
- The home screen appears with two tiles: `Lamp 1` and `Lamp 2`, each with an on/off dot reflecting their actual current state in Home Assistant.

- [ ] **Step 3: Verify entering and controlling each device**

For each lamp:
- Tap its tile — confirm the dial switches to that lamp's control screen (brightness arc, back icon top-left).
- Rotate the dial — confirm brightness changes on screen and the bulb's brightness changes in HA.
- Press the dial button — confirm the bulb toggles on/off, both on-screen and in HA.
- Tap the screen (away from the back icon) — confirm the colour preset cycles, both on-screen and in HA.

- [ ] **Step 4: Verify back navigation and independence between lamps**

- Tap the back icon — confirm the dial returns to the home screen, and the tile's on/off dot reflects the state you just set.
- Enter Lamp 1, change its brightness/colour, go back, enter Lamp 2 — confirm Lamp 2's brightness/colour are unaffected by the Lamp 1 change (they're independent devices).

- [ ] **Step 5: Verify external state sync**

From the Home Assistant dashboard (not the dial), change one lamp's brightness/colour/on-off state. Go back to the dial's home screen (or re-enter that lamp's control screen) and confirm it reflects the change.

- [ ] **Step 6: Push the branch**

```bash
cd "C:/Users/user/Documents/Claude/M5-Dial"
git push
```
