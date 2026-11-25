#pragma once

#include <stdint.h>

struct ClockBattery {
  int8_t percent = -1;
  bool present = false;
  bool usb = false;
  bool charging = false;
  bool valid = false;
};

// Pass the board's readClockPmu. Returns true if the visible state changed.
inline bool updateClockBattery(ClockBattery& out, int (*readClockPmu)(uint8_t)) {
  ClockBattery next{};
  const int status1 = readClockPmu(0x00);
  const int status2 = readClockPmu(0x01);
  // STATUS1 has six defined bits. The defined STATUS2 current directions are 0, 1, and 2.
  if (status1 >= 0 && status1 < 0x40 && status2 >= 0 && status2 < 0x60 && readClockPmu(0x68) == 1) {
    next.valid = true;
    next.present = status1 & 0x08;
    next.usb = (status1 & 0x20) && !(status2 & 0x08);  // VBUS good, outside input voltage limiting.
    next.charging = next.present && ((status2 >> 5) == 1);  // Battery current flows into the cell.
    if (next.present) {
      const int gauge = readClockPmu(0x18);
      if (gauge >= 0 && gauge <= 0x0f && (gauge & 0x08)) {
        const int percent = readClockPmu(0xa4);
        if (percent >= 0 && percent <= 100) next.percent = static_cast<int8_t>(percent);
      }
    }
  }
  const bool changed = next.percent != out.percent || next.present != out.present || next.usb != out.usb ||
                       next.charging != out.charging || next.valid != out.valid;
  out = next;
  return changed;
}
