# Desk Controller

Open-hardware controller for a motorized (sit/stand) desk. A
[Seeed XIAO ESP32-C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/)
measures the desk height with a VL53L1X time-of-flight lidar and drives the
desk's up/down motor lines until it reaches a requested height, exposed over a
small WiFi web server that [Home Assistant](#home-assistant) can control.

> `skrivebord` is Danish for *desk*.

<p align="center">
  <img src="casing/casing.png" alt="3D-printed enclosure holding the controller PCB" width="640">
  <br>
  <em>The press-fit 3D-printed enclosure — sensor opening on top, ventilation slots on the front.</em>
</p>

## How it works

- The ESP32-C6 hosts three HTTP endpoints. Each replies only once the action
  is finished, with the measured height as JSON:

  | Request | Action | Reply |
  |---------|--------|-------|
  | `GET /target?cm=90` | Move to 90 cm | `{"height":90.1}` |
  | `GET /nudge?cm=1` (or `-1`) | Move 1 cm up (or down) from where it is | `{"height":91.2}` |
  | `GET /height` | Take one reading | `{"height":92.4}` |

- The lidar is powered off (XSHUT low) except while a request runs.
- On a move it reads the current height, then holds the UP or DOWN line until
  the measured height reaches the target. Within 0.5 cm of the target it does
  not move at all.
- Targets are clamped to a safe range (64–114 cm) and every move has a 20 s
  hard safety timeout, so the motor is never left running.
- A missing or non-numeric `cm` returns `400`; a lidar failure, lost sensor or
  safety timeout returns `503` with `{"error":"..."}`.
- If WiFi drops (e.g. the router restarts), the controller retries every 15 s
  and restarts itself after 5 minutes offline, so it never needs to be
  unplugged to reconnect. At boot it waits at most 30 s for WiFi before
  carrying on and retrying in the background.
- A `/target` or `/nudge` sent while the desk is already moving is answered
  `409` (`{"error":"busy"}`) once the move ends, instead of starting a second
  move. `/nudge` never moves against the requested direction, even if the desk
  is outside 64–114 cm.

## Repository layout

| Path | Contents |
|------|----------|
| `kicad_files/` | KiCad 8 project — schematic, PCB, and fabrication gerbers |
| `casing/` | 3D-printed enclosure — see below |
| `esp_program/` | ESP32-C6 firmware (Arduino / `.ino`) |

### `casing/` — enclosure files

| File | Use |
|------|-----|
| `box_top.3mf` / `box_bottom.3mf` | Ready-to-print halves — press-fit together |
| `box.step` | Neutral CAD for editing in any tool (FreeCAD, SolidWorks, Onshape…) |
| `casing.f3z` | Editable Fusion 360 source archive |

### `esp_program/` — firmware

| File | Use |
|------|-----|
| `main/main.ino` | Main firmware: lidar-guided move-to-height over WiFi |
| `main/desk_logic.h` | Pure height/direction/JSON logic, unit-tested on a computer |
| `tests/run_tests.sh` | Builds and runs those unit tests (`c++`, no hardware needed) |
| `lidar_xshut_test/lidar_xshut_test.ino` | Standalone lidar bring-up / XSHUT test |

## Building the firmware

1. Install the ESP32 board support (Seeed XIAO ESP32-C6) in the Arduino IDE.
2. Install the **VL53L1X** (Pololu) library via Library Manager.
3. Open `esp_program/main/main.ino` and set your WiFi credentials:

   ```cpp
   const char* ssid     = "XXX";   // <- your network
   const char* password = "XXX";   // <- your password
   ```

   > Keep your real credentials out of git — leave the placeholders in the
   > committed copy.

4. Flash to the board, open the serial monitor at 115200 baud to see the
   assigned IP, then move the desk with e.g. `http://<device-ip>/target?cm=90`.

To check the logic without the board, run `esp_program/tests/run_tests.sh`.

## Home Assistant

`homeassistant/packages/desk.yaml` adds the desk to Home Assistant over the
HTTP API above. You don't need MQTT or any add-ons. It gives you:

- **Desk height** (`sensor.desk_height`), updated after every action. The
  lidar is never polled, so the value is the last known height.
- **Desk target** slider (`input_number.desk_target`, 64–114 cm). Moving it
  moves the desk. Restarting Home Assistant does not.
- Scripts **Sit** (72 cm), **Stand** (111 cm), **+1 cm**, **−1 cm** and
  **Measure**. While the desk is moving, further presses are ignored.
- A notification (`Desk`) if the controller replies with an error or can't be
  reached.

Setup:

1. Reserve a fixed IP for the ESP in your router (DHCP reservation).
2. Copy `homeassistant/packages/desk.yaml` to `/config/packages/desk.yaml` on
   Home Assistant (e.g. with the File editor or Samba add-on), and replace
   `DESK_IP` in it with that IP.
3. Make sure `/config/configuration.yaml` loads packages:

   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```

4. Check the configuration (Developer tools → YAML → Check configuration) and
   restart Home Assistant.
5. Add a dashboard card (Edit dashboard → Add card → Manual):

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

To change the presets, edit the `cm:` values in `desk_sit` / `desk_stand`.

## Hardware

- Board / schematic / gerbers: open `kicad_files/skrivebord.kicad_pro` in
  KiCad 10. `kicad_files/gerbers/desktop_control_v1.zip` is the packaged
  fabrication output.
- Enclosure: print `box_top.3mf` and `box_bottom.3mf` and press-fit.

## License

Released under the [MIT License](LICENSE) — free to use, modify, and
distribute, keep the copyright notice. The bundled Seeed XIAO KiCad libraries
under `kicad_files/libraries/` remain the property of Seeed Studio.
