# Home Assistant over HTTP — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let Home Assistant (on a Raspberry Pi 5) move the desk to presets, a slider value or ±1 cm, and show the measured height, with the lidar off except during a request.

**Architecture:** The ESP32-C6 firmware keeps its WebServer but replies only after each action finishes, with `{"height": …}` JSON. Pure decision logic (clamping, parsing, direction, dead band, JSON) moves to a header that compiles both on the ESP and on the Mac for unit tests. Home Assistant gets one package file: a single `rest_command`, one `mode: single` dispatcher script that feeds a trigger-based template sensor, and thin wrapper scripts and a slider.

**Tech Stack:** Arduino core esp32 3.3.11, Pololu VL53L1X 1.3.1, arduino-cli 1.5.1 (FQBN `esp32:esp32:XIAO_ESP32C6`), host C++17 (`c++`) for logic tests, Home Assistant YAML packages; config checking via `uvx --from homeassistant hass --script check_config`.

**Spec:** `docs/superpowers/specs/2026-11-22-home-assistant-rest-design.md`

**Branch:** work on `home-assistant-rest` (create from `main` before Task 1).

## Global Constraints

- Height range 64–114 cm; every target (absolute or nudged) is clamped into it.
- Dead band 0.5 cm: if |target − current| ≤ 0.5 the motor is not driven.
- `MAX_MOVE_MS = 20000` hard safety cutoff stays; PIN_UP and PIN_DOWN are never HIGH together; motor pin always LOW on every exit path.
- Lidar (XSHUT) is LOW except inside a request handler.
- Presets: Sit = 72 cm, Stand = 111 cm.
- HA `rest_command` timeout 30 s (above the 20 s move cutoff).
- WiFi credentials stay `"XXX"` in the committed `main.ino`.
- No MQTT, ESPHome or HA add-ons.

## Review Focus

1. `cm` missing or non-numeric (`/target?cm=abc`, `/target`) → HTTP 400 and the desk does **not** move. (Today `toInt()` turns garbage into 0 → clamped to 64 → desk drives all the way down.) Pinned by `parseCm` tests in Task 1 and an "I run" curl in Task 2.
2. HA restart restoring the slider's value must **not** move the desk. Pinned by the automation's `from_state` condition in Task 3 and an "I run" restart check.
3. Target equal to or within 0.5 cm of the current height → no motor pulse, reply 200 with current height. Pinned by `chooseDirection` tests in Task 1.
4. Nudge or target beyond 64/114 → clamped, never driven past the limit. Pinned by `clampHeight` tests in Task 1.
5. ESP unreachable or a button pressed mid-move → HA shows a notification / drops the press; it never queues a second move behind the first. Pinned by `continue_on_error` + `mode: single` in Task 3 and an "I run" check.

## Acceptance checks

| # | Check | Who |
|---|---|---|
| A1 | `esp_program/tests/run_tests.sh` prints `All tests passed` | agent runs |
| A2 | `arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C6 esp_program/main` succeeds | agent runs |
| A3 | HA `check_config` on the package prints no `Incorrect config` / `ERROR` | agent runs |
| H1 | Flash; `curl http://<ip>/height` → `{"height":…}` matching a tape measure ±1 cm | I run |
| H2 | `curl "http://<ip>/target?cm=90"` moves to ~90 and replies with final height | I run |
| H3 | `curl "http://<ip>/nudge?cm=1"` / `cm=-1` moves ~1 cm | I run |
| H4 | `curl "http://<ip>/target?cm=abc"` and `/target` → 400, desk does not move | I run |
| H5 | `curl "http://<ip>/target?cm=200"` stops at ~114 | I run |
| H6 | In HA: Sit, Stand, ±1, Measure, slider all work and update **Desk height** | I run |
| H7 | Press Stand, then Sit within 2 s → only Stand happens | I run |
| H8 | Restart HA → desk does not move | I run |
| H9 | Unplug ESP, press Measure → HA notification, no hang | I run |

