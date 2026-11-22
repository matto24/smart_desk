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
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(1000);
  }

  Serial.println("\nWiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  delay(5000);
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
  int failCount = 0;
  while (true) {
    if (millis() - startTime > MAX_MOVE_MS) {
      Serial.println("Safety timeout - Stopping");
      error = "safety timeout";
      break;
    }

    float d = readDistanceCm();
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

    cur_dist = d;
    Serial.print("Afstand: ");
    Serial.print(cur_dist);
    Serial.println(" cm");

    if (reachedTarget(dir, cur_dist, targetCm)) break;

    delay(50);
  }

  digitalWrite(activePin, LOW); // always stop, regardless of why the loop ended

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

// GET /target?cm=90 -> moves to 90 cm (clamped), replies {"height":90.1}
void handleTarget() {
  float cm;
  if (!parseCm(server.arg("cm").c_str(), &cm)) {
    sendBadRequest();
    return;
  }
  if (!lidarOn()) {
    sendResult({false, 0, "sensor unavailable"});
    return;
  }
  MoveResult r = moveTo(clampHeight(cm));
  lidarOff();
  sendResult(r);
}

// GET /nudge?cm=-1 -> moves 1 cm down from the current height (clamped)
void handleNudge() {
  float delta;
  if (!parseCm(server.arg("cm").c_str(), &delta)) {
    sendBadRequest();
    return;
  }
  if (!lidarOn()) {
    sendResult({false, 0, "sensor unavailable"});
    return;
  }
  float cur = readDistanceCm();
  MoveResult r = cur < 0 ? MoveResult{false, 0, "sensor unavailable"}
                         : moveTo(clampHeight(cur + delta));
  lidarOff();
  sendResult(r);
}

// GET /height -> one reading, lidar off again, replies {"height":92.4}
void handleHeight() {
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
  server.handleClient();
}
