#pragma once

#include <Preferences.h>
#include <stddef.h>
#include <string.h>

namespace clock_networks {
constexpr int capacity = 16;
struct Profile {
  char ssid[33], password[65];
};
struct List {
  uint8_t count = 0;
  Profile entries[capacity]{};
  bool writable = false;  // RAM only; set after loading the current NVS snapshot.
};
enum class Save { Ok, Full, Invalid, Error };

inline int find(const List& list, const char* ssid) {
  for (int i = 0; i < list.count; ++i)
    if (!strcmp(list.entries[i].ssid,ssid)) return i;
  return -1;
}
inline int find(const List& list, const String& ssid) { return find(list,ssid.c_str()); }

inline Save setProfile(List& list, const String& ssid, const String& password) {
  if (ssid.isEmpty() || ssid.length() > 32 || !password.c_str() || password.length() > 64 ||
      strlen(ssid.c_str()) != ssid.length() || strlen(password.c_str()) != password.length())
    return Save::Invalid;
  int i = find(list,ssid);
  if (i < 0) {
    if (list.count == capacity) return Save::Full;
    i = list.count++;
  }
  Profile& p = list.entries[i];
  p = {};
  memcpy(p.ssid,ssid.c_str(),ssid.length()+1);
  memcpy(p.password,password.c_str(),password.length()+1);
  return Save::Ok;
}

inline size_t bytesFor(const List& list) {
  return offsetof(List,entries) + list.count * sizeof(Profile);
}

inline bool store(const List& list) {
  if (!list.writable) return false;
  Preferences nvs;
  if (!nvs.begin("clock-wifi",false)) return false;
  const size_t bytes = bytesFor(list);
  return nvs.putBytes("profiles-v1",&list,bytes) == bytes;
}

inline Save save(List& list, const String& ssid, const String& password) {
  List next = list;
  const Save result = setProfile(next,ssid,password);
  if (result != Save::Ok) return result;
  if (!store(next)) return Save::Error;
  list = next;
  return Save::Ok;
}

inline bool forget(List& list, const String& ssid) {
  const int i = find(list,ssid);
  if (i < 0) return false;
  List next = list;
  --next.count;
  for (int j = i; j < next.count; ++j) next.entries[j] = next.entries[j+1];
  next.entries[next.count] = {};
  if (!store(next)) return false;
  list = next;
  return true;
}

inline void readLegacy(List& list, Preferences& nvs, const char* ssidKey, const char* passKey) {
  const String ssid = nvs.getString(ssidKey,"");
  if (!ssid.isEmpty() && find(list,ssid) < 0)
    setProfile(list,ssid,nvs.getString(passKey,""));
}

inline bool load(List& list, bool* legacy = nullptr) {
  if (legacy) *legacy = false;
  list.writable = false;
  List next;
  Preferences nvs;
  if (!nvs.begin("clock-wifi",false)) return false;
  if (nvs.isKey("profiles-v1")) {
    const size_t bytes = nvs.getBytesLength("profiles-v1");
    if (bytes < offsetof(List,entries) || bytes > offsetof(List,writable) ||
        nvs.getBytes("profiles-v1",&next,bytes) != bytes ||
        next.count > capacity || bytesFor(next) != bytes) return false;
    for (int i = 0; i < next.count; ++i) {
      const Profile& p = next.entries[i];
      if (!p.ssid[0] || !memchr(p.ssid,0,sizeof(p.ssid)) ||
          !memchr(p.password,0,sizeof(p.password)) || find(next,p.ssid) != i) return false;
    }
    next.writable = true;
    list = next;
    return true;
  }
  // Older versions tried these namespaces in this order. Keep the first password for each SSID.
  readLegacy(next,nvs,"ssid","password");
  nvs.end();
  if (nvs.begin("desktop",true)) {
    readLegacy(next,nvs,"ssid","password");
    nvs.end();
  }
  if (nvs.begin("wifi",true)) {
    const int count = nvs.getInt("n",0);
    for (int i = 0; i < count && i < 10; ++i) {
      char ssidKey[] = "s0", passKey[] = "p0";
      ssidKey[1] = passKey[1] = '0'+i;
      readLegacy(next,nvs,ssidKey,passKey);
    }
  }
  next.writable = true;
  list = next;
  if (legacy) *legacy = true;
  return true;
}
}  // namespace clock_networks
