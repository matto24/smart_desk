#include <Wire.h>
#include <VL53L1X.h>

VL53L1X sensor;

// Pin Definitions for Seeed XIAO ESP32-C6
const int XSHUT = D0;





float readDistanceCm() {
  int d = sensor.read();
  if (sensor.timeoutOccurred() || d <= 0) return -1; // invalid
  return d / 10.0;
}


void setup() {
  Serial.begin(115200);
  Wire.begin();

  if (!sensor.init()) {
    Serial.println("Kunne ikke finde VL53L1X sensor!");
    while (1);
  }


  pinMode(XSHUT, OUTPUT);
  digitalWrite(XSHUT, LOW);

}



void loop() {
  delay(100);
  lidarInit();
  float dist = readDistanceCm();
  Serial.println(dist);
  digitalWrite(XSHUT, LOW);
  delay(10);
  dist = readDistanceCm();
  Serial.println(dist);
  delay(1000);
}