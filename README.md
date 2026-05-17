# M5 Dial – Cync Bulb Controller via Home Assistant

Control a Cync RGB bulb from an M5 Dial:  
- **Rotate** the dial → adjust brightness  
- **Press** the dial button → toggle on / off  
- **Tap** the screen → cycle through 10 colour presets  
- The display shows a brightness arc, percentage, and current colour name

## Prerequisites

| Requirement | Notes |
|---|---|
| M5 Dial | ESP32-S3 based |
| Home Assistant | With Cync integration already working |
| Mosquitto (or any MQTT broker) | Can run as an HA add-on |
| PlatformIO | VS Code extension or CLI |

## Setup

### 1. Edit `src/config.h`

Fill in your WiFi credentials, MQTT broker IP/credentials, and (optionally) change the topic names.

### 2. Flash the M5 Dial

```bash
pio run --target upload
pio device monitor   # to see serial logs
```

### 3. Configure Home Assistant

1. Install the **Mosquitto broker** HA add-on (or use an external broker).
2. Enable the **MQTT integration** in HA → Settings → Devices & Services.
3. Find your Cync bulb entity ID (e.g. `light.living_room_cync`).
4. Copy the contents of `ha_mqtt_light.yaml` into your `automations.yaml`  
   (or import via the HA automation UI) and replace `light.your_cync_bulb`  
   with your actual entity ID.
5. Restart Home Assistant.

## Colour presets (in order, cycling on touch)

White → Warm → Red → Orange → Yellow → Green → Cyan → Blue → Purple → Pink

## Display layout

```
        ╭────────────╮
       ╱  brightness  ╲
      │   arc (270°)   │
      │                │
      │      78%       │  ← large text
      │      Blue      │  ← colour name (tinted)
      │  · · ● · · ·  │  ← colour dot ring
       ╲              ╱
        ╰────────────╯
```

The arc colour matches the currently selected colour preset.