---

### Task 1: Pure desk logic with host tests

**Files:**
- Create: `esp_program/main/desk_logic.h`
- Create: `esp_program/tests/test_desk_logic.cpp`
- Create: `esp_program/tests/run_tests.sh`

**Interfaces:**
- Consumes: nothing.
- Produces (all `inline`, header-only, only `<cmath>`, `<cstdio>`, `<cstdlib>`, `<cstddef>` — no Arduino headers):
  ```cpp
  constexpr float kMinHeightCm = 64.0f;
  constexpr float kMaxHeightCm = 114.0f;
  constexpr float kDeadBandCm  = 0.5f;
  enum class Direction { None, Up, Down };
  struct MoveResult { bool ok; float heightCm; const char* error; };

  float clampHeight(float cm);
  bool parseCm(const char* text, float* out);          // false on nullptr, empty, trailing garbage, non-finite
  Direction chooseDirection(float currentCm, float targetCm); // None inside dead band
  bool reachedTarget(Direction dir, float currentCm, float targetCm); // Up: current >= target; Down: current <= target; None: true
  int formatHeightJson(char* buf, size_t len, float heightCm);       // {"height":90.1}  (one decimal)
  int formatErrorJson(char* buf, size_t len, const char* message);   // {"error":"<message>"}
  ```

- [ ] **Step 1: Write the failing tests** in `esp_program/tests/test_desk_logic.cpp` — a `main()` using a small `CHECK(cond)` macro that counts failures, prints each failing line, and prints `All tests passed` and returns 0 only when none fail. Cases:

  ```cpp
  // clampHeight
  CHECK(clampHeight(50) == 64);   CHECK(clampHeight(64) == 64);
  CHECK(clampHeight(90) == 90);   CHECK(clampHeight(200) == 114);
  // parseCm
  float v;
  CHECK(parseCm("90", &v) && v == 90.0f);
  CHECK(parseCm("-1", &v) && v == -1.0f);
  CHECK(parseCm("1.5", &v) && v == 1.5f);
  CHECK(parseCm(" 1", &v) && v == 1.0f);   // '+' arrives URL-decoded as space
  CHECK(!parseCm("abc", &v));  CHECK(!parseCm("", &v));
  CHECK(!parseCm("12abc", &v)); CHECK(!parseCm(nullptr, &v));
  CHECK(!parseCm("nan", &v));  CHECK(!parseCm("inf", &v));
  // chooseDirection
  CHECK(chooseDirection(80, 90) == Direction::Up);
  CHECK(chooseDirection(90, 80) == Direction::Down);
  CHECK(chooseDirection(90, 90) == Direction::None);
  CHECK(chooseDirection(90, 90.5f) == Direction::None);
  CHECK(chooseDirection(90, 89.5f) == Direction::None);
  CHECK(chooseDirection(90, 90.6f) == Direction::Up);
  // reachedTarget
  CHECK(!reachedTarget(Direction::Up, 89, 90));  CHECK(reachedTarget(Direction::Up, 90, 90));
  CHECK(!reachedTarget(Direction::Down, 91, 90)); CHECK(reachedTarget(Direction::Down, 90, 90));
  CHECK(reachedTarget(Direction::None, 50, 90));
  // nudge composition stays within limits
  CHECK(clampHeight(113.5f + 1) == 114); CHECK(clampHeight(64.2f - 1) == 64);
  // JSON
  char b[64];
  formatHeightJson(b, sizeof b, 90.14f);  CHECK(strcmp(b, "{\"height\":90.1}") == 0);
  formatErrorJson(b, sizeof b, "sensor lost"); CHECK(strcmp(b, "{\"error\":\"sensor lost\"}") == 0);
  ```

  `run_tests.sh` (executable, `set -euo pipefail`): `cd` to its own directory, `c++ -std=c++17 -Wall -Wextra -Werror -I ../main test_desk_logic.cpp -o "${TMPDIR:-/tmp}/test_desk_logic"` then run the binary.

