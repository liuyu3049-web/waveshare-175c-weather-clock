#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Preferences.h>
#include <sys/time.h>
#include <time.h>
#include "clock_face.h"
#include "clock_touch.h"
#include "clock_power.h"
#include "clock_sleep_key.h"
#include "clock_holdover.h"
#include <esp_sntp.h>
#include <esp_wifi.h>
#include "weather_fetch.h"
#include "clock_networks.h"
#include "clock_roaming.h"
#include "wifi_setup.h"
#include <atomic>

struct WeatherReading {
  WeatherData data;
  time_t updated;
};

static QueueHandle_t latestReading;
static SemaphoreHandle_t serialFrameLock;
static TaskHandle_t weatherNetworkTask;
static std::atomic<unsigned> savedNetworkCount{0};
static constexpr uint32_t refreshRequest = 1, setupRequest = 2;
static constexpr unsigned long weatherInterval = 15UL * 60 * 1000;
static RTC_DATA_ATTR WeatherReading weather{};
static RTC_DATA_ATTR uint8_t face = ClockMountainDigital;
static RTC_DATA_ATTR int page;
static RTC_DATA_ATTR bool checkPmuAfterSleep;
static constexpr uint64_t keyCheckUs = 220000;
static bool frameReady, touchReady, powerReady, shotRequested, repaint, shutdownPending;
static ClockScreen screen = ClockScreen::On;
static uint32_t screenOffAt;
static uint32_t lastInteractionAt;
static bool settingsOpen;
static Preferences displayPrefs;
static ClockDisplayOptions clockDisplayOptions;
static ClockBattery battery;
static constexpr uint32_t sweepPeriodUs = 1000000 / 30;
static uint32_t sweepFrames, lastSweepCommit, longestSweepGap;
static constexpr char clockTimeZone[] = "CST-8";
static const char* const weatherStatus[] = {"等待联网", "正在校时", "正在更新天气", ""};

static bool readClockLocalTime(timeval& wall, tm& local) {
  gettimeofday(&wall, nullptr);
  const time_t epoch = wall.tv_sec;
  return epoch >= 1700000000 && localtime_r(&epoch, &local);
}

static int clockWeatherState(bool online, bool synced) {
  return !online ? 0 : !synced ? 1 : !weather.updated ? 2 : 3;
}

static void requestClockWeatherRefresh() {
  if (weatherNetworkTask) xTaskNotify(weatherNetworkTask, refreshRequest, eSetBits);
}

static void openClockWifiSetup() {
  if (weatherNetworkTask) xTaskNotify(weatherNetworkTask, setupRequest, eSetBits);
}

static void applySettingsTap(int action) {
  using namespace clock_settings;
  if (action == Back) settingsOpen = false;
  else if (action == Wifi) openClockWifiSetup();
  else if (action >= Timeout && action < Timeout+timeCount) {
    const uint8_t index = action-Timeout;
    if (index == clockDisplayOptions.timeout) return;
    clockDisplayOptions.timeout = index;
    displayPrefs.putUChar("timeout",index);
  } else if (action >= Level && action < Timeout) {
    const uint8_t index = action-Level;
    if (index == clockDisplayOptions.level) return;
    clockDisplayOptions.level = index;
    if (frameReady) clock_face::panel.setBrightness(clockDisplayOptions.brightness());
    displayPrefs.putUChar("level",index);
  } else return;
  repaint = true;
}

