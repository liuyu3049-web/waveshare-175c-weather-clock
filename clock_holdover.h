#pragma once

#include <atomic>
#include <sys/time.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_private/esp_clk.h>
#include <hal/clk_tree_ll.h>
#include <hal/rtc_cntl_ll.h>
#include <soc/rtc.h>

namespace clock_holdover {
struct Snapshot {
  int64_t utcUs;
  uint64_t startTicks, wakeTicks;
  uint32_t period;
  int32_t trimPpm;
  bool ntpAtEntry;
};
static RTC_DATA_ATTR Snapshot saved{};
static RTC_DATA_ATTR std::atomic<int32_t> learnedPpm{0};
static uint32_t wakePeriod;
static int64_t elapsedUs, nominalUs, sdkDeltaUs, restoredUtc, restoredAt;
static int32_t restoredTrim;
static bool trainable;
static std::atomic<int32_t> lastSyncSecond{-1};
static std::atomic<uint32_t> lastSyncEpoch{0};
static std::atomic<int32_t> ntpErrorMs{INT32_MIN};

static int64_t wallUs() {
  timeval t;
  gettimeofday(&t,nullptr);
  return int64_t(t.tv_sec)*1000000+t.tv_usec;
}

static void setWallUs(int64_t us) {
  const timeval t{static_cast<time_t>(us/1000000),static_cast<suseconds_t>(us%1000000)};
  settimeofday(&t,nullptr);
}

// Counter periods use the SDK's microseconds Q19 format. Split the
// product so even a long shutdown cannot overflow the intermediate.
static uint64_t ticksToUs(uint64_t ticks,uint32_t period) {
  constexpr uint32_t unit=1U<<RTC_CLK_CAL_FRACT;
  return (ticks/unit)*period+((ticks%unit)*period)/unit;
}

static uint32_t measureFastDivider() {
  rtc_clk_8m_enable(true,true);
  return rtc_clk_cal(RTC_CAL_8MD256,2048);  // measured against the 40 MHz crystal
}

static void RTC_IRAM_ATTR onWake() {
  esp_default_wake_deep_sleep();
  saved.wakeTicks=rtc_cntl_ll_get_rtc_time();
  // From this boundary onward the counter uses the SDK's boot clock.
  // The timestamps on either side can then be converted independently.
  clk_ll_rtc_slow_set_src(SOC_RTC_SLOW_CLK_SRC_RC_SLOW);
}

static bool prepare() {
  saved={};
  const uint32_t period=measureFastDivider();
  const int64_t utc=wallUs(), mono=esp_timer_get_time();
  if (!period || utc<1700000000000000LL) return false;
  rtc_clk_slow_src_set(SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256);
  esp_clk_slowclk_cal_set(period);
  // Anchor to UTC captured before changing the slow-clock conversion.
  saved.startTicks=rtc_time_get();
  saved.utcUs=utc+(esp_timer_get_time()-mono);
  saved.period=period;
  saved.trimPpm=learnedPpm.load();
  const int32_t sync=lastSyncSecond.load();
  const int64_t age=mono/1000000-sync;
  const int64_t offset=utc/1000000-(int64_t(lastSyncEpoch.load())+age);
  saved.ntpAtEntry=sync>=0 && age>=0 && age<=7200 && offset>=-2 && offset<=2;
  setWallUs(saved.utcUs);
  esp_set_deep_sleep_wake_stub(onWake);
  return true;
}

static bool restore() {
  if (esp_reset_reason()!=ESP_RST_DEEPSLEEP || !saved.period ||
      saved.wakeTicks<=saved.startTicks) return false;
  // Measure the cooled oscillator while the panel and radio are still off.
  wakePeriod=measureFastDivider();
  const uint32_t period=wakePeriod ? (uint64_t(saved.period)+wakePeriod)/2 : saved.period;
  const uint64_t ticks=saved.wakeTicks-saved.startTicks;
  nominalUs=ticksToUs(ticks,period);
  restoredTrim=saved.trimPpm;
  const uint32_t adjustedPeriod=uint64_t(period)*(1000000+restoredTrim)/1000000;
  elapsedUs=ticksToUs(ticks,adjustedPeriod);
  const uint64_t nowTicks=rtc_time_get();
  if (nowTicks<saved.wakeTicks) return false;
  const uint64_t bootUs=ticksToUs(nowTicks-saved.wakeTicks,esp_clk_slowclk_cal_get());
  restoredUtc=saved.utcUs+elapsedUs+bootUs;
  restoredAt=esp_timer_get_time();
  trainable=saved.ntpAtEntry && nominalUs>=1800LL*1000000;
  sdkDeltaUs=restoredUtc-wallUs();
  setWallUs(restoredUtc);
  return true;
}

// Compare the first genuine network timestamp with the independent wake
// estimate even though SNTP has already adjusted gettimeofday().
static void onNetworkTime(struct timeval* t) {
  const int64_t mono=esp_timer_get_time();
  lastSyncEpoch.store(static_cast<uint32_t>(t->tv_sec));
  lastSyncSecond.store(static_cast<int32_t>(mono/1000000));
  if (!restoredUtc || ntpErrorMs.load()!=INT32_MIN) return;
  const int64_t awakeUs=mono-restoredAt;
  const int64_t expected=restoredUtc+awakeUs;
  const int64_t error=(int64_t(t->tv_sec)*1000000+t->tv_usec-expected)/1000;
  if (error<=INT32_MIN || error>INT32_MAX) return;
  ntpErrorMs.store(static_cast<int32_t>(error));
  if (!trainable || awakeUs>10LL*60*1000000) return;
  // Derive the absolute observed correction relative to the untrimmed
  // interval; blend rather than adding the same residual repeatedly.
  const int64_t observed=restoredTrim+error*1000000000LL/nominalUs;
  if (observed>=-50000 && observed<=50000)
    learnedPpm.store(static_cast<int32_t>((restoredTrim+observed)/2));
}
}  // namespace clock_holdover
