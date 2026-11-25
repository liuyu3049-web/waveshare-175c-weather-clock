#pragma once

#include <stdint.h>

namespace clock_settings {
constexpr uint8_t levels[] = {26, 64, 108, 166, 255};
constexpr uint16_t seconds[] = {15, 30, 60, 120, 300, 0};
constexpr const char* times[] = {"15秒", "30秒", "1分", "2分", "5分", "常亮"};
constexpr int levelCount = sizeof(levels) / sizeof(levels[0]);
constexpr int timeCount = sizeof(seconds) / sizeof(seconds[0]);
constexpr int levelStartX = 97, levelStep = 68, levelY = 164;
constexpr int timeStartX = 68, timeStep = 66, timeY = 253;
enum Action { None, Back, Wifi, Level, Timeout = Level + levelCount };

inline int hit(int x, int y) {
  if (y >= levelY-24 && y <= levelY+24 && x >= levelStartX-24 && x <= levelStartX+(levelCount-1)*levelStep+24)
    return Level + (x-levelStartX+levelStep/2)/levelStep;
  if (y >= timeY-24 && y <= timeY+24 && x >= timeStartX-timeStep/2 && x < timeStartX+(timeCount-1)*timeStep+timeStep/2)
    return Timeout + (x-timeStartX+timeStep/2)/timeStep;
  if (y >= 300 && y <= 357 && x >= 76 && x <= 390) return Wifi;
  if (y >= 375 && y <= 428 && x >= 165 && x <= 301) return Back;
  return None;
}
}

struct ClockDisplayOptions {
  uint8_t level = 2, timeout = clock_settings::timeCount-1;
  uint8_t brightness() const { return clock_settings::levels[level]; }
  uint32_t idleMs() const { return 1000UL * clock_settings::seconds[timeout]; }
};
