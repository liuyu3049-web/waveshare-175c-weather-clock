#pragma once

#include <Arduino_GFX_Library.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include "face_type.h"
#include "clock_sky.h"
#include "clock_touch.h"
#include "clock_battery.h"
#include "weather_data.h"

namespace clock_face {
using namespace face_type;
constexpr int size = 466, center = size / 2;
static Arduino_ESP32QSPI bus(12, 38, 4, 5, 6, 7);
static Arduino_CO5300 panel(&bus, 2, 0, size, size, 6, 0, 0, 0);
static Arduino_Canvas canvas(size, size, &panel);
static uint16_t* fb;

constexpr uint16_t rgb(int r, int g, int b) {
  return ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3);
}
constexpr uint16_t paper = rgb(249, 247, 237), silver = rgb(177, 201, 209);
constexpr uint16_t warm = rgb(255, 203, 132), aqua = rgb(132, 220, 228);
constexpr uint16_t rail = rgb(55, 78, 90), dark = rgb(16, 32, 44);
static const char* const weekdays[] = {"周日","周一","周二","周三","周四","周五","周六"};

static void label(int x, int y, const char* s, Size sz = Text, uint16_t color = paper, Align align = Center) {
  draw(fb, x, y, s, sz, color, align);
}

// A round-ended stroke also draws circles when its endpoints coincide.
static void stroke(float x, float y, float ex, float ey, float r, uint16_t color) {
  int left = (int)floorf(fminf(x, ex) - r), right = (int)ceilf(fmaxf(x, ex) + r);
  int top = (int)floorf(fminf(y, ey) - r), bottom = (int)ceilf(fmaxf(y, ey) + r);
  float dx = ex - x, dy = ey - y, len2 = dx * dx + dy * dy;
  for (int row = top < 0 ? 0 : top; row <= bottom && row < size; ++row) {
    for (int col = left < 0 ? 0 : left; col <= right && col < size; ++col) {
      float t = len2 ? fminf(1, fmaxf(0, ((col-x)*dx + (row-y)*dy) / len2)) : 0;
      float u = col-x-t*dx, v = row-y-t*dy;
      float coverage = r + 0.5f - sqrtf(u*u + v*v);
      if (coverage <= 0) continue;
      uint16_t& pixel = fb[row*size + col];
      pixel = coverage >= 1 ? color : mix(pixel, color, (unsigned)(15*coverage + 0.5f));
    }
  }
}
static void disc(float x, float y, float r, uint16_t c) { stroke(x,y,x,y,r,c); }

static const char* condition(int code) {
  if (code == 0) return "晴";
  if (code == 1) return "少云";
  if (code == 2) return "多云";
  if (code == 3) return "阴";
  if (code == 45 || code == 48) return "雾";
  if (code == 56 || code == 57 || code == 66 || code == 67) return "冻雨";
  if (code >= 51 && code <= 55) return "毛毛雨";
  if (code == 61) return "小雨";
  if (code == 63) return "中雨";
  if (code == 65) return "大雨";
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "雪";
  if (code >= 80 && code <= 82) return "阵雨";
  if (code >= 95) return "雷雨";
  return "天气";
}

