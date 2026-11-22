# Home Assistant integration over HTTP — design

## Intent

Control the desk from the Home Assistant install on a Raspberry Pi 5: see the
current height, move to presets, set a height with a slider, and nudge up/down
in small increments. The lidar must stay powered off except while moving or
when a reading is explicitly requested.

## Goals

- HA shows the desk height (last known, updated after every action).
- Sit / Stand presets, a target-height slider (64–114 cm), and ±1 cm nudges.
- A "Measure" button that takes a fresh reading on demand.
- Lidar off (XSHUT low) at all other times — no polling.

## Non-goals

- Stop button / cancelling a move mid-way (would need a non-blocking move loop).
- Detecting moves made with the desk's own physical buttons.
- Battery operation or ESP deep sleep — the ESP stays on WiFi.
- MQTT, ESPHome, or any extra add-on on the Pi.

## Approaches considered

| Approach | Trade-off | Outcome |
|---|---|---|
| **HTTP endpoints + HA `rest_command`** | No broker, keeps the working Arduino firmware; HA config is hand-written YAML; no mid-move stop | **Chosen** |
| Arduino + MQTT discovery | Entities auto-appear, push updates; needs Mosquitto add-on — push brings little since the lidar is off anyway | Rejected |
| ESPHome rewrite | Most HA-native; VL53L1X is only a community component and re-initialising it after XSHUT power-off is unlikely to work | Rejected |

## Design

### Firmware (`esp_program/main/main.ino`)

Keep the existing lidar, WiFi/antenna and motor logic. Change the HTTP API so
each request replies **after** the action finishes, with the measured height:

| Request | Action | Reply (`application/json`) |
|---|---|---|
| `GET /target?cm=N` | Clamp N to 64–114, move there | `{"height": 90.1}` |
| `GET /nudge?cm=D` | Measure, target = current + D (clamped), move | `{"height": 91.2}` |
| `GET /height` | Lidar on, one reading, lidar off | `{"height": 92.4}` |

- Lidar or sensor failure → HTTP 503 with `{"error": "..."}`, motors off.
- `move_to_target` takes a float target and returns the final measured height.
- If already within 0.5 cm of the target, don't drive the motor (dead band).
- The 20 s hard safety timeout and "never both pins high" rule stay as they are.

### Home Assistant (`homeassistant/desk.yaml`, a package file)

One file the user copies into HA's `packages/` folder:

- `rest_command`s for the three endpoints, with a timeout above the 20 s move
  cutoff.
- A trigger-based template sensor **Desk height** (cm), updated from an event
  that the scripts fire with each reply's height.
- Scripts: `desk_sit`, `desk_stand`, `desk_up_1`, `desk_down_1`,
  `desk_measure`, all going through one dispatcher `desk_call` — all
  `mode: single`, so presses during a move are dropped rather than queued.
- An `input_number` **Desk target** slider (64–114, step 1) and an automation
  that calls `desk_call` when it changes (not on HA restart).
- A dashboard card example in the README.

### Network

The user reserves the ESP's IP in the router (DHCP reservation); the package
has a single place to set it.

## Calls made by Claude (correct any)

- Sit = 72 cm (placeholder), Stand = 111 cm (user's value); one line each to change.
- The slider is not moved back to match the measured height (doing so would
  re-trigger a move).
- HA config lives in the repo under `homeassistant/`.
- README gets a Home Assistant section and the wrong `simple_up_down.ino`
  filename is fixed to `main/main.ino`.
- WiFi credentials stay as `XXX` placeholders in the committed copy, as today.

## Left to the builder

Exact JSON handling in the HA scripts and the HA-version details of
`rest_command` response variables (to be checked against current HA docs).
