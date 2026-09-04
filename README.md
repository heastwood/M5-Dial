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
