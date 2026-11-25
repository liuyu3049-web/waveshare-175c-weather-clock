#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

// On the 1.75C, Key2 drives the AXP2101 PWRON and the inverted SYS_OUT on GPIO3.
static constexpr gpio_num_t clockPowerPin = GPIO_NUM_3;
static constexpr uint32_t clockSleepDelay = 5UL * 60 * 1000;
enum class ClockKeyEvent { None, Click, Hold };
enum class ClockScreen { On, Off, Confirm };

static struct {
  bool raw, down, suppressClick, longSent;
  uint32_t edgeAt, pressedAt;
} clockKey;

// IRQ status 0x49 is latched, including when the host isn't sampling SYS_OUT.
static constexpr uint8_t clockRelease = 1, clockPress = 2, clockLong = 4, clockShort = 8;
static bool pmuKeys, gpioHighWhilePressed, gpioEdgesMatched;
static uint32_t sampledAt, finishedAt, keyHolds, keyClicks;
static uint8_t lastKeyFlags;
static unsigned pmuLongMs = 1500;
static bool haveReleased;

static bool clockGpioWakeProven() { return gpioEdgesMatched; }
static bool clockPowerKeyBusy() { return clockKey.down || (!pmuKeys && clockKey.raw); }

static void beginClockKey() {
  rtc_gpio_deinit(clockPowerPin);
  pinMode(clockPowerPin, INPUT);
  bool high = digitalRead(clockPowerPin);
  clockKey = {high, high, high, false, millis(), millis()};
}

static ClockKeyEvent pollClockGpioKey() {
  const uint32_t now = millis();
  const bool high = digitalRead(clockPowerPin);
  if (high != clockKey.raw) {
    clockKey.raw = high;
    clockKey.edgeAt = now;
  }
  if (high != clockKey.down && now - clockKey.edgeAt >= 25) {
    clockKey.down = high;
    if (high) {
      clockKey.pressedAt = clockKey.edgeAt;
      clockKey.suppressClick = false;
      clockKey.longSent = false;
    } else if (!clockKey.longSent && !clockKey.suppressClick) {
      return clockKey.edgeAt - clockKey.pressedAt >= 3000 ? ClockKeyEvent::Hold : ClockKeyEvent::Click;
    }
  }
  if (high && clockKey.down && !clockKey.longSent && now - clockKey.pressedAt >= 3000) {
    clockKey.longSent = true;
    return ClockKeyEvent::Hold;
  }
  return ClockKeyEvent::None;
}

static int readClockPmu(uint8_t reg) {
  Wire.beginTransmission(0x34);
  Wire.write(reg);
  if (Wire.endTransmission(false) || Wire.requestFrom(uint8_t(0x34), size_t(1)) != 1) return -1;
  return Wire.read();
}

static bool writeClockPmu(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(0x34);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static int takeClockPmuKeyFlags() {
  int flags = readClockPmu(0x49);
  if (flags < 0 || flags == 0xff) return -1;
  flags &= 0x0f;
  if (flags && !writeClockPmu(0x49, flags)) return -1;
  if (flags) lastKeyFlags = flags;
  return flags;
}

static void seedClockWakeKey(uint8_t flags) {
  const bool held = !(flags & (clockRelease | clockShort)) &&
                    ((flags & (clockPress | clockLong)) || digitalRead(clockPowerPin));
  const uint32_t now = millis();
  clockKey = {held, held, true, false, now, now - ((flags & clockLong) ? pmuLongMs : 0)};
  finishedAt = now;
  haveReleased = !held;
}

static ClockKeyEvent pollClockKey() {
  if (!pmuKeys) return pollClockGpioKey();
  const uint32_t now = millis();
  if (now - sampledAt < 15) return ClockKeyEvent::None;
  sampledAt = now;
  const int flags = takeClockPmuKeyFlags();
  if (flags < 0) return ClockKeyEvent::None;
  bool gpioHigh = digitalRead(clockPowerPin);
  if (!clockKey.down && (flags & (clockPress | clockLong))) {
    clockKey.down = true;
    clockKey.longSent = false;
    clockKey.suppressClick = false;
    clockKey.pressedAt = now - ((flags & clockLong) ? pmuLongMs : 0);
    gpioHighWhilePressed = false;
  }
  if (clockKey.down && !(flags & (clockRelease | clockShort)) && gpioHigh)
    gpioHighWhilePressed = true;
  if (flags & (clockRelease | clockShort)) {
    const bool wasDown = clockKey.down;
    const bool duplicate = !wasDown && haveReleased && now - finishedAt < 250;
    const bool consumed = clockKey.longSent || clockKey.suppressClick;
    if (wasDown && (flags & clockRelease))
      gpioEdgesMatched = gpioHighWhilePressed && !gpioHigh;
    clockKey.down = clockKey.raw = false;
    clockKey.longSent = clockKey.suppressClick = false;
    haveReleased = true;
    finishedAt = now;
    if (consumed || duplicate) return ClockKeyEvent::None;
    if (wasDown && !(flags & clockShort) && now - clockKey.pressedAt >= 3000) {
      ++keyHolds;
      return ClockKeyEvent::Hold;
    }
    if (wasDown || (flags & clockShort)) {
      ++keyClicks;
      return ClockKeyEvent::Click;
    }
  } else if (clockKey.down && !clockKey.longSent && now - clockKey.pressedAt >= 3000) {
    clockKey.longSent = true;
    ++keyHolds;
    return ClockKeyEvent::Hold;
  }
  return ClockKeyEvent::None;
}

static bool beginClockPmu() {
  if (readClockPmu(0x03) != 0x4A) return false;
  // Let the user decide on screen, even if they keep holding POWER.
  const int off = readClockPmu(0x22);
  if (off < 0) return false;
  if ((off & 2) && !writeClockPmu(0x22, off & ~2)) return false;
  if (readClockPmu(0x22) != (off & ~2)) return false;
  const int enables = readClockPmu(0x41);
  if (enables < 0 || !writeClockPmu(0x41, enables | 0x0f)) return false;
  const int enabled = readClockPmu(0x41);
  if (enabled < 0 || (enabled & 15) != 15) return false;
  const int timing = readClockPmu(0x27);
  if (timing >= 0) pmuLongMs = 1000 + ((timing >> 4) & 3) * 500;
  clockKey = {};
  pmuKeys = true;
  return true;
}

static bool requestClockShutdown() {
  int control = readClockPmu(0x10);
  return control >= 0 && writeClockPmu(0x10, control | 1);
}
