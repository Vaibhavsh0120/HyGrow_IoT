#include <DHT.h>
#include <Adafruit_NeoPixel.h>

#define DHTPIN 4          // DHT22 Data Pin
#define DHTTYPE DHT22

#define RGB_PIN 48
#define NUMPIXELS 1

DHT dht(DHTPIN, DHTTYPE);
Adafruit_NeoPixel pixel(NUMPIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);

void setup() {
  Serial.begin(115200);

  dht.begin();

  pixel.begin();
  pixel.clear();
  pixel.show();

  Serial.println("DHT22 Temperature & Humidity Monitor Started");
}

void loop() {
  float temperature = dht.readTemperature(); // Celsius
  float humidity = dht.readHumidity();

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("Failed to read from DHT22!");

    // Purple = Error
    pixel.setPixelColor(0, pixel.Color(255, 0, 255));
    pixel.show();

    delay(2000);
    return;
  }

  String status;

  if (temperature < 20) {
    status = "COLD";
    pixel.setPixelColor(0, pixel.Color(0, 0, 255));   // Blue
  }
  else if (temperature <= 35) {
    status = "NORMAL";
    pixel.setPixelColor(0, pixel.Color(0, 255, 0));   // Green
  }
  else {
    status = "HOT";
    pixel.setPixelColor(0, pixel.Color(255, 0, 0));   // Red
  }

  pixel.show();

  Serial.print("Temperature: ");
  Serial.print(temperature, 2);
  Serial.print(" °C | Humidity: ");
  Serial.print(humidity, 2);
  Serial.print(" % | ");
  Serial.println(status);

  delay(2000);   // DHT22 should be read about once every 2 seconds
}