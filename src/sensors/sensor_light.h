#include <BH1750.h>
#include <Wire.h>

BH1750 lightMeter;

void setup() {
  Serial.begin(115200);

  // SDA = GPIO 8, SCL = GPIO 9
  Wire.begin(8, 9);

  lightMeter.begin();

  Serial.println("BH1750 Test begin");
}

void loop() {
  float lux = lightMeter.readLightLevel();

  Serial.print("Light: ");
  Serial.print(lux);
  Serial.println(" lx");

  delay(1000);
}