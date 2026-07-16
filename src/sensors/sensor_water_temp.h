#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_NeoPixel.h>

#define ONE_WIRE_BUS 4
#define RGB_PIN 48
#define NUMPIXELS 1

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

Adafruit_NeoPixel pixel(NUMPIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);

void setup() {
  Serial.begin(115200);

  sensors.begin();

  pixel.begin();
  pixel.clear();
  pixel.show();

  Serial.println("DS18B20 Temperature Monitor Started");
}

void loop() {
  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);

  if (tempC == DEVICE_DISCONNECTED_C) {
    Serial.println("Sensor not found!");
    pixel.setPixelColor(0, pixel.Color(255, 0, 255)); // Purple = Error
    pixel.show();
    delay(1000);
    return;
  }

  String status;

  if (tempC < 20) {
    status = "COLD";
    pixel.setPixelColor(0, pixel.Color(0, 0, 255));
  }
  else if (tempC <= 35) {
    status = "NORMAL";
    pixel.setPixelColor(0, pixel.Color(0, 255, 0));
  }
  else {
    status = "HOT";
    pixel.setPixelColor(0, pixel.Color(255, 0, 0));
  }

  pixel.show();

  Serial.print("Temperature: ");
  Serial.print(tempC, 2);
  Serial.print(" °C | ");
  Serial.println(status);

  delay(1000);
}