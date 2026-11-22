#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <VL53L1X.h>

#include "desk_logic.h"

VL53L1X sensor;

// WiFi Credentials
const char* ssid = "XXX";
const char* password = "XXX";

WebServer server(80);

// Pin Definitions for Seeed XIAO ESP32-C6
const int PIN_UP = D7;
const int PIN_DOWN = D6;
const int XSHUT = D0;

const unsigned long MAX_MOVE_MS = 20000; // hard safety cutoff

// When the last /target or /nudge finished; see isStaleRequest().
uint32_t lastMoveEndMs = 0;
bool hasMoved = false;

// WiFi watchdog state; see wifiAction() in desk_logic.h.
const unsigned long WIFI_BOOT_WAIT_MS = 30000;
uint32_t wifiLastConnectedMs = 0;
uint32_t wifiLastAttemptMs = 0;
bool wifiWasConnected = false;


void initWiFi() {
  pinMode(WIFI_ENABLE, OUTPUT); // pinMode(3, OUTPUT);
  digitalWrite(WIFI_ENABLE, LOW); // digitalWrite(3, LOW); // Activate RF switch control

  delay(100);

  pinMode(WIFI_ANT_CONFIG, OUTPUT); // pinMode(14, OUTPUT);
  digitalWrite(WIFI_ANT_CONFIG, LOW); // digitalWrite(14, LOW); // Use internal antenna


  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(ssid,password);
  Serial.println("Connecting to WiFi");
  // Don't wait forever: if the router isn't up yet, loop() keeps retrying.
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_BOOT_WAIT_MS) {
    Serial.print(".");
    delay(1000);
  }
  wifiLastConnectedMs = millis();
  wifiLastAttemptMs = millis();

  if (WiFi.status() == WL_CONNECTED) {
    wifiWasConnected = true;
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    delay(5000);
  } else {
    Serial.println("\nWiFi not connected yet - will keep retrying");
  }
}

// Called from loop(): reconnects after drops the ESP32 core won't retry on
// its own, and as a last resort restarts the board (like unplugging it).
void ensureWiFi() {
  uint32_t now = millis();
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) {
    wifiLastConnectedMs = now;
    if (!wifiWasConnected) {
      Serial.print("WiFi reconnected, IP address: ");
      Serial.println(WiFi.localIP());
    }
    wifiWasConnected = true;
    return;
  }
  wifiWasConnected = false;

  switch (wifiAction(connected, now, wifiLastConnectedMs, wifiLastAttemptMs)) {
    case WifiAction::Retry:
      Serial.println("WiFi down - retrying");
      wifiLastAttemptMs = now;
      WiFi.disconnect();
      WiFi.begin(ssid, password);
      break;
    case WifiAction::Restart:
      Serial.println("WiFi down for 5 minutes - restarting");
      delay(100);
      ESP.restart();
      break;
    default:
      break;
  }
}

bool lidarOn() {
  digitalWrite(XSHUT, HIGH);
  delay(10);
    if (!sensor.init()) {
    Serial.println("Kunne ikke finde VL53L1X sensor!");
    digitalWrite(XSHUT, LOW);
    return false;
  }
  sensor.setTimeout(500);
  sensor.setDistanceMode(VL53L1X::Short);
  sensor.setMeasurementTimingBudget(33000); // microseconds, not ms — this is 33 ms
  sensor.startContinuous(50);                // continuous mode, 50 ms period
  return true;
}

void lidarOff() {
  sensor.stopContinuous();
  digitalWrite(XSHUT, LOW);
}


float readDistanceCm() {
  int d = sensor.read();
  if (sensor.timeoutOccurred() || d <= 0) return -1; // invalid
  return d / 10.0;
}

// Drives the desk to targetCm. The lidar must already be on; the caller
// turns it off. The motor pin is LOW again on every return path.
MoveResult moveTo(float targetCm) {
  float cur_dist = readDistanceCm();
  if (cur_dist < 0) {
    Serial.println("Fail with initial measurement - abort");
    return {false, 0, "sensor unavailable"};
  }

  Direction dir = chooseDirection(cur_dist, targetCm);
  Serial.printf("[DBG] move start: current %.1f cm, target %.1f cm, dir %s\n", cur_dist, targetCm,
                dir == Direction::Up ? "UP" : dir == Direction::Down ? "DOWN" : "none");
  if (dir == Direction::None) {
    return {true, cur_dist, nullptr};  // already there (dead band)
  }

  bool goingDown = dir == Direction::Down;
  int activePin = goingDown ? PIN_DOWN : PIN_UP;
  int inactivePin = goingDown ? PIN_UP : PIN_DOWN;

  // Prevent enabling both directions at the same time
  digitalWrite(inactivePin, LOW);
  delay(10);
  digitalWrite(activePin, HIGH);

  const char* error = nullptr;
  unsigned long startTime = millis();
  uint32_t lastGoodReadingMs = startTime;
  int failCount = 0;
  while (true) {
    if (millis() - startTime > MAX_MOVE_MS) {
      Serial.println("Safety timeout - Stopping");
      error = "safety timeout";
      break;
    }

    float d = readDistanceCm();
    if (readingGapTooLong(millis(), lastGoodReadingMs)) {
      Serial.println("No good reading for over 1 s - stopping");
      error = "readings too slow";
      break;
    }
    if (d <= 0) {
      if (++failCount > 10) {
        Serial.println("Sensor lost - aborting");
        error = "sensor lost";
        break;
      }
      Serial.println("Measurement Failure");
      delay(50);
      continue;
    }
    failCount = 0;
    lastGoodReadingMs = millis();

    cur_dist = d;
    Serial.print("Afstand: ");
    Serial.print(cur_dist);
    Serial.println(" cm");

    if (reachedTarget(dir, cur_dist, targetCm)) break;

    delay(50);
  }

  digitalWrite(activePin, LOW); // always stop, regardless of why the loop ended
  Serial.printf("[DBG] move stop: %s at %.1f cm after %lu ms\n",
                error ? error : "reached target", cur_dist, millis() - startTime);

  // The desk coasts a little after the motor stops; report where it settled.
  delay(300);
  float settled = readDistanceCm();
  if (settled > 0) cur_dist = settled;

  if (error) return {false, cur_dist, error};
  return {true, cur_dist, nullptr};
}