static void networkWorker(void*) {
  clock_networks::List profiles{};
  clock_roaming::Roamer roaming;
  bool legacy = false;
  const bool loaded = clock_networks::load(profiles, &legacy);
  clock_roaming::importNativeIfLegacy(profiles, legacy || !loaded);
  savedNetworkCount = profiles.count;
  WiFi.persistent(false);
  while (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
    WiFi.mode(WIFI_STA);
    delay(250);
  }
  bool clockStarted = false;
  uint32_t nextFetch = millis(), closedSetupAt = 0;
  bool autoSetupAllowed = true;
  TickType_t waitTicks = 0;
  for (;;) {
    uint32_t requests = 0;
    xTaskNotifyWait(0, UINT32_MAX, &requests, waitTicks);
    const bool exhausted = roaming.tick(profiles);
    if (!autoSetupAllowed && millis() - closedSetupAt >= 180000) autoSetupAllowed = true;
    if ((requests & setupRequest) || (exhausted && !profiles.count && autoSetupAllowed)) {
      if (!profiles.writable) clock_networks::load(profiles);
      xSemaphoreTake(serialFrameLock, portMAX_DELAY);
      runClockWifiSetup(profiles, roaming, (requests & setupRequest) != 0);
      xSemaphoreGive(serialFrameLock);
      savedNetworkCount = profiles.count;
      closedSetupAt = millis();
      autoSetupAllowed = false;
      clockStarted = false;
    }
    if (WiFi.status() != WL_CONNECTED) {
      clockStarted = false;
      waitTicks = pdMS_TO_TICKS(75);
      continue;
    }
    if (!clockStarted) {
      xSemaphoreTake(serialFrameLock, portMAX_DELAY);
      Serial.printf("CLOCK_WIFI_CONNECTED rssi=%d\n", WiFi.RSSI());
      xSemaphoreGive(serialFrameLock);
      configTzTime(clockTimeZone, "ntp.aliyun.com", "ntp.tencent.com", "pool.ntp.org");
      nextFetch = millis();
      clockStarted = true;
    }
    waitTicks = pdMS_TO_TICKS(500);
    if (requests & refreshRequest) nextFetch = millis();
    if (time(nullptr) < 1700000000 || (int32_t)(millis() - nextFetch) < 0) continue;
    WeatherReading reading{};
    // Keep diagnostics produced by the HTTPS client outside binary frame transfers.
    xSemaphoreTake(serialFrameLock, portMAX_DELAY);
    const bool ok = fetchChengduWeather(reading.data);
    if (ok) {
      reading.updated = time(nullptr);
      xQueueOverwrite(latestReading, &reading);
      Serial.printf("CLOCK_WEATHER_OK city=Chengdu temperature=%.1f wmo=%d epoch=%lld\n",
                    reading.data.temperature, reading.data.wmo, (long long)reading.updated);
    } else {
      Serial.println("CLOCK_WEATHER_RETRY");
    }
    xSemaphoreGive(serialFrameLock);
    nextFetch = millis() + (ok ? weatherInterval : 60000);
  }
}

static bool clockPmuWakeEvent(int flags) {
  return flags > 0 && (flags & (clockPress | clockLong | clockShort));
}

static void resumeClockKey() {
  const auto cause = esp_sleep_get_wakeup_cause();
  const bool checkPmu = checkPmuAfterSleep && (cause == ESP_SLEEP_WAKEUP_TIMER || cause == ESP_SLEEP_WAKEUP_ULP);
  checkPmuAfterSleep = false;
  int flags = powerReady ? takeClockPmuKeyFlags() : -1;
  if (!checkPmu) {
    if (cause == ESP_SLEEP_WAKEUP_EXT0) seedClockWakeKey(flags < 0 ? 0 : flags);
    return;  // On cold boot, the read above only clears stale latches.
  }

  if (clockPmuWakeEvent(flags)) {
    seedClockWakeKey(flags);
    return;
  }
  // Recovery only: the ULP probe or a subsequent PMIC transaction failed.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  const bool timerReady = esp_sleep_enable_timer_wakeup(keyCheckUs) == ESP_OK;
  while (!clockPmuWakeEvent(flags)) {
    // Normally only the timer runs here. A stuck-high GPIO must never spin the CPU.
    const bool usePin = flags < 0 && digitalRead(clockPowerPin) == LOW &&
                        esp_sleep_enable_ext0_wakeup(clockPowerPin, 1) == ESP_OK;
    const esp_err_t result = timerReady ? esp_light_sleep_start() : ESP_FAIL;
    const bool pinWoke = result == ESP_OK && usePin && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
    if (usePin) {
      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
      rtc_gpio_deinit(clockPowerPin);
      pinMode(clockPowerPin, INPUT);
    }
    if (result != ESP_OK) delay(keyCheckUs / 1000);
    if (flags < 0) powerReady = beginClockPmu();
    flags = powerReady ? takeClockPmuKeyFlags() : -1;
    if (pinWoke && flags < 0) break;  // No working PMIC, but a new low-to-high edge was seen.
  }
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  seedClockWakeKey(flags < 0 ? 0 : flags);
}

