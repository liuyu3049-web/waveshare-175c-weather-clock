#pragma once

#include <stdint.h>

struct WeatherHour {
    uint8_t hour;
    float temperature;
    uint8_t rainChance;
};

struct WeatherDay {
    uint8_t weekday;  // Sunday = 0
    int wmo;
    float low;
    float high;
};

struct WeatherData {
    float temperature;
    int wmo;
    uint8_t humidity;
    WeatherHour hourly[6];
    WeatherDay daily[5];
};
