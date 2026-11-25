#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvCSTXXX.hpp>
#include "clock_settings.h"

namespace clock_power_buttons {
constexpr int cancelX = 146, confirmX = 320, centerY = 303, halfSpan = 39, radius = 38;

inline int hit(int x, int y) {
  int action = x < 233 ? -1 : 1;
  int dx = abs(x - (action < 0 ? cancelX : confirmX)) - halfSpan;
  if (dx < 0) dx = 0;
  int dy = y - centerY;
  return dx*dx + dy*dy <= radius*radius ? action : 0;
}
}

namespace clock_touch {
static TouchDrvCST92xx controller;
static constexpr int interruptPin = 11;
static volatile bool reportPending = false;
static bool ready = false;
static bool touching = false, consumed = false;
static struct {
  uint32_t at;
  int16_t x, y;
  bool pending;
} firstTap;

static void IRAM_ATTR onReport() {
  reportPending = true;
}
}

inline bool beginClockTouch(int resetPin = -1) {
  using namespace clock_touch;
  controller.setPins(resetPin, interruptPin);
  ready = controller.begin(Wire, 0x5A, 15, 14);
  if (ready) {
    controller.setMaxCoordinates(466, 466);
    controller.setMirrorXY(true, true);
    pinMode(interruptPin, INPUT_PULLUP);
    attachInterrupt(interruptPin, onReport, FALLING);
    reportPending = true;
  }
  return ready;
}

// A contact held before the dialog appeared must lift before a new press counts.
inline void resetClockConfirmTouch() {
  using namespace clock_touch;
  firstTap.pending = false;
  if (ready && reportPending) {
    reportPending = false;
    int16_t x[2], y[2];
    touching = controller.getPoint(x, y, 2) != 0;
  }
  consumed = touching;
}

namespace clock_touch {
enum class View { Weather, Confirm, Wifi, Settings };

inline int readGesture(View view, bool home = false) {
  static int16_t startX, startY, lastX, lastY;
  static uint32_t started;
  static bool moved = false, homeContact = false;
  const uint32_t now = millis();
  if (!home || now - firstTap.at > 350) firstTap.pending = false;
  if (!home) homeContact = false;
  if (!ready) return 0;
  auto homeHold = [&]() {
    if (!touching || !homeContact || consumed || moved || now-started < 1500) return 0;
    consumed = true;
    firstTap.pending = false;
    return 3;
  };
  if (!reportPending) return homeHold();
  reportPending = false;

  int16_t x[2], y[2];  // CST92xx can report both contacts in one frame.
  const uint8_t count = controller.getPoint(x, y, 2);
  if (count == 0) {
    if (!touching) return 0;
    const int hold = homeHold();
    touching = false;
    if (hold) return hold;
    const uint32_t held = now - started;
    if (!consumed && view == View::Settings) {
      const int start = clock_settings::hit(startX,startY), end = clock_settings::hit(lastX,lastY);
      if (start >= clock_settings::Level && start < clock_settings::Timeout &&
          end >= clock_settings::Level && end < clock_settings::Timeout) return end;
      return !moved && held <= 600 && start == end ? start : 0;
    }
    if (consumed || moved || held > (view == View::Confirm ? 600u : 300u)) {
      firstTap.pending = false;
      return 0;
    }
    if (view == View::Confirm) return clock_power_buttons::hit(startX, startY);
    if (view == View::Wifi) return 4;
    if (!homeContact) return 0;
    const int dx = startX - firstTap.x, dy = startY - firstTap.y;
    if (firstTap.pending && dx*dx + dy*dy <= 36*36) {
      firstTap.pending = false;
      return 2;
    }
    firstTap = {now, startX, startY, true};
    return 0;
  }
  if (!touching) {
    startX = x[0];
    startY = y[0];
    started = now;
    homeContact = home;
    consumed = false;
    moved = false;
  }
  touching = true;
  lastX = x[0];
  lastY = y[0];
  if (count > 1) consumed = true;
  const int dx = x[0] - startX;
  const int dy = y[0] - startY;
  if (abs(dx) > 14 || abs(dy) > 14) moved = true;
  if (view == View::Confirm) {
    if (clock_power_buttons::hit(x[0], y[0]) != clock_power_buttons::hit(startX, startY)) consumed = true;
    return 0;
  }
  if (view == View::Settings) return 0;
  if (consumed || abs(dx) < 48 || abs(dx) <= abs(dy)) return homeHold();
  consumed = true;
  return dx < 0 ? 1 : -1;
}
}

// Finger left: +1. Finger right: -1. Two nearby releases on the home page: +2.
inline int readClockSwipe(bool home = true) { return clock_touch::readGesture(clock_touch::View::Weather, home); }

inline bool readClockSetupDismiss() { return clock_touch::readGesture(clock_touch::View::Wifi) == 4; }
inline int readClockSettingsTap() { return clock_touch::readGesture(clock_touch::View::Settings); }

enum ClockFaceId : uint8_t {
  ClockMountainDigital, ClockMountainAnalog, ClockRetroFlip, ClockPeacockAnalog,
  ClockExplorer, ClockMoonwatch, ClockPanoGreen, ClockFaceCount
};

inline bool clockFaceHasSweep(uint8_t face) { return face >= ClockPeacockAnalog && face < ClockFaceCount; }

inline void applyClockGesture(int gesture, int& page, uint8_t& face) {
  if (gesture == 2 && page == 0) face = (face + 1) % ClockFaceCount;
  if (gesture == 1 || gesture == -1) page = (page + 3 + gesture) % 3;
}

// Single-finger press and release on the same button: cancel -1, power off +1.
inline int readClockConfirmTap() { return clock_touch::readGesture(clock_touch::View::Confirm); }