static void drawClockWakeHome() {
  timeval wall;
  tm local{};
  const bool synced = readClockLocalTime(wall, local);
  const bool online = WiFi.status() == WL_CONNECTED;
  const bool stale = weather.updated && (!online || wall.tv_sec - weather.updated > 30 * 60);
  drawWeatherPage(0, face, synced ? &local : nullptr, weather.updated ? &weather.data : nullptr,
                  weatherStatus[clockWeatherState(online, synced)], stale, false, &battery,
                  local.tm_sec + wall.tv_usec * .000001f);
  if (synced) Serial.printf("CLOCK_HOME_READY local=%04d-%02d-%02d %02d:%02d:%02d wifi=%d\n",
                           local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                           local.tm_hour, local.tm_min, local.tm_sec, online);
}

void setup() {
  clock_holdover::restore();
  sntp_set_time_sync_notification_cb(clock_holdover::onNetworkTime);
  // The ESP RTC retains UTC during sleep, but the process timezone resets on deep wake.
  setenv("TZ", clockTimeZone, 1);
  tzset();
  beginClockKey();
  if (clock_sleep_key::active) clock_sleep_key::releaseBus();
  Wire.begin(15, 14);
  powerReady = beginClockPmu();
  resumeClockKey();
  Serial.begin(115200);
  if (displayPrefs.begin("clock_display",false)) {
    const uint8_t level = displayPrefs.getUChar("level",clockDisplayOptions.level);
    const uint8_t timeout = displayPrefs.getUChar("timeout",clockDisplayOptions.timeout);
    if (level < clock_settings::levelCount) clockDisplayOptions.level = level;
    if (timeout < clock_settings::timeCount) clockDisplayOptions.timeout = timeout;
  }
  // The CST and CO5300 share GPIO2. Probe the CST while it can still pulse reset.
  touchReady = beginClockTouch(2);
  frameReady = beginClockFace(0);
  if (!touchReady) touchReady = beginClockTouch();
  page = 0;
  if (frameReady) {
    if (powerReady) updateClockBattery(battery, readClockPmu);
    if (face == ClockPeacockAnalog) clock_face::prepareLagoonCache();
    drawClockWakeHome();
    clock_face::panel.setBrightness(clockDisplayOptions.brightness());
    if (face != ClockPeacockAnalog) clock_face::prepareLagoonCache();
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  latestReading = xQueueCreate(1, sizeof(WeatherReading));
  serialFrameLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(networkWorker, "clock_network", 12288, nullptr, 1, &weatherNetworkTask, 0);
  Serial.println("WEATHER_CLOCK_READY");
  lastInteractionAt = millis();
}

static void changeClockScreen(ClockScreen next) {
  if (screen == next) return;
  if (next == ClockScreen::Off) {
    settingsOpen = false;
    screenOffAt = millis();
    shotRequested = false;
    clock_face::lagoonOnPanel = false;
    if (frameReady) {
      clock_face::panel.setBrightness(0);
      clock_face::panel.displayOff();
    }
  } else if (screen == ClockScreen::Off) {
    if (next == ClockScreen::On) page = 0;
    if (frameReady) {
      clock_face::panel.displayOn();
      if (next == ClockScreen::On) drawClockWakeHome();
    }
    requestClockWeatherRefresh();
  }
  const bool waking = screen == ClockScreen::Off;
  screen = next;
  lastInteractionAt = millis();
  repaint = true;
  if (next == ClockScreen::Confirm) {
    if (frameReady) drawClockPowerConfirm();
    resetClockConfirmTouch();
  } else if (next == ClockScreen::On) resetClockConfirmTouch();
  if (waking && frameReady) clock_face::panel.setBrightness(clockDisplayOptions.brightness());
}

static void enterClockSleep() {
  const bool pinReady = (!powerReady || clockGpioWakeProven()) && digitalRead(clockPowerPin) == LOW &&
                        esp_sleep_enable_ext0_wakeup(clockPowerPin, 1) == ESP_OK;
  if (!pinReady && !powerReady) return;
  const int flags = powerReady ? takeClockPmuKeyFlags() : -1;
  if (clockPmuWakeEvent(flags) || (pinReady && digitalRead(clockPowerPin) == HIGH && clockGpioWakeProven())) {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    seedClockWakeKey(flags < 0 ? 0 : flags);
    changeClockScreen(ClockScreen::On);
    return;
  }
  // Keep a recovery timer only when the ULP cannot read the PMIC on this board.
  checkPmuAfterSleep = !pinReady;
  if (!pinReady && esp_sleep_enable_timer_wakeup(keyCheckUs) != ESP_OK) return;
  esp_wifi_stop();
  if (touchReady) clock_touch::controller.sleep();
  clock_holdover::prepare();  // Select and calibrate before programming the ULP period.
  const bool ulp = !pinReady && clock_sleep_key::arm();
  if (ulp) esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  Serial.printf("CLOCK_SLEEP off_ms=%lu key=%s\n", (unsigned long)(millis() - screenOffAt),
                pinReady ? "gpio" : ulp ? "ulp" : "pmic-fallback");
  Serial.flush();
  esp_deep_sleep_start();
}

static void sendFrame() {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(clock_face::canvas.getFramebuffer());
  constexpr size_t bytes = 466u * 466u * sizeof(uint16_t);
  Serial.printf("CLOCK_FRAME 466 466 %u\n", static_cast<unsigned>(bytes));
  size_t sent = 0;
  uint32_t lastWrite = millis();
  while (sent < bytes) {
    const int space = Serial ? Serial.availableForWrite() : 0;
    if (space > 0) {
      const size_t chunk = min(size_t(1024), min(size_t(space), bytes - sent));
      const size_t wrote = Serial.write(data + sent, chunk);
      if (wrote) {
        sent += wrote;
        lastWrite = millis();
        continue;
      }
    }
    if (millis() - lastWrite >= 1500) return;
    delay(1);
  }
  Serial.print("\nCLOCK_FRAME_END\n");
}

static void readCommands(const tm& local, bool online, bool synced, bool stale, bool provisioning) {
  static char line[32];
  static size_t length;
  while (Serial.available()) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c != '\n') {
      if (length < sizeof(line)) line[length++] = static_cast<char>(c);
      continue;
    }
    if (length == sizeof(line)) {
      length = 0;
      continue;
    }
    line[length] = '\0';
    length = 0;
    if (strcmp(line, "status") == 0) {
      Serial.printf("CLOCK_STATUS wifi=%d ntp=%d local=%04d-%02d-%02d %02d:%02d:%02d city=Chengdu weather_epoch=%lld temperature=%.1f wmo=%d stale=%d touch=%d frame=%d page=%d analog=%d hourly=%d daily=%d display=%s off_ms=%lu power=%d wake=%d key_down=%d key_irq=%02x holds=%lu clicks=%lu gpio3=%d gpio_matched=%d face=%d battery_pct=%d battery_present=%d charging=%d vbus=%d\n",
                    online, synced, local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                    local.tm_hour, local.tm_min, local.tm_sec, (long long)weather.updated,
                    weather.data.temperature, weather.data.wmo, stale, touchReady, frameReady, page,
                    face == ClockMountainAnalog || clockFaceHasSweep(face), weather.updated ? 6 : 0, weather.updated ? 5 : 0,
                    screen == ClockScreen::Off ? "off" : screen == ClockScreen::Confirm ? "confirm" : settingsOpen && !provisioning ? "settings" : "on",
                    screen == ClockScreen::Off ? (unsigned long)(millis() - screenOffAt) : 0UL,
                    powerReady, (int)esp_sleep_get_wakeup_cause(), clockKey.down, lastKeyFlags,
                    (unsigned long)keyHolds, (unsigned long)keyClicks, digitalRead(clockPowerPin), clockGpioWakeProven(),
                    face, battery.percent, battery.valid ? int(battery.present) : -1,
                    battery.valid ? int(battery.charging) : -1, battery.valid ? int(battery.usb) : -1);
    } else if (strncmp(line, "page ", 5) == 0 && line[5] >= '0' && line[5] <= '3' && !line[6]) {
      settingsOpen = line[5] == '3';
      page = settingsOpen ? 0 : line[5] - '0';
      repaint = true;
      lastInteractionAt = millis();
      Serial.printf("CLOCK_PAGE page=%c\n", line[5]);
    } else if (!strcmp(line, "face analog") || !strcmp(line, "face digital")) {
      face = !strcmp(line, "face analog") ? ClockMountainAnalog : ClockMountainDigital;
      Serial.printf("CLOCK_FACE analog=%d\n", face == ClockMountainAnalog);
    } else if (!strncmp(line, "face ", 5) && line[5] >= '0' && line[5] < '0' + ClockFaceCount && !line[6]) {
      face = line[5] - '0';
      Serial.printf("CLOCK_FACE face=%d\n", face);
    } else if (!strcmp(line, "wifi info")) {
      Serial.printf("CLOCK_WIFI saved=%u online=%d setup=%d\n", savedNetworkCount.load(), online, provisioning);
    } else if (!strcmp(line, "wifi setup")) {
      if (!provisioning) openClockWifiSetup();
    } else if (!strcmp(line, "wifi close")) {
      if (provisioning) requestClockWifiSetupClose();
    } else if (!strcmp(line, "rtc")) {
      using namespace clock_holdover;
      Serial.printf("CLOCK_RTC restored=%d entry_q19=%lu wake_q19=%lu sleep_us=%lld sdk_delta_us=%lld ntp_error_ms=%ld trim_ppm=%ld next_ppm=%ld\n",
                    restoredUtc!=0,(unsigned long)saved.period,(unsigned long)wakePeriod,
                    (long long)elapsedUs,(long long)sdkDeltaUs,(long)ntpErrorMs.load(),
                    (long)restoredTrim,(long)learnedPpm.load());
    } else if (!strcmp(line, "perf")) {
      Serial.printf("CLOCK_PERF frames=%lu max_gap_us=%lu last_pixels=%lu now_us=%lu\n",
                    (unsigned long)sweepFrames, (unsigned long)longestSweepGap,
                    (unsigned long)clock_face::lastSweepPixels, (unsigned long)micros());
      longestSweepGap = 0;
    } else if (!strcmp(line, "shot")) {
      if (provisioning || !frameReady || screen == ClockScreen::Off) Serial.println("CLOCK_FRAME_UNAVAILABLE");
      else shotRequested = true;
    }
  }
}