static void weatherIcon(int x, int y, float s, int wmo) {
  if (wmo <= 2) {
    float sx = x + (wmo ? 5*s : 0), sy = y - (wmo ? 6*s : 0);
    for (int i = 0; i < 8; ++i) {
      float a = i * 0.7853982f, dx = cosf(a)*s, dy = sinf(a)*s;
      stroke(sx + 10*dx, sy + 10*dy, sx + 13*dx, sy + 13*dy, 1.15f*s, warm);
    }
    disc(sx,sy,7*s,warm);
    if (!wmo) return;
  }
  bool wet = wmo >= 51;
  float cy = y - (wet ? 3*s : 0);
  uint16_t cloud = wmo < 3 ? paper : silver;
  disc(x-6*s,cy,5*s,cloud);
  disc(x,cy-3*s,7*s,cloud);
  disc(x+7*s,cy,4.5f*s,cloud);
  stroke(x-7*s,cy+2*s,x+7*s,cy+2*s,3*s,cloud);
  if (wmo == 45 || wmo == 48) {
    stroke(x-8*s,y+8*s,x+7*s,y+8*s,s,aqua);
    stroke(x-4*s,y+12*s,x+9*s,y+12*s,s,aqua);
  } else if (wmo >= 95) {
    stroke(x+2*s,y+3*s,x-2*s,y+9*s,1.7f*s,warm);
    stroke(x-2*s,y+9*s,x+2*s,y+9*s,1.7f*s,warm);
    stroke(x+2*s,y+9*s,x-2*s,y+15*s,1.3f*s,warm);
  } else if ((wmo >= 71 && wmo <= 77) || wmo == 85 || wmo == 86) {
    for (int i = -1; i <= 1; ++i) disc(x+i*6*s,y+(i ? 7 : 10)*s,1.5f*s,paper);
  } else if (wet) {
    for (int i = -1; i <= 1; ++i)
      stroke(x+i*6*s,y+5*s,x+(i*6-2)*s,y+10*s,s,aqua);
  }
}

static void forecastBackground() {
  for (int y = 0; y < size; ++y) {
    // Dither the gradient before reducing it to the panel's RGB565 depth.
    static constexpr uint8_t dither[] = {0,4,6,2,5,1,3,7};
    int light = 466 - y;
    uint16_t rowColors[8];
    for (int j = 0; j < 8; ++j) {
      int d = dither[(j + 3*(y & 7)) & 7];
      rowColors[j] = rgb(9+light*17/466+d,20+light*23/466+(d>>1),30+light*27/466+d);
    }
    for (int x = 0; x < size; ++x) fb[y*size+x] = rowColors[x & 7];
  }
}

static void pager(int page, uint16_t active = paper, uint16_t inactive = rail) {
  for (int i = 0; i < 3; ++i) {
    int x = center + 19*(i-1);
    if (i == page) stroke(x-4,419,x+4,419,2.5f,active);
    else disc(x,419,2.3f,inactive);
  }
}

static void batteryFooter(const ClockBattery& battery, uint16_t ink, int top = 433, int anchorX = center) {
  char text[8] = "--%";
  const bool usbOnly = battery.valid && !battery.present && battery.usb;
  if (usbOnly) snprintf(text,sizeof(text),"USB");
  else if (battery.percent >= 0) snprintf(text,sizeof(text),"%d%%",battery.percent);
  const int x = anchorX-(28+measure(text,Small)+1)/2;
  if (usbOnly) {
    stroke(x+5,top+4,x+5,top+8,1.0f,ink);
    stroke(x+12,top+4,x+12,top+8,1.0f,ink);
    stroke(x+4,top+9,x+13,top+9,1.7f,ink);
    stroke(x+6,top+12,x+11,top+12,1.7f,ink);
    stroke(x+9,top+13,x+9,top+17,1.0f,ink);
  } else {
    stroke(x+1,top+5,x+18,top+5,.85f,ink);
    stroke(x+1,top+15,x+18,top+15,.85f,ink);
    stroke(x,top+6,x,top+14,.85f,ink);
    stroke(x+19,top+6,x+19,top+14,.85f,ink);
    stroke(x+21,top+8,x+21,top+12,.85f,ink);
    if (battery.percent > 0)
      for (int y=top+8; y<=top+12; ++y) stroke(x+3,y,x+3+13*battery.percent/100.f,y,.7f,ink);
  }
  label(x+28,top,text,Small,ink,Left);
}

