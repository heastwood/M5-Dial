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
    needsRedraw = true; // triggers the first draw of the home screen
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
            if (pendingPublish) publishState();
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
