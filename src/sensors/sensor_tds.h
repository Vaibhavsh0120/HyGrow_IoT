#include <Adafruit_NeoPixel.h>

#define TDS_PIN      4
#define RGB_PIN      48
#define NUMPIXELS    1
#define SCOUNT       30       // median filter sample count
#define VREF         3.3f
#define ADC_RES      4095.0f

Adafruit_NeoPixel pixel(NUMPIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);

int analogBuffer[SCOUNT];
int analogBufferIndex = 0;
float temperature = 25.0;  // Replace with DHT22 reading in AgriNova

// Median filter (DFRobot's algorithm)
int getMedianNum(int bArray[], int iFilterLen) {
  int bTab[iFilterLen];
  for (int i = 0; i < iFilterLen; i++) bTab[i] = bArray[i];
  for (int j = 0; j < iFilterLen - 1; j++) {
    for (int i = 0; i < iFilterLen - j - 1; i++) {
      if (bTab[i] > bTab[i + 1]) {
        int tmp = bTab[i];
        bTab[i] = bTab[i + 1];
        bTab[i + 1] = tmp;
      }
    }
  }
  return (iFilterLen & 1)
    ? bTab[(iFilterLen - 1) / 2]
    : (bTab[iFilterLen / 2] + bTab[iFilterLen / 2 - 1]) / 2;
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);

  pixel.begin();
  pixel.clear();
  pixel.show();

  Serial.println("DFRobot TDS Sensor Ready (ESP32-S3)");
}

void loop() {
  static unsigned long sampleTimer = millis();
  static unsigned long printTimer  = millis();

  // Sample every 40ms into ring buffer
  if (millis() - sampleTimer > 40U) {
    sampleTimer = millis();
    analogBuffer[analogBufferIndex++] = analogRead(TDS_PIN);
    if (analogBufferIndex == SCOUNT) analogBufferIndex = 0;
  }

  // Print every 800ms
  if (millis() - printTimer > 800U) {
    printTimer = millis();

    // Median → voltage → temperature compensation → TDS
    float medianRaw      = getMedianNum(analogBuffer, SCOUNT);
    float avgVoltage     = medianRaw * (VREF / ADC_RES);
    float compCoeff      = 1.0 + 0.02 * (temperature - 25.0);
    float compVoltage    = avgVoltage / compCoeff;
    float tdsValue       = (133.42 * pow(compVoltage, 3)
                          - 255.86 * pow(compVoltage, 2)
                          + 857.39 * compVoltage) * 0.5;

    // LED by PPM (hydroponic ranges)
    String status;
    if (avgVoltage < 0.1) {
      status = "NO WATER / ERROR";
      pixel.setPixelColor(0, pixel.Color(255, 0, 255)); // Purple
    } else if (tdsValue < 200) {
      status = "TOO LOW";
      pixel.setPixelColor(0, pixel.Color(0, 0, 255));   // Blue
    } else if (tdsValue < 1500) {
      status = "NORMAL (hydroponic)";
      pixel.setPixelColor(0, pixel.Color(0, 255, 0));   // Green
    } else {
      status = "TOO HIGH";
      pixel.setPixelColor(0, pixel.Color(255, 0, 0));   // Red
    }
    pixel.show();

    Serial.println("=================================");
    Serial.print("Raw Median ADC : "); Serial.println((int)medianRaw);
    Serial.print("Voltage        : "); Serial.print(avgVoltage, 3); Serial.println(" V");
    Serial.print("Comp. Voltage  : "); Serial.print(compVoltage, 3); Serial.println(" V");
    Serial.print("TDS            : "); Serial.print(tdsValue, 0); Serial.println(" ppm");
    Serial.print("Status         : "); Serial.println(status);
  }
}