static void analogDial(const tm* t) {
  constexpr int cy = 169, r = 93;
  for (int i = 0; i < 60; ++i) {
    float a = i * 0.104719755f, dx = sinf(a), dy = -cosf(a);
    bool hour = i%5 == 0;
    int depth = hour ? 9 : 3;
    stroke(center+(r-depth)*dx, cy+(r-depth)*dy, center+r*dx, cy+r*dy,
           hour ? 1.8f : 0.8f, hour ? paper : silver);
  }
  label(center,cy-71,"12",Small); label(center,cy+53,"6",Small);
  label(center-62,cy-10,"9",Small); label(center+62,cy-10,"3",Small);
  if (!t) { label(center,cy-11,"--:--",Text); return; }
  for (int hand = 0; hand < 2; ++hand) {
    float angle = hand ? t->tm_min * 0.104719755f :
                    ((t->tm_hour%12) + t->tm_min/60.0f) * 0.52359878f;
    float dx = sinf(angle), dy = -cosf(angle), length = hand ? 72 : 46;
    stroke(center-7*dx,cy-7*dy,center+dx*length,cy+dy*length,
           hand ? 2.3f : 3.8f,hand ? warm : paper);
  }
  disc(center,cy,4.8f,paper); disc(center,cy,2.2f,dark);
}

static void home(bool analog, const tm* t, const WeatherData* d, const char* status, bool stale, bool hint) {
  drawClockSky(fb, t && (t->tm_hour < 6 || t->tm_hour >= 19), d ? d->wmo : 3);
  char s[96];
  if (analog && t) {
    snprintf(s,sizeof(s),"成都  ·  %d月%d日  %s",t->tm_mon+1,t->tm_mday,weekdays[t->tm_wday]);
    label(center,45,s,Small,paper);
  } else label(center,analog ? 37 : 69,"成 都",Text,paper);
  if (analog) analogDial(t);
  else {
    if (t) snprintf(s,sizeof(s),"%02d:%02d",t->tm_hour,t->tm_min);
    else snprintf(s,sizeof(s),"--:--");
    label(center,108,s,Large);
  }
  if (t && !analog) {
    snprintf(s,sizeof(s),"%d月%d日   %s",t->tm_mon+1,t->tm_mday,weekdays[t->tm_wday]);
    label(center,215,s,Small,paper);
  }
  if (d) {
    snprintf(s,sizeof(s),"%.0f°",d->temperature);
    const char* word = condition(d->wmo);
    int width = 39 + 11 + measure(s,Medium) + 12 + measure(word,Text);
    int x = center - width/2;
    weatherIcon(x+19,341,1.35f,d->wmo);
    label(x+50,322,s,Medium,paper,Left);
    label(x+50+measure(s,Medium)+12,330,word,Text,paper,Left);
    snprintf(s,sizeof(s),"湿度 %u%%    %.0f° / %.0f°",d->humidity,d->daily[0].low,d->daily[0].high);
    label(center,365,s,Small,paper);
  } else {
    label(center,333,status,Text);
  }
  if (stale) label(center,392,"上次更新",Small,warm);
  else if (hint) label(center,392,"双击切换表盘  ·  左右滑动",Small,silver);
}

static void hourly(const WeatherData& d) {
  constexpr int x0 = 78, step = 62, floorY = 245;
  float lo = d.hourly[0].temperature, hi = lo;
  for (const auto& h : d.hourly) { lo = fminf(lo,h.temperature); hi = fmaxf(hi,h.temperature); }
  int ys[6];
  for (int i = 0; i < 6; ++i)
    ys[i] = 205 - lroundf(59 * (d.hourly[i].temperature - (hi+lo)/2) / fmaxf(hi-lo,2));
  for (int x = x0; x <= x0+5*step; ++x) {
    int i = (x-x0)/step; if (i == 5) i = 4;
    int y = ys[i] + (ys[i+1]-ys[i])*(x-x0-i*step)/step;
    for (int row = y; row <= floorY; ++row) {
      uint16_t& px = fb[row*size+x];
      px = mix(px,aqua,1+3*(floorY-row)/(1+floorY-y));
    }
  }
  stroke(x0,floorY,x0+5*step,floorY,0.7f,rail);
  char s[24];
  for (int i = 0; i < 6; ++i) {
    int x = x0+i*step;
    if (i < 5) stroke(x,ys[i],x+step,ys[i+1],2.1f,aqua);
    disc(x,ys[i],4.7f,aqua); disc(x,ys[i],2.2f,dark);
    snprintf(s,sizeof(s),"%.0f°",d.hourly[i].temperature);
    label(x,ys[i]-33,s,Text,paper);
    snprintf(s,sizeof(s),"%02d时",d.hourly[i].hour);
    label(x,254,s,Small,silver);
    snprintf(s,sizeof(s),"%u%%",d.hourly[i].rainChance);
    label(x,343,s,Small,d.hourly[i].rainChance ? aqua : silver);
  }
  stroke(98,296,368,296,0.6f,rail);
  label(center,307,"降水概率",Small,aqua);
}

