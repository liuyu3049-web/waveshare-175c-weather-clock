#pragma once

namespace clock_face {
constexpr size_t lagoonFrameBytes = size * size * sizeof(uint16_t);
static uint16_t *lagoonDial, *lagoonUnderlay;
static bool lagoonOnPanel;
static float lastSweep = -1;
static uint32_t lastSweepPixels;

struct WatchDial {
  void (*second)(float);
  int cx, cy, tail, tip, pad;
};
static constexpr WatchDial watchDials[] = {
  {referenceSecondsBlack, 235,227,4,218,20},
  {referenceSecondsWhite, 134,230,14,22,9},
  {referenceSecondsGreen, 146,338,44,72,12},
};
static_assert(sizeof(watchDials)/sizeof(watchDials[0]) == ClockFaceCount-ClockExplorer, "Missing watch dial");
static uint16_t* watchBases[ClockFaceCount-ClockExplorer];
static const WatchDial* activeWatch;  // null selects the Peacock sweep

static uint16_t* allocateDialPixels() {
#ifdef ESP32
  return static_cast<uint16_t*>(ps_malloc(lagoonFrameBytes));
#else
  return static_cast<uint16_t*>(malloc(lagoonFrameBytes));
#endif
}

inline void prepareLagoonCache() {
  if (lagoonUnderlay || lagoonDial) return;
  // Both images exceed internal RAM; leave the Wi-Fi and DMA heaps to their drivers.
  lagoonUnderlay = allocateDialPixels();
  lagoonDial = allocateDialPixels();
  if (lagoonDial) {
    uint16_t* canvasPixels = fb;
    fb = lagoonDial;
    lagoonBase();
    fb = canvasPixels;
  }
}

// CO5300 windows start at even coordinates and contain even numbers of rows/columns.
constexpr int sweepRows = 12, sweepBands = (size+sweepRows-1)/sweepRows;
struct SweepSpan {
  int left = size, right = 0;  // right is exclusive
  void include(int x0, int x1) {
    if (x1 < x0 || x1 < 0 || x0 >= size) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= size) x1 = size-1;
    x0 &= ~1;
    x1 = (x1+2)&~1;  // inclusive -> even exclusive
    if (x0 < left) left = x0;
    if (x1 > right) right = x1;
  }
};

static void markHand(SweepSpan* spans, float seconds, int tail, int tip, int pad, int cx=lagoonCx, int cy=lagoonCy) {
  if (seconds < 0) return;
  const float a = seconds*lagoonStep, dx = sinf(a), dy = -cosf(a);
  const float y0 = cy-tail*dy, y1 = cy+tip*dy;
  int first = int(fminf(y0,y1)-pad)/sweepRows, last = int(ceilf(fmaxf(y0,y1)+pad))/sweepRows;
  if (first<0) first=0;
  if (last>=sweepBands) last=sweepBands-1;
  for (int b = first; b <= last; ++b) {
    float near = -tail, far = tip;
    if (fabsf(dy) > 0.0001f) {
      float t0 = (b*sweepRows-pad-cy)/dy, t1 = ((b+1)*sweepRows-1+pad-cy)/dy;
      near = fmaxf(near,fminf(t0,t1));
      far = fminf(far,fmaxf(t0,t1));
      if (far < near) continue;
    }
    float x0 = cx + near*dx, x1 = cx + far*dx;
    spans[b].include(int(floorf(fminf(x0,x1)-pad)),int(ceilf(fmaxf(x0,x1)+pad)));
  }
}

static void markSweep(SweepSpan* spans, float seconds) {
  if (activeWatch)
    markHand(spans,seconds,activeWatch->tail,activeWatch->tip,activeWatch->pad,activeWatch->cx,activeWatch->cy);
  else markHand(spans,seconds,lagoonSecondTail,lagoonSecondTip,4);
}

static void copyLagoonSpans(uint16_t* dest, const uint16_t* source, const SweepSpan* spans) {
  for (int b = 0; b < sweepBands; ++b) {
    int x = spans[b].left, w = spans[b].right-x;
    if (w <= 0) continue;
    int top = b*sweepRows, bottom = (b+1)*sweepRows < size ? (b+1)*sweepRows : size;
    for (int y = top; y < bottom; ++y) memcpy(dest+y*size+x,source+y*size+x,w*sizeof(uint16_t));
  }
}

static void includeRowChanges(SweepSpan& changed, int y, int left, int right) {
  const uint16_t *src=lagoonUnderlay+y*size, *shown=fb+y*size;
  if (!memcmp(src+left,shown+left,(right-left)*sizeof(uint16_t))) return;
  while (src[left]==shown[left]) ++left;
  --right;
  while (src[right]==shown[right]) --right;
  changed.include(left,right);
}

static void submitSweep(SweepSpan* spans, float seconds) {
  static uint16_t packed[size*sweepRows];
  copyLagoonSpans(fb,lagoonUnderlay,spans);
  // Only the completed composition goes to the display; the clean dial is never visible.
  if (seconds >= 0) (activeWatch ? activeWatch->second : lagoonSecondHand)(seconds);
  lastSweepPixels = 0;
  for (int b = 0; b < sweepBands; ++b) {
    int x = spans[b].left, w = spans[b].right-x;
    if (w <= 0) continue;
    int y = b*sweepRows, h = y+sweepRows < size ? sweepRows : size-y;
    for (int j = 0; j < h; ++j) memcpy(packed+j*w,fb+(y+j)*size+x,w*sizeof(uint16_t));
    panel.draw16bitRGBBitmap(x,y,packed,w,h);
    lastSweepPixels += w*h;
  }
  lastSweep = seconds;
}