- [ ] **Step 2: Run to verify it fails**

  Run: `esp_program/tests/run_tests.sh`
  Expected: compile error, `desk_logic.h: No such file or directory`.

- [ ] **Step 3: Implement `esp_program/main/desk_logic.h`** with the interface above. `parseCm` uses `strtof` with an end-pointer check (end must reach `'\0'`, at least one char consumed) plus `std::isfinite`. JSON via `snprintf` with `%.1f`.

- [ ] **Step 4: Run to verify it passes**

  Run: `esp_program/tests/run_tests.sh`
  Expected: `All tests passed`, exit 0. (A1)

- [ ] **Step 5: Commit**

  ```bash
  git add esp_program/main/desk_logic.h esp_program/tests/
  git commit -m "Add host-tested desk logic (clamp, parse, direction, JSON)"
  ```

---

### Task 2: Firmware endpoints reply with height

**Files:**
- Modify: `esp_program/main/main.ino` (`move_to_target`, `toTarget`, `setup`; add handlers)
- Modify: `README.md` ("How it works", "Repository layout" firmware table, "Building the firmware" — fix `simple_up_down.ino` → `main/main.ino`, list the three endpoints, add the `run_tests.sh` line)

**Interfaces:**
- Consumes: everything in `desk_logic.h` (`#include "desk_logic.h"`).
- Produces (HTTP, all `GET`, all replies `application/json`):
  - `/target?cm=N` → 200 `{"height":H}` | 400 `{"error":"missing or invalid cm"}` | 503 `{"error":"<reason>"}`
  - `/nudge?cm=D` → same codes; target = `clampHeight(current + D)`
  - `/height` → 200 `{"height":H}` | 503 `{"error":"sensor unavailable"}`
  - 503 reasons used: `"sensor unavailable"` (lidarOn fails or first reading invalid), `"sensor lost"` (>10 consecutive failed reads mid-move), `"safety timeout"` (MAX_MOVE_MS hit).

- [ ] **Step 1: Refactor the move into `MoveResult moveTo(float targetCm)`** (lidar must already be on; caller turns it off). Takes the initial reading; `chooseDirection`; on `None` returns `{true, current, nullptr}` without touching the motor pins; otherwise drives as today, stopping on `reachedTarget`, timeout or sensor loss. Motor pin LOW on every exit. After stopping, wait 300 ms and take one more reading for the reported height (desk coasts); fall back to the last good reading if that read fails.

- [ ] **Step 2: Add `MoveResult measureOnce()`** — `lidarOn`, one `readDistanceCm`, `lidarOff`; failure → `{false, 0, "sensor unavailable"}`.

- [ ] **Step 3: Add the handlers** `handleTarget`, `handleNudge`, `handleHeight` and a `sendResult(const MoveResult&)` that sends 200 + `formatHeightJson` or 503 + `formatErrorJson`. `/target` and `/nudge` validate `cm` with `parseCm(server.arg("cm").c_str(), &v)` **before** turning the lidar on; invalid → 400, no lidar, no motor. Each handler wraps its work in `lidarOn()` … `lidarOff()` so the lidar is off on every path. Register them in `setup()`; remove the old `toTarget`.

- [ ] **Step 4: Compile**

  Run: `arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C6 esp_program/main`
  Expected: `Sketch uses … bytes`, no errors. (A2) Also re-run `esp_program/tests/run_tests.sh` → `All tests passed`.

- [ ] **Step 5: Confirm credentials are still placeholders**

  Run: `grep -n 'const char\* \(ssid\|password\)' esp_program/main/main.ino`
  Expected: both lines show `"XXX"`.

- [ ] **Step 6: Commit**

  ```bash
  git add esp_program/main/main.ino README.md
  git commit -m "Firmware: /target, /nudge, /height reply with measured height"
  ```

