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

// ── State ─────────────────────────────────────────────────────────────────────
static int      brightness    = 128; // 0-255
static int      colorIndex    = 0;
static bool     lightOn       = true;
static bool     needsRedraw   = true;
static bool     pendingPublish = false;
static uint32_t lastPublishMs  = 0;
static long     lastEncoderVal = 0;
static uint32_t lastActivityMs = 0;
static bool     screenDimmed   = false;

static constexpr uint32_t DIM_TIMEOUT_MS  = 7000;
static constexpr uint32_t FADE_DURATION_MS = 1000;

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

static void drawBrightnessArc(int pct) {
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

static void drawCenterText(int pct, bool on) {
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

static void drawColorDots() {
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

void drawUI() {
    int pct = lightOn ? brightness * 100 / 255 : 0;
    drawBrightnessArc(pct);
    drawCenterText(pct, lightOn);
    // Color dots only when on
    if (lightOn) drawColorDots();
    needsRedraw = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT
// ─────────────────────────────────────────────────────────────────────────────

void publishState() {
    if (!mqtt.connected()) return;

    JsonDocument doc;
    doc["state"]      = lightOn ? "ON" : "OFF";
    doc["brightness"] = brightness;
    JsonObject color  = doc["color"].to<JsonObject>();
    color["r"] = COLORS[colorIndex].r;
    color["g"] = COLORS[colorIndex].g;
    color["b"] = COLORS[colorIndex].b;

    char buf[128];
    serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(LIGHT_CMD_TOPIC, buf, /*retain=*/false);
    lastPublishMs  = millis();
    pendingPublish = false;
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
    // Parse state updates pushed from HA so the dial stays in sync
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return;

    bool changed = false;

    if (doc["state"].is<const char*>()) {
        bool on = strcmp(doc["state"], "ON") == 0;
        if (on != lightOn) { lightOn = on; changed = true; }
    }
    if (doc["brightness"].is<int>()) {
        int b = doc["brightness"].as<int>();
        b = constrain(b, 0, 255);
        if (b != brightness) { brightness = b; changed = true; }
    }
    if (doc["color"].is<JsonObject>()) {
        uint8_t r = doc["color"]["r"] | 255;
        uint8_t g = doc["color"]["g"] | 255;
        uint8_t b = doc["color"]["b"] | 255;
        // Find closest preset by minimising squared distance
        int best = colorIndex, bestDist = INT_MAX;
        for (int i = 0; i < NUM_COLORS; i++) {
            int dr = r - COLORS[i].r, dg = g - COLORS[i].g, db = b - COLORS[i].b;
            int d = dr*dr + dg*dg + db*db;
            if (d < bestDist) { bestDist = d; best = i; }
        }
        if (best != colorIndex) { colorIndex = best; changed = true; }
    }

    if (changed) needsRedraw = true;
}

void reconnectMqtt() {
    static uint32_t lastAttemptMs = 0;
    if (millis() - lastAttemptMs < 5000) return;
    lastAttemptMs = millis();

    if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
        mqtt.subscribe(LIGHT_STATE_TOPIC);
    }
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

    M5Dial.Display.fillScreen(TFT_BLACK);
    lastEncoderVal = M5Dial.Encoder.read();
    lastActivityMs = millis();
    needsRedraw = true;
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

    // ── Rotary encoder → brightness ───────────────────────────────────────────
    long encVal = M5Dial.Encoder.read();
    long delta  = encVal - lastEncoderVal;
    if (delta != 0) {
        lastEncoderVal = encVal;
        lastActivityMs = millis();
        if (screenDimmed) { M5Dial.Display.setBrightness(128); screenDimmed = false; needsRedraw = true; }
        if (!lightOn) { lightOn = true; } // first turn wakes the light

        brightness = constrain(brightness + (int)delta * BRIGHTNESS_STEP, 0, 255);
        needsRedraw    = true;
        pendingPublish = true;
    }

    // ── Encoder button → toggle on/off ────────────────────────────────────────
    if (M5Dial.BtnA.wasClicked()) {
        lastActivityMs = millis();
        if (screenDimmed) { M5Dial.Display.setBrightness(128); screenDimmed = false; needsRedraw = true; }
        lightOn        = !lightOn;
        needsRedraw    = true;
        pendingPublish = true;
    }

    // ── Touch → next colour ───────────────────────────────────────────────────
    auto touch = M5Dial.Touch.getDetail();
    if (touch.wasClicked()) {
        lastActivityMs = millis();
        if (screenDimmed) { M5Dial.Display.setBrightness(128); screenDimmed = false; needsRedraw = true; }
        colorIndex = (colorIndex + 1) % NUM_COLORS;
        needsRedraw    = true;
        pendingPublish = true;
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
