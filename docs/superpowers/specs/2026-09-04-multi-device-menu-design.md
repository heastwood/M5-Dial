# M5 Dial: Multi-Device Home Menu — Design

Date: 2026-09-04

## Background

The M5 Dial currently controls a single Cync bulb: rotate adjusts brightness, press toggles on/off, tap cycles through 10 color presets. All state (brightness, color, on/off) and MQTT topics are hardcoded globals/defines for that one lamp.

## Goal

Control a second lamp (same Cync-via-HA-MQTT pattern as today) from the same dial, and structure the firmware so more devices can be added later — including non-light devices such as a volume control — without rewriting the screen/interaction logic each time.

Out of scope for this spec: implementing the volume control itself. Only the extensibility (a device list + screen state machine) is built now; a second device type is a future project.

## Approach

Generalize the current single-lamp state into a small `Device` array (name + MQTT topics + cached state) and parametrize the existing rotate/press/tap control logic to operate on "whichever device is active" instead of hardcoded globals. Add a home screen that lists devices as tiles; tapping a tile enters that device's control screen (today's existing UI, reused).

Rejected alternatives:
- **Copy-paste duplication** (parallel `lamp1_*`/`lamp2_*` variables and an if/else): fastest to write but doesn't scale past 2 devices and the copies drift out of sync as bugs get fixed in one but not the other.
- **Full polymorphic device-type plugin system** (abstract `Controllable` interface, `LightController`/`VolumeController` subclasses, registry): the natural design once there are genuinely different device *types* to control, but overkill for two lights — no second type exists yet to justify the abstraction.

## Architecture

### Data model (`main.cpp`)

```cpp
struct Device {
  const char* name;
  const char* cmdTopic;
  const char* stateTopic;
  const char* getTopic;
  bool stateKnown;   // false until first state message arrives
  bool on;
  int brightness;    // 0-255
  RGBColor color;
};

Device devices[2];
```

### Screen state machine

- `ScreenState { HOME, DEVICE_CONTROL }`
- `int activeDeviceIndex` (meaningful only in `DEVICE_CONTROL`)
- Boot sequence: connect WiFi/MQTT → subscribe to **both** devices' `stateTopic` → publish to **both** devices' `getTopic` (requests current state, same boot-poll pattern as today, just looped over both devices) → show `HOME`.

### Screens

- `drawHomeScreen()`: two tiles (top/bottom half of the round display), each showing device name + a small on/off indicator dot (grey/neutral if `stateKnown == false`).
- `drawDeviceScreen()`: today's existing brightness arc / color name / preset dots UI, unchanged in appearance, but reading/writing `devices[activeDeviceIndex]` instead of the current hardcoded globals. Adds a small back-icon tap zone (top-left corner) that switches `ScreenState` back to `HOME`.

### Touch/input handling

- `HOME`: tap in top-half zone → `activeDeviceIndex = 0`, `ScreenState = DEVICE_CONTROL`. Tap in bottom-half zone → same for index 1.
- `DEVICE_CONTROL`: rotate/press/tap behave exactly as today (brightness/toggle/color-cycle) but scoped to `devices[activeDeviceIndex]`. Tap on the back-icon zone → `ScreenState = HOME` (redraw tiles, reflecting latest cached state).

### MQTT topics (`config.h`)

Two topic sets following the existing `home/cync/*` pattern:

```cpp
#define LAMP1_NAME        "Lamp 1"
#define LAMP1_CMD_TOPIC   "home/lamp1/set"
#define LAMP1_STATE_TOPIC "home/lamp1/state"
#define LAMP1_GET_TOPIC   "home/lamp1/get"

#define LAMP2_NAME        "Lamp 2"
#define LAMP2_CMD_TOPIC   "home/lamp2/set"
#define LAMP2_STATE_TOPIC "home/lamp2/state"
#define LAMP2_GET_TOPIC   "home/lamp2/get"
```

(Lamp 1's topics change from `home/cync/*` to `home/lamp1/*` for naming consistency — the corresponding HA automations must be updated to match.)

## Home Assistant side

Duplicate the 3 automations already in `ha_mqtt_light.yaml` for the second lamp, using the `home/lamp2/*` topics and the second bulb's actual entity ID (placeholder `light.your_second_cync_bulb`, same pattern as today's `light.your_cync_bulb`). Existing lamp-1 automations get their topics renamed from `home/cync/*` to `home/lamp1/*` to match.

## Error handling

- MQTT connect/subscribe/reconnect logic (already present in `main.cpp`) is generalized to loop over `devices[]` instead of a single lamp.
- A device whose state hasn't arrived yet (`stateKnown == false`) shows a neutral tile indicator rather than assuming on/off, and its control screen shows a "waiting for state" indication if entered before the first state message arrives.

## Testing

This is embedded firmware with no existing automated test suite. Validation is manual, on hardware:
1. Home screen shows both tiles with correct initial on/off state after boot.
2. Tapping each tile enters that device's control screen.
3. Rotate/press/tap work correctly and independently for each device.
4. Back icon returns to home screen with tiles reflecting latest state.
5. Changing a bulb externally (HA dashboard/voice) updates its tile the next time home is viewed.

The user will flash and test on the actual M5 Dial hardware.
