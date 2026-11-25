#pragma once

namespace clock_face {

static const char* faceNote(const char* status, bool stale, bool hint) {
  if (stale) return status && !strcmp(status,"等待联网") ? "离线  ·  上次更新" : "上次更新";
  return hint ? "双击切换  ·  左右滑动" : "";
}

constexpr int lagoonCx = 233, lagoonCy = 229, lagoonSecondTip = 183, lagoonSecondTail = 29;
constexpr int lagoonMinuteTip = 156;
constexpr uint16_t lagoonSteel = rgb(153,177,182), lagoonPearl = rgb(238,245,232);
constexpr uint16_t lagoonShadow = rgb(3,23,36);
constexpr float lagoonStep = 0.104719755f;

#include "lagoon_ground.h"

static void lagoonGround() {
  // Paired rows and the four-pixel dither repeat across much of the brushed dial.
  static constexpr int back[] = {0,1,2,4,size-2,size+2,2*size,4*size};
  const unsigned char* pack = lagoonGroundPixels;
  uint16_t* dst = fb;
  while (dst < fb+size*size) {
    unsigned command = *pack++, count = 1+(command&31);
    int distance = back[command>>5];
    if (distance) {
      const uint16_t* from = dst-distance;
      while (count--) *dst++ = *from++;
    } else {
      while (count--) *dst++ = lagoonGroundPalette[*pack++];
    }
  }
}

// Every applique has a bevel, two polished planes and a small cast shadow.
static void lagoonFacet(float angle, float base, float crest, float shoulder, float tip,
                        float tailWidth, float fullWidth, float neckWidth, float offset) {
    constexpr int cx = lagoonCx, cy = lagoonCy;
    constexpr uint16_t glint = rgb(255,255,243);
    float dx = sinf(angle), dy = -cosf(angle), tx = -dy, ty = dx;
    float ox = cx+offset*tx, oy = cy+offset*ty;
    float bx = ox+base*dx, by = oy+base*dy, ex = ox+tip*dx, ey = oy+tip*dy;
    int left = floorf(fminf(bx,ex)-fullWidth-1), right = ceilf(fmaxf(bx,ex)+fullWidth+2);
    int top = floorf(fminf(by,ey)-fullWidth-1), bottom = ceilf(fmaxf(by,ey)+fullWidth+3);
    float sign = tip>base ? 1 : -1, a = (crest-base)*sign;
    float b = (shoulder-base)*sign, span = (tip-base)*sign;
    float rise = (fullWidth-tailWidth)/a, slope = (neckWidth-fullWidth)/(b-a);
    float taper = neckWidth/(span-b);
    bool brightRight = -0.56f*tx-0.83f*ty > 0;
    uint16_t bright = lagoonPearl, dim = rgb(121,152,162);
    auto widthAt = [&](float u) {
      return u<a ? tailWidth+u*rise : u<b ? fullWidth+(u-a)*slope : (span-u)*taper;
    };
    bool trimRows = fabsf(dy) > 0.12f;
    float rowSlope = trimRows ? dx/dy : 0;
    float rowPad = trimRows ? (fullWidth+3)/fabsf(dy) : 0;
    for (int y = top; y <= bottom; ++y) {
      float axisX = ox+(y-oy)*rowSlope;
      int from = trimRows ? (int)fmaxf(left,floorf(axisX-rowPad)) : left;
      int to = trimRows ? (int)fminf(right,ceilf(axisX+rowPad)) : right;
      for (int x = from; x <= to; ++x) {
        if (y>=408 && abs(x-cx)<43) continue; // Opening in the chapter ring for the footer.
        float u = ((x-ox)*dx+(y-oy)*dy-base)*sign;
        float v = (x-ox)*tx+(y-oy)*ty;
        uint16_t& pixel = fb[y*size+x];
        float su = u-(dx+1.8f*dy)*sign, sv = v-tx-1.8f*ty;
        if (su>0 && su<span && fabsf(sv)<widthAt(su)) pixel = mix(pixel,lagoonShadow,8);
        float side = widthAt(u)-fabsf(v);
        float edge = fminf(side,fminf(u,span-u)) + 0.5f;
        if (edge <= 0) continue;
        bool lit = (v>=0)==brightRight;
        uint16_t metal = lit ? bright : dim;
        if (fabsf(v)<0.58f) metal = glint;
        else if (side<0.9f) metal = lit ? glint : rgb(66,95,108);
        pixel = edge>=1 ? metal : mix(pixel,metal,unsigned(edge*15+0.5f));
      }
    }
}

// The edge of the raised numeral casts a one-pixel shadow. The polished face
// changes tone across its height, letting the small numerals read on both lobes.
static void lagoonNumeral(int hour) {
  float angle = (hour%12)*5*lagoonStep;
  int radius = hour == 6 ? 223 : 221;
  int x = center+lroundf(radius*sinf(angle)), y = center-lroundf(radius*cosf(angle))-5;
  char s[3];
  snprintf(s,sizeof(s),"%d",hour);
  static constexpr uint16_t tones[] = {
    rgb(244,244,222),rgb(217,227,212),rgb(189,205,200),rgb(151,173,177),
    rgb(195,215,211),rgb(227,232,212),rgb(178,195,188)
  };
  constexpr int width=8, height=11;
  x -= strlen(s)*width/2;
  for (const char* digit=s; *digit; ++digit) {
    const Glyph& glyph = *find(fonts[Small],*digit);
    const uint8_t* runs = fonts[Small].runs+glyph.offset;
    uint8_t mask[15][13] = {};
    for (int row=0; row<glyph.height; ++row) {
      for (int col=0; col<glyph.width;) {
        int run = *runs++, stop = col+(run&15)+1, alpha = run>>4;
        for (; col<stop; ++col) mask[row+1][col+glyph.left+1]=alpha;
      }
    }
    uint8_t small[height][width];
    for (int row=0; row<height; ++row) {
      float sy = .5f+(row+.5f)*glyph.height/height;
      int iy=int(sy);
      for (int col=0; col<width; ++col) {
        float sx = .5f+(col+.5f)*glyph.advance/width;
        int ix=int(sx);
        float u=sx-ix, v=sy-iy;
        small[row][col]=lroundf((1-v)*((1-u)*mask[iy][ix]+u*mask[iy][ix+1])+
                                   v*((1-u)*mask[iy+1][ix]+u*mask[iy+1][ix+1]));
        uint16_t& shadow=fb[(y+row+1)*size+x+col+1];
        shadow=mix(shadow,lagoonShadow,small[row][col]);
      }
    }
    for (int row=0; row<height; ++row) for (int col=0; col<width; ++col) {
      uint16_t& pixel=fb[(y+row)*size+x+col];
      pixel=mix(pixel,tones[row*7/height],small[row][col]);
    }
    x+=width;
  }
}

inline void lagoonBase() {
  if (!fb) return;
  constexpr int cx = lagoonCx, cy = lagoonCy;
  lagoonGround();
  for (int hour=1; hour<=12; ++hour) lagoonNumeral(hour);
  for (int i = 0; i < 60; ++i) {
    float a = i*lagoonStep, dx = sinf(a), dy = -cosf(a);
    bool hour = i%5==0;
    float inside = hour ? 202 : 207, outside = hour ? 207 : 212;
    if (i==55) { inside=201; outside=204; }  // Clear the inner corner of the two-digit 11.
    stroke(cx+inside*dx,cy+inside*dy,cx+outside*dx,cy+outside*dy,
           hour ? 1.15f : 0.65f,hour ? lagoonPearl : lagoonSteel);
    if (!hour || i==30) continue;  // Keep the raised hour marker clear of the page dots.
    if (i==0) {
      lagoonFacet(a,199,197,164,159,2.4f,3.9f,3.1f,-5.1f);
      lagoonFacet(a,199,197,164,159,2.4f,3.9f,3.1f,5.1f);
    } else {
      int end = i==25 || i==35 ? 178 : 165;
      lagoonFacet(a,198,196,end+5,end,2.3f,4.8f,3.5f,0);
    }
  }

  // The date sits below a two-tone chamfer, like an aperture cut in the dial.
  constexpr int wx = 353, wy = 229, ww = 31, wh = 14;
  for (int y = -wh; y <= wh; ++y) {
    for (int x = -ww; x <= ww; ++x) {
      uint16_t c = rgb(8,39+(y+wh)/4,52+(y+wh)/4);
      if (x==-ww || y==-wh) c = rgb(5,29,39);
      else if (x==ww || y==wh) c = lagoonSteel;
      else if (x==ww-1 || y==wh-1) c = rgb(87,123,127);
      fb[(wy+y)*size+wx+x] = c;
    }
  }
}

constexpr int lagoonNoteTop = 369;
struct LagoonTextBounds { int left, top, right, bottom, noteWidth; };
inline void lagoonFace(const tm* t, const WeatherData* w, const char* status, bool stale, bool hint,
                       const ClockBattery* battery = nullptr, LagoonTextBounds* textBounds = nullptr) {
  if (!fb) return;
  constexpr int cx = lagoonCx, cy = lagoonCy;
  constexpr uint16_t steel = lagoonSteel, pearl = lagoonPearl;
  char s[40];
  if (t) {
    snprintf(s,sizeof(s),"%d.%02d",t->tm_mon+1,t->tm_mday);
    label(353,cy-9,s,Small,pearl);
  } else label(353,cy-9,"--.--",Small,steel);
  const char* word = w ? condition(w->wmo) : "";
  if (w) snprintf(s,sizeof(s),"%.0f°  ",w->temperature);
  else snprintf(s,sizeof(s),"%s",status && *status ? status : "暂无数据");
  const float hourAngle = t ? ((t->tm_hour%12)+t->tm_min/60.0f)*5*lagoonStep : 0;
  const float minuteAngle = t ? t->tm_min*lagoonStep : 0;
  const char* note = faceNote(status,stale,hint);
  const int noteWidth = measure(note,Small);
  const int iconSpace = w ? 30 : 0;
  const int readingWidth = measure(s,Text);
  const int cityWidth = measure("成 都",Small), weatherWidth = iconSpace+readingWidth+measure(word,Condition);
  constexpr int maxBatteryWidth = 78;  // Fits the icon and the longest label, 100%.
  auto blocked = [&](int x, int top, float angle, int length, int pad) {
    const float dx = sinf(angle), dy = -cosf(angle);
    auto crosses = [&](int width, int y, int height) {
      float near = 0, far = length;
      auto clip = [&](float direction, float low, float high) {
        if (fabsf(direction)<.0001f) return low<=0 && high>=0;
        const float a = low/direction, b = high/direction;
        near = fmaxf(near,fminf(a,b));
        far = fminf(far,fmaxf(a,b));
        return near<=far;
      };
      return clip(dx,x-cx-width/2-pad,x-cx+width/2+pad) && clip(dy,y-cy-pad,y+height-cy+pad);
    };
    return crosses(cityWidth,top-7,17) || crosses(weatherWidth,top+20,30) ||
           (battery && crosses(maxBatteryWidth,top+55,20));
  };
  static constexpr int positions[][2] = {{cx,119},{cx,287},{141,202},{123,148},{344,134}};
  int infoX = cx, infoY = 119;
  for (const auto& position : positions) {
    infoX = position[0]; infoY = position[1];
    if (!t || (!blocked(infoX,infoY,hourAngle,123,8) && !blocked(infoX,infoY,minuteAngle,lagoonMinuteTip,4))) break;
  }
  if (textBounds) {
    int width = weatherWidth > cityWidth ? weatherWidth : cityWidth;
    if (battery && width < maxBatteryWidth) width = maxBatteryWidth;
    const int half = (width+1)/2 + 4;
    *textBounds = {infoX-half,infoY-10,infoX+half,infoY+(battery ? 75 : 53),noteWidth};
  }
  label(infoX,infoY-8,"成 都",Small,rgb(195,215,206));
  const int left = infoX-weatherWidth/2;
  if (w) weatherIcon(left+11,infoY+38,.82f,w->wmo);
  label(left+iconSpace,infoY+25,s,Text,pearl,Left);
  if (w) label(left+iconSpace+readingWidth,infoY+28,word,Condition,pearl,Left);
  if (battery) batteryFooter(*battery,rgb(185,216,207),infoY+55,infoX);
  if (noteWidth) label(cx,lagoonNoteTop,note,Small,stale ? warm : steel);

  if (!t) return;
  lagoonFacet(hourAngle,-12,24,78,123,2.8f,9.7f,5.0f,0);
  lagoonFacet(minuteAngle,-15,18,126,lagoonMinuteTip,2.1f,4.8f,2.0f,0);
  disc(cx+0.6f,cy+1.2f,8.2f,lagoonShadow);
  disc(cx,cy,7.4f,steel);
  disc(cx-0.6f,cy-0.7f,5.3f,pearl);
  disc(cx,cy,2.7f,rgb(48,84,96));
}

inline void lagoonSecondHand(float seconds) {
  float angle = seconds*lagoonStep, dx = sinf(angle), dy = -cosf(angle);
  bool horizontal = fabsf(dx)>fabsf(dy);
  float major = horizontal ? dx : dy, minor = horizontal ? dy : dx;
  float slope = minor/major;
  float first = -lagoonSecondTail*major, last = lagoonSecondTip*major;
  int start = floorf(fminf(first,last))-2, stop = ceilf(fmaxf(first,last))+2;
  // Scan along the needle, including its counterweight, in both orientations.
  // A single antialiased silhouette stays connected through every angle.
  for (int i=start; i<=stop; ++i) {
    int middle = lroundf(i*slope);
    for (int j=middle-3; j<=middle+3; ++j) {
      int x = horizontal ? i : j, y = horizontal ? j : i;
      float u=x*dx+y*dy, v=-x*dy+y*dx;
      float width = u < -13 ? 1.8f : 0.98f-0.20f*fmaxf(0,u)/lagoonSecondTip;
      float edge = .5f+fminf(width-fabsf(v),fminf(u+lagoonSecondTail,lagoonSecondTip-u));
      if (edge<=0) continue;
      uint16_t color = v>.38f ? rgb(196,135,78) : rgb(255,210,148);
      uint16_t& pixel = fb[(lagoonCy+y)*size+lagoonCx+x];
      pixel = edge>=1 ? color : mix(pixel,color,unsigned(edge*15+.5f));
    }
  }
  disc(lagoonCx,lagoonCy,3.0f,rgb(177,126,79));
  disc(lagoonCx-.3f,lagoonCy-.4f,1.8f,rgb(255,223,168));
}

inline void flipFace(const tm* t, const WeatherData* w, const char* status, bool stale, bool hint) {
  if (!fb) return;
  const uint16_t ink = rgb(244, 231, 200), quiet = rgb(182, 176, 161);
  const uint16_t amber = rgb(217, 155, 79), gap = rgb(12, 13, 12);

  static constexpr int grain[4][4] = {{0,4,1,5},{6,2,7,3},{1,5,0,4},{7,3,6,2}};
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      int dx = x - center, dy = y - 160;
      int shade = 25 - (dx*dx + dy*dy) / 10500 + grain[y&3][x&3];
      fb[y*size+x] = rgb(shade + 1, shade + 1, shade);
    }
  }

  if (t) {
    char date[56];
    snprintf(date, sizeof(date), "%d月%d日  %s", t->tm_mon+1, t->tm_mday, weekdays[t->tm_wday]);
    int start = center - (measure("成都", Text) + 28 + measure(date, Small)) / 2;
    label(start, 76, "成都", Text, ink, Left);
    int divider = start + measure("成都", Text) + 14;
    stroke(divider, 81, divider, 96, 0.7f, amber);
    label(divider + 14, 81, date, Small, quiet, Left);
  } else label(center,76,"成都",Text,ink);

  auto plate = [&](int x0, int x1, int top, int bottom, int r, uint16_t upper, uint16_t lower) {
    for (int y = top; y <= bottom; ++y) {
      float d = fmaxf(0, fmaxf(top + r - y, y - (bottom - r)));
      float edge = r - sqrtf(r*r - d*d);
      int inset = (int)ceilf(edge);
      uint16_t color = y <= 205 ? upper : lower;
      uint16_t* row = fb + y*size;
      for (int x = x0 + inset; x <= x1 - inset; ++x) row[x] = color;
      if (inset) {
        unsigned alpha = (unsigned)((inset-edge)*15);
        int l = x0+inset-1, h = x1-inset+1;
        row[l] = mix(row[l], color, alpha); row[h] = mix(row[h], color, alpha);
      }
    }
  };

  for (int i = 0; i < 2; ++i) {
    int left = i ? 241 : 41, right = i ? 425 : 225;
    int mid = (left + right) / 2;
    plate(left-3,right+3,126,289,16, gap,gap);
    plate(left,right,128,285,13, rgb(96,94,84),rgb(67,66,60));
    plate(left+1,right-1,130,281,12, rgb(64,63,58),rgb(46,46,42));
    char digits[8] = "--";
    if (t) snprintf(digits, sizeof(digits), "%02d", i ? t->tm_min : t->tm_hour);
    // Thicken the existing numeral outlines to match ink on moulded flip leaves.
    for (int offset = -3; offset <= 3; offset += 3) {
      label(mid+offset, 157, digits, Large, ink);
      label(mid+offset, 159, digits, Large, ink);
    }
    stroke(left+1,205,right-1,205,0.65f,gap);
    stroke(left+1,206,right-1,206,0.69f,gap);
    for (int side = 0; side < 2; ++side) {
      int x = side ? right-4 : left+4;
      stroke(x,200,x,212,2.0f,gap);
      stroke(x,202,x,209,0.78f,rgb(132,129,114));
    }
  }
  disc(center,189,1.65f,amber);
  disc(center,222,1.65f,amber);

  if (w) {
    char temp[16], range[48];
    snprintf(temp, sizeof(temp), "%.0f°", w->temperature);
    const char* word = condition(w->wmo);
    int start = center - (34 + 9 + measure(temp,Medium) + 13 + measure(word,Text)) / 2;
    weatherIcon(start+16,333,1.12f,w->wmo);
    label(start+43,314,temp,Medium,ink,Left);
    label(start+43+measure(temp,Medium)+13,324,word,Text,ink,Left);
    snprintf(range,sizeof(range),"最低 %.0f°    最高 %.0f°",w->daily[0].low,w->daily[0].high);
    label(center,365,range,Small,quiet);
  } else label(center,324,status && *status ? status : "暂无数据",Text,quiet);
  const char* note = faceNote(status,stale,hint);
  if (*note) label(center,391,note,Small,stale ? amber : quiet);
}

}  // namespace clock_face