void loop() {
  static int lastPage = -1, lastState = -1, lastFace = -1;
  static time_t lastMinute = -1;
  static int lastDay = -1;
  static uint32_t nextSweepAt;
  static bool lastStale, lastHint;
  static uint32_t sampledBatteryAt;
  static bool sampledBattery;
  static char lastSetupCode[9] = {};
  static uint32_t hintStart = millis();
  static bool hintActive = true;
  const bool fresh = xQueueReceive(latestReading, &weather, 0) == pdTRUE;
  char setupCode[9];
  const bool provisioning = clockWifiSetupCode(setupCode);
  const ClockKeyEvent key = pollClockKey();
  if (!shutdownPending && key == ClockKeyEvent::Hold) changeClockScreen(ClockScreen::Confirm);
  if (!shutdownPending && key == ClockKeyEvent::Click)
    changeClockScreen(screen == ClockScreen::On ? ClockScreen::Off : ClockScreen::On);
  timeval wall;
  struct tm local{};
  const bool synced = readClockLocalTime(wall, local);
  const time_t now = wall.tv_sec;
  const bool online = WiFi.status() == WL_CONNECTED;
  const bool stale = weather.updated && (!online || now - weather.updated > 30 * 60);
  bool batteryChanged = false;
  if (screen == ClockScreen::On && (!sampledBattery || repaint || millis() - sampledBatteryAt >= 15000)) {
    batteryChanged = updateClockBattery(battery, readClockPmu);
    sampledBatteryAt = millis();
    sampledBattery = true;
  }
  readCommands(local, online, synced, stale, provisioning);
  if (screen == ClockScreen::Off) {
    lastSweepCommit = 0;
    readClockSwipe(false);  // Discard touches while the panel is dark.
    if (!clockPowerKeyBusy() && millis() - screenOffAt >= clockSleepDelay) enterClockSleep();
    delay(10);
    return;
  }
  if (screen == ClockScreen::Confirm) {
    int choice = shutdownPending ? 0 : readClockConfirmTap();
    if (choice < 0) changeClockScreen(ClockScreen::On);
    if (choice > 0) shutdownPending = true;
    if (shutdownPending && !clockPowerKeyBusy()) {
      // Release PWRON before cutting power so the same press cannot start the PMIC again.
      shutdownPending = false;
      if (powerReady && requestClockShutdown()) {
        delay(350);  // If external power keeps the SoC alive, leave it asleep until POWER.
        changeClockScreen(ClockScreen::Off);
        enterClockSleep();
      } else {
        if (frameReady) {
          clock_face::label(clock_face::center,365,"稍后重试",face_type::Small,clock_face::warm);
          clock_face::canvas.flush();
        }
        powerReady = beginClockPmu();
      }
    }
  }
  if (screen == ClockScreen::On) {
    if (!provisioning && lastSetupCode[0]) resetClockConfirmTouch();
    if (settingsOpen && !provisioning) applySettingsTap(readClockSettingsTap());
    else if (!provisioning) {
      const int gesture = readClockSwipe(page == 0);
      if (gesture == 3) {
        settingsOpen = true;
        resetClockConfirmTouch();
        repaint = true;
      } else if (gesture) {
        applyClockGesture(gesture,page,face);
        hintActive = false;
      }
    }
    if (provisioning || clock_touch::touching || clockPowerKeyBusy()) lastInteractionAt = millis();
    if (clockDisplayOptions.idleMs() && millis()-lastInteractionAt >= clockDisplayOptions.idleMs()) {
      changeClockScreen(ClockScreen::Off);
      return;
    }
  }
  if (repaint && screen == ClockScreen::On) {
    lastSetupCode[0] = 0;
    lastPage = -1;
    repaint = false;
  }
  const bool sweep = frameReady && synced && screen == ClockScreen::On && !provisioning && !settingsOpen && page == 0 && clockFaceHasSweep(face);
  if (!sweep) lastSweepCommit = 0;
  if (screen == ClockScreen::On && provisioning) {
    if (strcmp(setupCode, lastSetupCode) != 0) {
      resetClockConfirmTouch();
      if (frameReady) drawClockWifiSetup(clock_wifi_setup::ssid, setupCode);
    }
    if (readClockSetupDismiss()) requestClockWifiSetupClose();
    memcpy(lastSetupCode, setupCode, sizeof(lastSetupCode));
    if (shotRequested) Serial.println("CLOCK_FRAME_UNAVAILABLE");
    shotRequested = false;
  } else if (screen == ClockScreen::On && settingsOpen) {
    if (frameReady && (lastPage != 3 || lastSetupCode[0])) drawClockSettings(clockDisplayOptions);
    lastSetupCode[0] = 0;
    lastPage = 3;
  } else if (screen == ClockScreen::On) {
    if (lastSetupCode[0]) {
      lastSetupCode[0] = 0;
      hintStart = millis();
      hintActive = true;
      lastPage = -1;
    }
    if (millis() - hintStart >= 7000) hintActive = false;
    const int state = clockWeatherState(online, synced);
    const time_t minute = synced ? now / 60 : 0;
    const int day = synced ? local.tm_yday : -1;
    const bool timeOnly = page == 0 && face >= ClockExplorer && face < ClockFaceCount;
    const bool homeChanged = page == 0 && (minute != lastMinute || face != lastFace ||
                             (!timeOnly && (batteryChanged || hintActive != lastHint)));
    const bool redraw = page != lastPage || homeChanged || (!timeOnly && (fresh || stale != lastStale || state != lastState)) ||
                        (page == 2 && day != lastDay);
    const float seconds = sweep ? local.tm_sec + wall.tv_usec * .000001f : 0;
    const uint32_t tick = micros();
    bool committed = false;
    if (frameReady && redraw) {
      drawWeatherPage(page, face, synced ? &local : nullptr, weather.updated ? &weather.data : nullptr,
                      weatherStatus[state], stale, hintActive, &battery, seconds);
      lastPage = page;
      lastFace = face;
      lastStale = stale;
      lastState = state;
      lastMinute = minute;
      lastDay = day;
      lastHint = hintActive;
      committed = sweep;
      nextSweepAt = tick + sweepPeriodUs;
    } else if (sweep && (int32_t)(tick - nextSweepAt) >= 0) {
      committed = updateLagoonSecond(seconds);
      nextSweepAt += (1 + (tick - nextSweepAt) / sweepPeriodUs) * sweepPeriodUs;
    }
    if (committed) {
      const uint32_t done = micros();
      if (lastSweepCommit) longestSweepGap = max(longestSweepGap, done - lastSweepCommit);
      lastSweepCommit = done;
      ++sweepFrames;
    }
  }
  if (!provisioning && shotRequested && screen != ClockScreen::Off && xSemaphoreTake(serialFrameLock, 0) == pdTRUE) {
    sendFrame();
    shotRequested = false;
    xSemaphoreGive(serialFrameLock);
  }
  // Wake near the next sweep deadline, while keeping touch and POWER polling responsive.
  const int32_t untilSweep = (int32_t)(nextSweepAt - micros());
  delay(sweep ? constrain(untilSweep / 1000, 1, 8) : 8);
}