inline bool updateLagoonSecond(float seconds) {
  if (!lagoonOnPanel || !lagoonUnderlay || lastSweep < 0 || seconds == lastSweep) return false;
  SweepSpan spans[sweepBands];
  markSweep(spans,lastSweep);
  markSweep(spans,seconds);
  submitSweep(spans,seconds);
  return true;
}

static void drawLagoonPage(const tm* local, const WeatherData* weather, const char* status,
                           bool stale, bool hint, const ClockBattery* battery, float seconds) {
  if (!local) seconds = -1;
  static char previousKey[96];
  static SweepSpan previousMarks[sweepBands];
  if (activeWatch) {
    // The other dial used the shared underlay. Rebuild before applying Peacock's minute delta.
    activeWatch = nullptr;
    lagoonOnPanel = false;
    previousKey[0] = 0;
  }
  const int minute = local ? ((local->tm_mon*31+local->tm_mday)*24+local->tm_hour)*60+local->tm_min : -1;
  const int batteryLabel = !battery ? -3 : battery->valid && !battery->present && battery->usb ? -2 : battery->percent;
  char key[sizeof(previousKey)];
  int length = snprintf(key,sizeof(key),"%d|%d|%.0f|%d|%d|%d|%s",minute,weather ? weather->wmo : -1000,
                        weather ? weather->temperature : 0,batteryLabel,stale,hint,status ? status : "");
  const bool reusable = lagoonUnderlay && length >= 0 && size_t(length) < sizeof(key);
  if (reusable && !strcmp(key,previousKey)) {
    if (lagoonOnPanel) { updateLagoonSecond(seconds); return; }
  } else {
    const bool partial = lagoonUnderlay && lagoonDial && previousKey[0];
    uint16_t* canvasPixels = fb;
    if (lagoonUnderlay) fb = lagoonUnderlay;
    if (partial) copyLagoonSpans(fb,lagoonDial,previousMarks);
    else if (lagoonDial) memcpy(fb,lagoonDial,lagoonFrameBytes);
    else lagoonBase();

    LagoonTextBounds text;
    lagoonFace(local,weather,status,stale,hint,battery,&text);
    constexpr uint16_t ink = rgb(185,216,207);
    pager(0,ink,rgb(61,102,102));
    fb = canvasPixels;

    SweepSpan marks[sweepBands];
    for (int b=text.top/sweepRows; b<=text.bottom/sweepRows; ++b) marks[b].include(text.left,text.right);
    for (int b=215/sweepRows; b<=244/sweepRows; ++b) marks[b].include(320,386);  // date aperture
    for (int b=415/sweepRows; b<=423/sweepRows; ++b) marks[b].include(206,256);  // page dots
    if (text.noteWidth)
      for (int b=lagoonNoteTop/sweepRows; b<=(lagoonNoteTop+22)/sweepRows; ++b)
        marks[b].include(lagoonCx-text.noteWidth/2-3,lagoonCx+(text.noteWidth+1)/2+3);
    if (local) {
      markHand(marks,5*((local->tm_hour%12)+local->tm_min/60.f),12,123,14);
      markHand(marks,local->tm_min,15,lagoonMinuteTip,9);
    }
    SweepSpan changed[sweepBands];
    if (partial && lagoonOnPanel) {
      // All remaining pixels belong to the fixed dial. Compare only old and new complications.
      for (int b=0; b<sweepBands; ++b) {
        SweepSpan span = previousMarks[b];
        span.include(marks[b].left,marks[b].right-1);
        const int x = span.left, end = span.right;
        if (end<=x) continue;
        for (int y=b*sweepRows; y<(b+1)*sweepRows && y<size; ++y)
          includeRowChanges(changed[b],y,x,end);
      }
    }
    memcpy(previousMarks,marks,sizeof(marks));
    if (reusable) memcpy(previousKey,key,length+1);
    else previousKey[0]=0;
    if (partial && lagoonOnPanel) {
      markSweep(changed,lastSweep);
      markSweep(changed,seconds);
      submitSweep(changed,seconds);
      return;
    }
  }
  if (lagoonUnderlay) memcpy(fb,lagoonUnderlay,lagoonFrameBytes);
  if (seconds >= 0) lagoonSecondHand(seconds);
  lastSweep = seconds;
  canvas.flush();
  lagoonOnPanel = true;
}

static void drawWatchPage(uint8_t face, const tm* local, float seconds) {
  const int index = face-ClockExplorer;
  const WatchDial& dial = watchDials[index];
  if (!lagoonUnderlay) prepareLagoonCache();
  uint16_t* canvasPixels = fb;
  uint16_t*& base = watchBases[index];
  if (!base) {
    base = allocateDialPixels();
    if (base) { fb = base; referenceDialBase(face); }
  }
  fb = lagoonUnderlay ? lagoonUnderlay : canvasPixels;
  if (base) memcpy(fb,base,lagoonFrameBytes);
  else referenceDialBase(face);
  if (face==ClockPanoGreen) referenceCalendarGreen(local);
  if (local) referenceHourMinute(face,*local);
  fb = canvasPixels;
  if (!local) seconds = -1;
  if (lagoonUnderlay && lagoonOnPanel && activeWatch == &dial) {
    SweepSpan changed[sweepBands];
    for (int y=0; y<size; ++y) includeRowChanges(changed[y/sweepRows],y,0,size);
    markSweep(changed,seconds);
    submitSweep(changed,seconds);
    return;
  }
  if (lagoonUnderlay) memcpy(fb,lagoonUnderlay,lagoonFrameBytes);
  lastSweep = seconds;
  if (local) dial.second(seconds);
  canvas.flush();
  activeWatch = &dial;
  lagoonOnPanel = true;
}
}  // namespace clock_face

using clock_face::updateLagoonSecond;