static void daily(const WeatherData& d, const tm* t, bool stale) {
  float lo = d.daily[0].low, hi = d.daily[0].high;
  for (const auto& day : d.daily) { lo = fminf(lo,day.low); hi = fmaxf(hi,day.high); }
  char s[24];
  for (int i = 0; i < 5; ++i) {
    const auto& day = d.daily[i];
    int y = 132 + 48*i;
    bool current = !stale && t && t->tm_wday == d.daily[0].weekday;
    const char* dayName = current && i == 0 ? "今天" : current && i == 1 ? "明天" : weekdays[day.weekday];
    label(85,y,dayName,Text,i == 0 ? warm : paper,Left);
    weatherIcon(161,y+12,0.94f,day.wmo);
    label(183,y+3,condition(day.wmo),Small,paper,Left);
    snprintf(s,sizeof(s),"%.0f°",day.low); label(267,y+2,s,Small,silver,Right);
    const int start = 281, end = 332;
    stroke(start,y+13,end,y+13,2.5f,rail);
    float span = fmaxf(1,hi-lo);
    float a = start+(end-start)*(day.low-lo)/span, b = start+(end-start)*(day.high-lo)/span;
    stroke(a,y+13,b,y+13,2.8f,warm);
    snprintf(s,sizeof(s),"%.0f°",day.high); label(345,y+2,s,Small,paper,Left);
    if (i < 4) stroke(88,y+36,378,y+36,0.56f,rail);
  }
}
}  // namespace clock_face

#include "selected_faces.h"
#include "reference_art.h"
#include "reference_complications.h"
#include "reference_hands.h"
#include "lagoon_render.h"

inline bool beginClockFace(uint8_t brightness = 108) {
  using namespace clock_face;
  if (!canvas.begin()) return false;
  fb = canvas.getFramebuffer();
  panel.setBrightness(brightness);
  return fb != nullptr;
}

inline void drawWeatherPage(int page, uint8_t face, const tm* local, const WeatherData* data,
                            const char* status, bool stale, bool hint, const ClockBattery* battery = nullptr,
                            float seconds = 0) {
  using namespace clock_face;
  if (!fb) return;
  if (page == 0 && face == ClockPeacockAnalog) {
    drawLagoonPage(local,data,status,stale,hint,battery,seconds);
    return;
  }
  if (page == 0 && face >= ClockExplorer && face < ClockFaceCount) {
    drawWatchPage(face,local,seconds);
    return;
  }
  lagoonOnPanel = false;
  if (page == 0) {
    uint16_t ink = silver, active = paper, inactive = rail;
    if (face == ClockRetroFlip) {
      flipFace(local,data,status,stale,hint);
      active = ink = rgb(183,180,167);
      inactive = rgb(74,73,68);
    } else home(face == ClockMountainAnalog,local,data,status,stale,hint);
    pager(page,active,inactive);
    if (battery) batteryFooter(*battery,ink);
  } else {
    forecastBackground();
    label(center,56,page == 1 ? "逐时预报" : "五日预报",Text,warm);
    label(center,93,page == 1 ? "成都  /  未来 6 小时" : "成都  /  最低温 · 最高温",Small,silver);
    if (!data) label(center,210,status);
    else if (page == 1) hourly(*data);
    else daily(*data,local,stale);
    if (stale) label(center,384,"上次更新",Small,warm);
    pager(page);
  }
  canvas.flush();
}