- [ ] **Step 7: Hand over H1–H5 to the user** (I run).

---

### Task 3: Home Assistant package

**Files:**
- Create: `homeassistant/packages/desk.yaml`
- Modify: `README.md` — new "Home Assistant" section: enable packages (`homeassistant: packages: !include_dir_named packages` in `configuration.yaml`), copy `desk.yaml` to `/config/packages/`, replace `DESK_IP`, DHCP reservation note, restart, plus the dashboard card YAML below.

**Interfaces:**
- Consumes: the HTTP API from Task 2.
- Produces (entity / action ids the dashboard and user rely on):
  - `rest_command.desk` — url `http://DESK_IP/{{ endpoint }}{% if cm is defined %}?cm={{ cm }}{% endif %}`, `method: GET`, `timeout: 30`. `DESK_IP` appears only here.
  - `script.desk_call` — `mode: single`, fields `endpoint` (required), `cm` (optional). Calls `rest_command.desk` with `continue_on_error: true` and `response_variable: r`. If `r is defined and r.status == 200` → fire event `desk_height_reported` with `height: "{{ r.content.height }}"`; otherwise `persistent_notification.create` (`notification_id: desk_error`, title "Desk", message with the error from `r.content.error` when present, else "Desk controller unreachable").
  - `sensor.desk_height` — trigger-based `template:` sensor on event `desk_height_reported`, name "Desk height", `unique_id: desk_height`, `unit_of_measurement: cm`, `device_class: distance`, `state_class: measurement`, state `{{ trigger.event.data.height }}`.
  - `input_number.desk_target` — "Desk target", min 64, max 114, step 1, `unit_of_measurement: cm`, `mode: slider`.
  - Scripts, each `mode: single`, each one `script.desk_call` action: `desk_sit` (target, 72), `desk_stand` (target, 111), `desk_up_1` (nudge, 1), `desk_down_1` (nudge, -1), `desk_measure` (height, no cm).
  - `automation` "Desk – move to slider target": trigger `state` on `input_number.desk_target`; condition template `{{ trigger.from_state is not none and trigger.from_state.state not in ['unknown', 'unavailable'] }}`; action `script.desk_call` endpoint `target`, cm `{{ trigger.to_state.state | float | round(0) | int }}`.

- [ ] **Step 1: Write a failing check** — create a scratch HA config dir with `configuration.yaml` containing `homeassistant: {name: t, packages: !include_dir_named packages}` and a `packages` symlink to `homeassistant/packages`, then run:

  ```bash
  UV_CACHE_DIR=<scratch>/uvcache uvx --python 3.13 --from homeassistant \
    hass --script check_config -c <scratch>/hacheck 2>&1 | grep -v '^INFO'
  ```
  Expected before Step 2: failure (packages dir missing).

- [ ] **Step 2: Write `homeassistant/packages/desk.yaml`** with exactly the ids and values listed in Interfaces.

- [ ] **Step 3: Run the check**

  Same command. Expected: output contains `Testing configuration`, and **no** `Incorrect config`, `Invalid config` or `ERROR` lines. (A3)

- [ ] **Step 4: Dashboard card in README**

  ```yaml
  type: entities
  title: Desk
  entities:
    - sensor.desk_height
    - input_number.desk_target
    - type: buttons
      entities:
        - entity: script.desk_down_1
          name: "-1 cm"
        - entity: script.desk_up_1
          name: "+1 cm"
        - entity: script.desk_sit
          name: Sit
        - entity: script.desk_stand
          name: Stand
        - entity: script.desk_measure
          name: Measure
  ```

- [ ] **Step 5: Commit**

  ```bash
  git add homeassistant/ README.md
  git commit -m "Add Home Assistant package for desk control"
  ```

- [ ] **Step 6: Hand over H6–H9 to the user** (I run).
