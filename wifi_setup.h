#pragma once

#include <WiFi.h>
#include <WebServer.h>
#include <esp_random.h>
#include "clock_networks.h"
#include "clock_roaming.h"

namespace clock_wifi_setup {
constexpr char ssid[] = "WeatherClock-62A4";
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static char activeCode[9] = {};
static bool closeRequested;

static void publishCode(const char (&code)[9]) {
  portENTER_CRITICAL(&lock);
  memcpy(activeCode, code, sizeof(activeCode));
  closeRequested = false;
  portEXIT_CRITICAL(&lock);
}

static bool shouldClose() {
  portENTER_CRITICAL(&lock);
  const bool close = closeRequested;
  portEXIT_CRITICAL(&lock);
  return close;
}

static const char header[] PROGMEM = R"html(<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>天气钟配网</title><style>body{font:17px sans-serif;max-width:26em;margin:2em auto;padding:0 1em}label{display:block;margin:.8em 0}input,button{font:inherit;padding:.45em}input{display:block;box-sizing:border-box;width:100%}button{margin:.2em .4em .2em 0}li{overflow-wrap:anywhere;margin:.35em 0}li form{display:inline}</style>
<h1>天气钟配网</h1>)html";
static const char form[] PROGMEM = R"html(<form method="post" action="/connect">
<label>2.4 GHz 网络名称<input name="ssid" maxlength="32" autocomplete="off" required></label>
<label>网络密码（开放网络留空）<input name="password" type="password" maxlength="64" autocomplete="new-password"></label>
<button name="action" value="join">连接并保存</button><button name="action" value="save">仅保存</button>
</form><p>不在网络附近时可以仅保存。再次保存同名网络会更新密码。</p>)html";
static const char finish[] PROGMEM = R"html(<form method="post" action="/done"><button>完成配网</button></form></html>)html";

static String escapeName(const char* name) {
  String out(name);
  out.replace("&","&amp;"); out.replace("<","&lt;"); out.replace(">","&gt;");
  out.replace("\"","&quot;"); out.replace("'","&#39;");
  return out;
}
}

inline bool clockWifiSetupCode(char (&code)[9]) {
  using namespace clock_wifi_setup;
  portENTER_CRITICAL(&lock);
  memcpy(code, activeCode, sizeof(activeCode));
  portEXIT_CRITICAL(&lock);
  return code[0] != 0;
}

inline void requestClockWifiSetupClose() {
  using namespace clock_wifi_setup;
  portENTER_CRITICAL(&lock);
  if (activeCode[0]) closeRequested = true;
  portEXIT_CRITICAL(&lock);
}