void sendResult(const MoveResult& r) {
  char body[64];
  if (r.ok) {
    formatHeightJson(body, sizeof body, r.heightCm);
    server.send(200, "application/json", body);
  } else {
    formatErrorJson(body, sizeof body, r.error);
    server.send(503, "application/json", body);
  }
}

void sendBadRequest() {
  char body[64];
  formatErrorJson(body, sizeof body, "missing or invalid cm");
  server.send(400, "application/json", body);
}

// A move request that queued up while the desk was moving: refuse it, so a
// second client can't start a move the moment the first one ends.
bool rejectIfStale() {
  if (!isStaleRequest(millis(), lastMoveEndMs, hasMoved)) return false;
  Serial.println("[DBG] rejected as stale (409 busy)");
  char body[64];
  formatErrorJson(body, sizeof body, "busy");
  server.send(409, "application/json", body);
  return true;
}

void markMoveEnded() {
  lastMoveEndMs = millis();
  hasMoved = true;
}

// GET /target?cm=90 -> moves to 90 cm (clamped), replies {"height":90.1}
void handleTarget() {
  Serial.printf("[DBG] HTTP %s cm=%s from %s at %lu ms\n", server.uri().c_str(),
                server.arg("cm").c_str(), server.client().remoteIP().toString().c_str(), millis());
  float cm;
  if (!parseCm(server.arg("cm").c_str(), &cm)) {
    sendBadRequest();
    return;
  }
  if (rejectIfStale()) return;
  if (!lidarOn()) {
    sendResult({false, 0, "sensor unavailable"});
    return;
  }
  MoveResult r = moveTo(clampHeight(cm));
  lidarOff();
  sendResult(r);
  markMoveEnded();
}

// GET /nudge?cm=-1 -> moves 1 cm down from the current height (clamped,
// never against the requested direction)
void handleNudge() {
  Serial.printf("[DBG] HTTP %s cm=%s from %s at %lu ms\n", server.uri().c_str(),
                server.arg("cm").c_str(), server.client().remoteIP().toString().c_str(), millis());
  float delta;
  if (!parseCm(server.arg("cm").c_str(), &delta)) {
    sendBadRequest();
    return;
  }
  if (rejectIfStale()) return;
  if (!lidarOn()) {
    sendResult({false, 0, "sensor unavailable"});
    return;
  }
  float cur = readDistanceCm();
  MoveResult r = cur < 0 ? MoveResult{false, 0, "sensor unavailable"}
                         : moveTo(nudgeTarget(cur, delta));
  lidarOff();
  sendResult(r);
  markMoveEnded();
}

// GET /height -> one reading, lidar off again, replies {"height":92.4}
void handleHeight() {
  Serial.printf("[DBG] HTTP %s cm=%s from %s at %lu ms\n", server.uri().c_str(),
                server.arg("cm").c_str(), server.client().remoteIP().toString().c_str(), millis());
  if (!lidarOn()) {
    sendResult({false, 0, "sensor unavailable"});
    return;
  }
  float cur = readDistanceCm();
  lidarOff();
  if (cur < 0) {
    sendResult({false, 0, "sensor unavailable"});
    return;
  }
  sendResult({true, cur, nullptr});
}




void setup() {
    // Configure Pins
  pinMode(PIN_UP, OUTPUT);
  pinMode(PIN_DOWN, OUTPUT);
  digitalWrite(PIN_UP, LOW);
  digitalWrite(PIN_DOWN, LOW);
  //deactivate LiDAR
  pinMode(XSHUT, OUTPUT);
  digitalWrite(XSHUT, LOW);

  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Serial goes over USB. With the cable plugged in but no serial monitor
  // open, each print would otherwise block for up to ~2 s and stall the move
  // loop, so the desk overshoots. Never wait: drop log output instead.
  Serial.setTxTimeoutMs(0);
#endif
  initWiFi();
  Wire.begin();



  if (!lidarOn()) {
    Serial.println("Kunne ikke finde VL53L1X sensor!");
    while (1);
  }
  lidarOff();


  server.on("/target", handleTarget);
  server.on("/nudge", handleNudge);
  server.on("/height", handleHeight);
  server.begin();
}

void loop() {
  ensureWiFi();
  server.handleClient();
}
