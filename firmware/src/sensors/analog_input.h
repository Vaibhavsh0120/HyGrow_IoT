#pragma once

#include <Arduino.h>
#include <algorithm>
#include <driver/rtc_io.h>
#include <esp_idf_version.h>
#if ESP_IDF_VERSION_MAJOR < 5
#include <driver/adc.h>
#endif

namespace analog_input
{
inline int readBiasedAdc(uint8_t pin)
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

inline int readBiasMedian(uint8_t pin)
{
    delay(10);
    const int readings[] = {readBiasedAdc(pin), readBiasedAdc(pin), readBiasedAdc(pin)};
    const int lower = std::min(readings[0], readings[1]);
    const int upper = std::max(readings[0], readings[1]);
    return std::max(lower, std::min(upper, readings[2]));
}

inline bool isDriven(uint8_t pin)
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
    // under the weak pull-up. Take the sensor reading in neutral mode.
    constexpr int pullDownRail = 512;
    constexpr int pullUpRail = 3200;
    return down >= 0 && up >= 0 && neutral && !(down <= pullDownRail && up >= pullUpRail);
}
} // namespace analog_input
