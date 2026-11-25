#pragma once

#include <Arduino.h>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <math.h>
#include <time.h>
#include "weather_data.h"

namespace weather_forecast {
inline double number(const cJSON* section, const char* name, int index = -1) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(section, name);
    if (index >= 0) item = cJSON_GetArrayItem(item, index);
    return cJSON_IsNumber(item) ? item->valuedouble : NAN;
}

inline bool localTimeAt(const cJSON* section, int index, int offset, tm& local) {
    const double timestamp = number(section, "time", index);
    if (!isfinite(timestamp) || timestamp < 0) return false;
    const time_t adjusted = static_cast<time_t>(timestamp) + offset;
    return gmtime_r(&adjusted, &local) != nullptr;
}

inline bool parse(const cJSON* root, WeatherData& out) {
    const cJSON* current = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON* hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    const cJSON* daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    const double offset = number(root, "utc_offset_seconds");
    const double temp = number(current, "temperature_2m");
    const double wmo = number(current, "weather_code");
    const double humidity = number(current, "relative_humidity_2m");
    if (offset != 8 * 3600 || !isfinite(temp) || !(wmo >= 0 && wmo <= 99)
        || !(humidity >= 0 && humidity <= 100)) return false;

    WeatherData next{};
    next.temperature = temp;
    next.wmo = static_cast<int>(wmo);
    next.humidity = static_cast<uint8_t>(humidity);
    for (int i = 0; i < 6; ++i) {
        tm local{};
        const double celsius = number(hourly, "temperature_2m", i);
        const double rain = number(hourly, "precipitation_probability", i);
        if (!localTimeAt(hourly, i, offset, local) || !isfinite(celsius)
            || !(rain >= 0 && rain <= 100)) return false;
        next.hourly[i] = {static_cast<uint8_t>(local.tm_hour), static_cast<float>(celsius),
                          static_cast<uint8_t>(rain)};
    }
    for (int i = 0; i < 5; ++i) {
        tm local{};
        const double code = number(daily, "weather_code", i);
        const double low = number(daily, "temperature_2m_min", i);
        const double high = number(daily, "temperature_2m_max", i);
        if (!localTimeAt(daily, i, offset, local) || !(code >= 0 && code <= 99)
            || !isfinite(low) || !isfinite(high) || low > high) return false;
        next.daily[i] = {static_cast<uint8_t>(local.tm_wday), static_cast<int>(code),
                         static_cast<float>(low), static_cast<float>(high)};
    }
    out = next;
    return true;
}
}  // namespace weather_forecast

inline bool fetchChengduWeather(WeatherData& out) {
    esp_http_client_config_t config = {};
    config.url = "https://api.open-meteo.com/v1/forecast?latitude=30.67&longitude=104.07"
                 "&timezone=Asia%2FShanghai&timeformat=unixtime"
                 "&current=temperature_2m,relative_humidity_2m,weather_code"
                 "&hourly=temperature_2m,precipitation_probability"
                 "&daily=weather_code,temperature_2m_min,temperature_2m_max"
                 "&forecast_hours=6&forecast_days=5";
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 8000;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        Serial.println("Weather: HTTPS client unavailable");
        return false;
    }

    char body[2048];
    int bytes = 0;
    int status = 0;
    esp_err_t error = esp_http_client_open(client, 0);
    if (error == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        status = esp_http_client_get_status_code(client);
        if (status == 200) {
            bytes = esp_http_client_read_response(client, body, sizeof(body) - 1);
        }
    }
    esp_http_client_cleanup(client);
    if (bytes <= 0 || bytes >= static_cast<int>(sizeof(body) - 1)) {
        Serial.printf("Weather: request failed (HTTP %d, %s)\n", status, esp_err_to_name(error));
        return false;
    }

    body[bytes] = '\0';
    cJSON* json = cJSON_Parse(body);
    const bool valid = json && weather_forecast::parse(json, out);
    cJSON_Delete(json);
    if (!valid) Serial.println("Weather: missing or invalid forecast");
    return valid;
}