inline void drawClockSettings(const ClockDisplayOptions& options) {
  using namespace clock_face;
  using namespace clock_settings;
  if (!fb) return;
  lagoonOnPanel = false;
  forecastBackground();
  label(center,55,"设置",Text,warm);
  label(88,111,"亮度",Text,paper,Left);
  char pct[8];
  snprintf(pct,sizeof(pct),"%u%%",(options.brightness()*100u+127)/255);
  label(378,115,pct,Small,aqua,Right);
  const int selectedX = levelStartX + options.level*levelStep;
  stroke(levelStartX,levelY,levelStartX+(levelCount-1)*levelStep,levelY,2,rail);
  stroke(levelStartX,levelY,selectedX,levelY,2,aqua);
  for (int i=0; i<levelCount; ++i)
    disc(levelStartX+i*levelStep,levelY,3,i<=options.level ? aqua : silver);
  disc(selectedX,levelY,9,aqua);
  disc(selectedX,levelY,4,dark);
  label(88,201,"自动息屏",Text,paper,Left);
  for (int i=0; i<timeCount; ++i) {
    const int x = timeStartX+i*timeStep;
    if (i == options.timeout) stroke(x-13,timeY,x+13,timeY,17,aqua);
    label(x,timeY-9,times[i],Small,i == options.timeout ? dark : silver);
  }
  stroke(84,289,382,289,.65f,rail);
  label(88,317,"手机配网",Text,paper,Left);
  stroke(361,319,369,327,1.4f,silver);
  stroke(369,327,361,335,1.4f,silver);
  label(center,391,"返回",Small,aqua);
  canvas.flush();
}

inline void drawClockWifiSetup(const char* ssid, const char* code) {
  using namespace clock_face;
  if (!fb) return;
  lagoonOnPanel = false;
  forecastBackground();
  label(center,67,"手机配网",Text,warm);
  label(center,125,"连接热点",Small,silver);
  label(center,153,ssid);
  label(center,209,"热点密码",Small,silver);
  label(center,238,code,Text);
  label(center,303,"浏览器打开",Small,silver);
  label(center,334,"192.168.4.1",Text);
  label(center,394,"轻点返回",Small,silver);
  canvas.flush();
}

inline void drawClockPowerConfirm() {
  using namespace clock_face;
  using namespace clock_power_buttons;
  if (!fb) return;
  lagoonOnPanel = false;
  forecastBackground();
  constexpr int cy = 126, r = 23;
  disc(center,cy,44,rail);
  disc(center,cy,43,dark);
  float x = 0, y = 0;
  for (int i = 0; i <= 32; ++i) {
    float angle = (9*i - 54)*(PI/180.0f);
    float nx = center + r*cosf(angle), ny = cy + r*sinf(angle);
    if (i) stroke(x,y,nx,ny,2.5f,warm);
    x = nx; y = ny;
  }
  stroke(center,cy-28,center,cy-3,2.5f,warm);
  label(center+9,190,"关机？",Medium); // Center the ink of the full-width question mark.
  stroke(cancelX-halfSpan,centerY,cancelX+halfSpan,centerY,radius,rail);
  stroke(cancelX-halfSpan,centerY,cancelX+halfSpan,centerY,radius-1.5f,dark);
  stroke(confirmX-halfSpan,centerY,confirmX+halfSpan,centerY,radius,warm);
  label(cancelX,centerY-12,"取消",Text,paper);
  label(confirmX,centerY-12,"关机",Text,dark);
  canvas.flush();
}
