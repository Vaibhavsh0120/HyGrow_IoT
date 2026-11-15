#include "../core/state.h"
#include <Arduino.h>
#include <driver/rtc_io.h>
#include <esp_idf_version.h>
#if ESP_IDF_VERSION_MAJOR < 5
#include <driver/adc.h>
#endif

static float readTDS(uint8_t pin, float currentWaterTemp, float tds_k);

#define SCOUNT 30

// Median filter (DFRobot's algorithm)
int getMedianNum(const int bArray[], int iFilterLen)
{
    int bTab[SCOUNT] = {};
    int len = min(iFilterLen, SCOUNT);
    if (len <= 0)
        return 0;
    for (int i = 0; i < len; i++)
        bTab[i] = bArray[i];
    for (int j = 0; j < len - 1; j++)
    {
        for (int i = 0; i < len - j - 1; i++)
        {
            if (bTab[i] > bTab[i + 1])
            {
                int tmp = bTab[i];
                bTab[i] = bTab[i + 1];
                bTab[i + 1] = tmp;
            }
        }
    }
    return (len & 1)
               ? bTab[(len - 1) / 2]
               : (bTab[len / 2] + bTab[len / 2 - 1]) / 2;
}

void initTDS()
{
    pinMode(currentConfig.pin_tds, INPUT);
    analogReadResolution(12);
    webLog(1, LOG_INFO, "TDS sensor initialized on pin " + String(currentConfig.pin_tds));
}

void sensor_tds_init()
{
    initTDS();
}

static int readBiasedAdc(uint8_t pin)
{
#if ESP_IDF_VERSION_MAJOR < 5
    // Arduino's legacy read reconfigures the pad and clears the RTC pulls.
    const int channel = digitalPinToAnalogChannel(pin);
    if (channel < 0)
        return -1;
    if (channel < SOC_ADC_MAX_CHANNEL_NUM)
        return adc1_get_raw(static_cast<adc1_channel_t>(channel));
    int value = 0;
    const auto adc2Channel = static_cast<adc2_channel_t>(channel - SOC_ADC_MAX_CHANNEL_NUM);
    return adc2_get_raw(adc2Channel, ADC_WIDTH_BIT_12, &value) == ESP_OK ? value : -1;
#else
    return analogRead(pin);
#endif
}

static int readBiasMedian(uint8_t pin)
{
    delay(10);
    const int readings[] = {readBiasedAdc(pin), readBiasedAdc(pin), readBiasedAdc(pin)};
    return getMedianNum(readings, 3);
}

static bool tdsInputIsDriven(uint8_t pin)
{
    // Initialize ADC ownership before enabling any RTC pull resistor. Arduino
    // configures the pad on the first ADC read.
    (void)analogReadMilliVolts(pin);
    const auto gpio = static_cast<gpio_num_t>(pin);
    const bool pullDownReady = rtc_gpio_pullup_dis(gpio) == ESP_OK && rtc_gpio_pulldown_en(gpio) == ESP_OK;
    const int down = pullDownReady ? readBiasMedian(pin) : -1;
    const bool pullUpReady = rtc_gpio_pulldown_dis(gpio) == ESP_OK && rtc_gpio_pullup_en(gpio) == ESP_OK;
    const int up = pullUpReady ? readBiasMedian(pin) : -1;
    const bool neutral = rtc_gpio_pullup_dis(gpio) == ESP_OK;

    // An open lead follows both weak pulls toward the rails. A connected
    // output can legitimately read zero and should still hold the input low
    // under the weak pull-up. Always measure conductivity in neutral mode.
    constexpr int pullDownRail = 512;
    constexpr int pullUpRail = 3200;
    return down >= 0 && up >= 0 && neutral && !(down <= pullDownRail && up >= pullUpRail);
}

bool sensor_tds_read(float water_temp_c, float tds_k, float &tds_ppm)
{
    const uint8_t pin = currentConfig.pin_tds;
    if (!tdsInputIsDriven(pin))
    {
        tds_ppm = NAN;
        return false;
    }
    tds_ppm = readTDS(pin, water_temp_c, tds_k);
    return isfinite(tds_ppm);
}

static float readTDS(uint8_t pin, float currentWaterTemp, float tds_k)
{
    // 1. Rapidly sample 30 times into a local buffer. Taking samples 2ms
    // apart ensures we get a good average without blocking FreeRTOS too
    // long. sensor_enabled[S_TDS] is what decides whether this ever gets
    // called in practice — see validateSensor()/readAll() in task_sensor.cpp.
    int analogBuffer[SCOUNT];
    for (int i = 0; i < SCOUNT; i++)
    {
        analogBuffer[i] = analogReadMilliVolts(pin);
        delay(2);
    }

    // 2. Apply Median Filter
    int medianRaw = getMedianNum(analogBuffer, SCOUNT);
    float avgVoltage = medianRaw / 1000.0;

    // 3. Temperature Compensation
    if (currentWaterTemp <= 0.0)
    {
        currentWaterTemp = 25.0; // Fallback if water temp sensor fails
    }
    float compCoeff = 1.0 + 0.02 * (currentWaterTemp - 25.0);
    float compVoltage = avgVoltage / compCoeff;

    // 4. Calculate final TDS using polynomial and NVS calibration K-Factor
    float tdsValue = (133.42 * pow(compVoltage, 3) - 255.86 * pow(compVoltage, 2) + 857.39 * compVoltage) * 0.5 * tds_k;

    // 5. Sanity bounds check
    if (tdsValue < 0.0)
    {
        tdsValue = 0.0;
    }

    return tdsValue;
}