inline bool runClockWifiSetup(clock_networks::List& profiles, clock_roaming::Roamer& roaming, bool manual) {
  using namespace clock_wifi_setup;
  using clock_networks::Save;
  char code[9];
  snprintf(code,sizeof(code),"%08lu",static_cast<unsigned long>(esp_random()%100000000UL));
  if (!WiFi.softAP(ssid,code)) {
    WiFi.softAPdisconnect(true);
    return WiFi.status() == WL_CONNECTED;
  }
  WebServer server(WiFi.softAPIP(),80);
  enum class Join { Idle, Disconnecting, Connecting, Saved };
  Join join = Join::Idle;
  String target, password;
  const char* feedback = "";
  uint32_t phaseAt = millis(), lastActivity = phaseAt;
  bool done = false;
  auto pending = [&]() { return join == Join::Disconnecting || join == Join::Connecting; };
  auto redirect = [&]() {
    server.sendHeader("Location","/");
    server.send(303,"text/plain","");
  };
  auto save = [&]() {
    const Save result = clock_networks::save(profiles,target,password);
    if (result == Save::Full) feedback = "已存满，请先忘记一个网络。";
    if (result == Save::Invalid) feedback = "网络名称或密码长度不正确。";
    if (result == Save::Error) feedback = "保存失败，请重试。";
    return result == Save::Ok;
  };
  server.on("/",HTTP_GET,[&]() {
    lastActivity = millis();
    String html(header);
    if (pending()) {
      server.sendHeader("Refresh","2; url=/");
      html += "<p>正在验证网络连接，请稍候。</p>";
    } else {
      if (*feedback) { html += "<p>"; html += feedback; html += "</p>"; }
      if (join != Join::Saved) {
        html += form;
        html += "<h2>已保存的网络（";
        html += profiles.count;
        html += "/";
        html += clock_networks::capacity;
        html += "）</h2><ul>";
        for (int i=0; i<profiles.count; ++i) {
          const String name = escapeName(profiles.entries[i].ssid);
          html += "<li>"; html += name;
          html += " <form method=\"post\" action=\"/forget\"><input type=\"hidden\" name=\"ssid\" value=\"";
          html += name;
          html += "\"><button>忘记</button></form></li>";
        }
        html += "</ul>";
      }
    }
    html += finish;
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"text/html; charset=utf-8",html);
  });
  server.on("/connect",HTTP_POST,[&]() {
    lastActivity = millis();
    if (join != Join::Idle) { redirect(); return; }
    target = server.arg("ssid");
    password = server.arg("password");
    if (!target.length() || target.length()>32 || password.length()>64 ||
        strlen(target.c_str())!=target.length() || strlen(password.c_str())!=password.length()) {
      feedback = "网络名称或密码长度不正确。";
    } else if (clock_networks::find(profiles,target)<0 && profiles.count==clock_networks::capacity) {
      feedback = "已存满，请先忘记一个网络。";
    } else if (server.arg("action")=="save") {
      if (save()) {
        roaming.reset();
        manual = true; // The phone may be entering several networks from another location.
        feedback = "已保存。到该网络附近时，天气钟会自动尝试连接。";
      }
    } else {
      roaming.reset();
      WiFi.disconnect(false,false);
      join = Join::Disconnecting;
      phaseAt = millis();
      feedback = "";
    }
    if (!pending()) password = "";
    redirect();
  });
  server.on("/forget",HTTP_POST,[&]() {
    lastActivity = millis();
    if (join != Join::Idle) { redirect(); return; }
    const String name = server.arg("ssid");
    if (clock_networks::forget(profiles,name)) {
      roaming.reset();
      if (WiFi.status()!=WL_CONNECTED || WiFi.SSID()==name) WiFi.disconnect(false,false);
      manual = true;
      feedback = "已忘记该网络。";
    } else feedback = "未能删除该网络，请刷新后重试。";
    redirect();
  });
  server.on("/done",HTTP_POST,[&]() {
    server.send(200,"text/plain; charset=utf-8","已结束配网，可以断开天气钟热点。");
    done = true;
  });
  server.begin();
  publishCode(code);
  Serial.println("CLOCK_SETUP_AP_READY ssid=WeatherClock-62A4 ip=192.168.4.1");
  while (!done && !shouldClose()) {
    server.handleClient();
    if (done || shouldClose()) break;
    const uint32_t now = millis();
    if (join == Join::Disconnecting && WiFi.status()!=WL_CONNECTED) {
      WiFi.begin(target.c_str(),password.c_str());
      join = Join::Connecting;
      phaseAt = now;
    } else if (join == Join::Connecting && WiFi.status()==WL_CONNECTED && WiFi.SSID()==target) {
      if (save()) {
        feedback = "已连接并保存，临时热点即将关闭。";
        join = Join::Saved;
        phaseAt = now;
      } else {
        WiFi.disconnect(false,false);
        join = Join::Idle;
        manual = true;
      }
      password = "";
    } else if (pending() && now-phaseAt>=15000) {
      WiFi.disconnect(false,false);
      password = "";
      join = Join::Idle;
      manual = true;
      feedback = "未连接成功。请检查网络名称和密码，或选择仅保存。";
    } else if (join==Join::Saved && now-phaseAt>=5000) {
      break;
    } else if (join==Join::Idle) {
      if (!manual && WiFi.status()==WL_CONNECTED) break;
      if (WiFi.status()!=WL_CONNECTED) roaming.tick(profiles);
    }
    if (manual && now-lastActivity>=180000) break;
    delay(8);
  }
  if (pending()) WiFi.disconnect(false,false);
  roaming.reset();
  const char cleared[9] = {};
  publishCode(cleared);
  if (done) delay(200);
  server.stop();
  WiFi.softAPdisconnect(true);
  return WiFi.status() == WL_CONNECTED;
}

