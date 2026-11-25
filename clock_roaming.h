#pragma once

#include <WiFi.h>
#include <esp_wifi.h>
#include "clock_networks.h"

namespace clock_roaming {

// Read the original SDK station once, before selecting WIFI_STORAGE_RAM.
inline void importNativeIfLegacy(clock_networks::List& list, bool legacy) {
  if (!legacy) return;
  wifi_config_t config{};
  if (esp_wifi_get_config(WIFI_IF_STA, &config) != ESP_OK) return;
  clock_networks::Profile profile{};
  memcpy(profile.ssid, config.sta.ssid, sizeof(config.sta.ssid));
  memcpy(profile.password, config.sta.password, sizeof(config.sta.password));
  if (!profile.ssid[0] || clock_networks::find(list, profile.ssid) >= 0 || list.count >= clock_networks::capacity) {
    if (list.count) clock_networks::store(list);
    return;
  }
  // An unavailable NVS write must not prevent using a working SDK profile for this boot.
  if (clock_networks::save(list, profile.ssid, profile.password) == clock_networks::Save::Error)
    list.entries[list.count++] = profile;
}

class Roamer {
 public:
  // Hand ownership to manual provisioning without dropping an established link.
  void reset() {
    if (phase == Scanning) {
      esp_wifi_scan_stop();
      WiFi.scanDelete();
    } else if (phase == Joining && WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect(false, false);
    }
    phase = Idle;
    count = cursor = 0;
  }

  // Called by the network worker and by an idle automatic setup portal.
  // True once per exhausted round, so the caller can offer phone provisioning.
  bool tick(const clock_networks::List& list) {
    if (WiFi.status() == WL_CONNECTED) {
      reset();
      return false;
    }
    const uint32_t now = millis();
    if (phase == Cooling) {
      if (now - started < 15000) return false;
      phase = Idle;
    }
    if (!list.count) { reset(); return finish(now); }
    if (phase == Joining) {
      if (now - started < 9000) return false;
      WiFi.disconnect(false, false);
    } else {
      int found;
      if (phase == Idle) {
        started = now;
        phase = Scanning;
        found = WiFi.scanNetworks(true, true, false, 150);
      } else {
        found = WiFi.scanComplete();
      }
      if (found == WIFI_SCAN_RUNNING && now - started < 6500) return false;
      if (found < 0) esp_wifi_scan_stop();
      rank(list, found);
      WiFi.scanDelete();
    }
    if (cursor >= count || order[cursor] >= list.count) return finish(now);
    const auto& selected = list.entries[order[cursor++]];
    WiFi.begin(selected.ssid, selected.password);
    phase = Joining;
    started = now;
    return false;
  }

 private:
  enum Phase { Idle, Scanning, Joining, Cooling } phase = Idle;
  uint32_t started = 0;
  uint8_t order[clock_networks::capacity]{}, count = 0, cursor = 0, nextHidden = 0;

  bool finish(uint32_t now) {
    phase = Cooling;
    started = now;
    return true;
  }

  void rank(const clock_networks::List& list, int found) {
    int rssi[clock_networks::capacity];
    for (int i = 0; i < list.count; ++i) rssi[i] = -128;
    bool hidden = found < 0;
    // Arduino scan result indices are uint8_t.
    for (int row = 0; row < found && row < 256; ++row) {
      const String ssid = WiFi.SSID(row);
      if (ssid.isEmpty()) { hidden = true; continue; }
      const int index = clock_networks::find(list, ssid);
      const int signal = WiFi.RSSI(row);
      if (index >= 0 && signal > rssi[index]) rssi[index] = signal;
    }
    count = cursor = 0;
    for (int i = 0; i < list.count; ++i) {
      if (rssi[i] == -128) continue;
      int j = count;
      while (j > 0 && rssi[order[j-1]] < rssi[i]) {
        order[j] = order[j-1];
        --j;
      }
      order[j] = i;
      ++count;
    }
    // Hidden beacons carry no usable name. Probe one unseen profile per round
    // so one unrelated hidden AP cannot delay the phone portal by N timeouts.
    if (hidden) {
      for (int i = 0; i < list.count; ++i) {
        nextHidden %= list.count;
        const int index = nextHidden++;
        if (rssi[index] == -128) { order[count++] = index; break; }
      }
    }
  }
};
}  // namespace clock_roaming
