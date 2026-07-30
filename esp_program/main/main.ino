#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <VL53L1X.h>

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

void move_to_target(int target) {
  if (!lidarOn()) return;
  float cur_dist = readDistanceCm();
  if (cur_dist < 0) {
    Serial.println("Fail with initial measurement - abort");
    lidarOff();
    return;
  }

  bool goingDown = cur_dist > target;
  int activePin = goingDown ? PIN_DOWN : PIN_UP;
  int inactivePin = goingDown ? PIN_UP : PIN_DOWN;

  // Prevent enabling both directions at the same time
  digitalWrite(inactivePin, LOW);
  delay(10);
  digitalWrite(activePin, HIGH);

  unsigned long startTime = millis();
  int failCount = 0;
  while (true) {
    if (millis() - startTime > MAX_MOVE_MS) {
      Serial.println("Safety timeout - Stopping");
      break;
    }

    float d = readDistanceCm();
    if (d <= 0) {
      if (++failCount > 10) {
        Serial.println("Sensor lost - aborting");
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

    if (goingDown && cur_dist <= target) break;
    if (!goingDown && cur_dist >= target) break;

    delay(50);
  }

  digitalWrite(activePin, LOW); // always stop, regardless of why the loop ended
  lidarOff();

}


// Handler for HTTP GET /target?cm=200
void toTarget() {
  int cm = 70; // default
  if (server.hasArg("cm")) {
    cm = server.arg("cm").toInt();
  }
  cm = min(cm, 114);
  cm = max(64, cm);

  server.send(200, "text/plain", "Moving to target");
  move_to_target(cm);
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


  server.on("/target", toTarget);
  server.begin();
}

void loop() {
  server.handleClient();
}
