#pragma once

#include <esp32s3/ulp.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/rtc_io_reg.h>
#include <soc/sens_reg.h>

namespace clock_sleep_key {
// The 1.75C has no routed PMIC IRQ. Its I2C pins are RTC GPIOs, but cannot
// be assigned to the S3 RTC I2C controller. Use the ULP while both CPUs sleep.
constexpr gpio_num_t sda = GPIO_NUM_15, scl = GPIO_NUM_14;
constexpr unsigned regWord = CONFIG_ULP_COPROC_RESERVE_MEM / 4 - 2, valueWord = regWord + 1;
static RTC_DATA_ATTR bool active;
enum Label { send, sendBit, zeroBit, dataSet, ack, pointer, restart, readBit, nack, error, stop, halt };

#define CLOCK_I2C_LINE(pin, level) I_WR_REG_BIT(level ? RTC_GPIO_OUT_W1TS_REG : RTC_GPIO_OUT_W1TC_REG, RTC_GPIO_OUT_DATA_W1TS_S + pin, 1)
#define CLOCK_I2C_WAIT I_DELAY(36)
#define CLOCK_I2C_DATA I_RD_REG(RTC_GPIO_IN_REG, RTC_GPIO_IN_NEXT_S + sda, RTC_GPIO_IN_NEXT_S + sda)
static const ulp_insn_t program[] = {
  I_WR_REG_BIT(SENS_SAR_PERI_CLK_GATE_CONF_REG, SENS_IOMUX_CLK_EN_S, 1),
  // The S3 switches off RTC peripherals between polls. Restore the open-drain
  // GPIOs after each power-up: GPIO14 is TOUCH_PAD14, GPIO15 is XTAL_32P.
  I_WR_REG(RTC_CNTL_PAD_HOLD_REG, RTC_CNTL_TOUCH_PAD14_HOLD_S, RTC_CNTL_X32P_HOLD_S, 0),
  I_WR_REG_BIT(RTC_GPIO_PIN14_REG, RTC_GPIO_PIN14_PAD_DRIVER_S, 1),
  I_WR_REG_BIT(RTC_GPIO_PIN15_REG, RTC_GPIO_PIN15_PAD_DRIVER_S, 1),
  I_WR_REG(RTC_GPIO_OUT_W1TS_REG, RTC_GPIO_OUT_DATA_W1TS_S + scl, RTC_GPIO_OUT_DATA_W1TS_S + sda, 3),
  I_WR_REG(RTC_GPIO_ENABLE_W1TS_REG, RTC_GPIO_ENABLE_W1TS_S + scl, RTC_GPIO_ENABLE_W1TS_S + sda, 3),
  I_WR_REG(RTC_IO_TOUCH_PAD14_REG, RTC_IO_TOUCH_PAD14_RUE_S, RTC_IO_TOUCH_PAD14_RDE_S, 1),
  I_WR_REG(RTC_IO_XTAL_32P_PAD_REG, RTC_IO_X32P_RUE_S, RTC_IO_X32P_RDE_S, 1),
  // MUX_SEL=RTC, FUN_SEL=GPIO, SLP_SEL/SLP_IE/SLP_OE/FUN_IE=1.
  I_WR_REG(RTC_IO_TOUCH_PAD14_REG, RTC_IO_TOUCH_PAD14_FUN_IE_S, RTC_IO_TOUCH_PAD14_MUX_SEL_S, 0x4f),
  I_WR_REG(RTC_IO_XTAL_32P_PAD_REG, RTC_IO_X32P_FUN_IE_S, RTC_IO_X32P_MUX_SEL_S, 0x4f),
  I_DELAY(100),
  // Both lines must be released before START; stuck-low is a bus fault, not an empty IRQ.
  I_RD_REG(RTC_GPIO_IN_REG, RTC_GPIO_IN_NEXT_S + scl, RTC_GPIO_IN_NEXT_S + sda), M_BL(error, 3),
  CLOCK_I2C_LINE(sda, 0), CLOCK_I2C_WAIT, CLOCK_I2C_LINE(scl, 0),
  I_MOVI(R1, 0x34 << 1), I_MOVI(R3, 0),
  M_LABEL(send), I_MOVI(R2, 8),
  M_LABEL(sendBit), I_ANDI(R0, R1, 0x80), M_BE(zeroBit, 0),
  CLOCK_I2C_LINE(sda, 1), M_BX(dataSet),
  M_LABEL(zeroBit), CLOCK_I2C_LINE(sda, 0),
  M_LABEL(dataSet), CLOCK_I2C_WAIT, CLOCK_I2C_LINE(scl, 1), CLOCK_I2C_WAIT, CLOCK_I2C_LINE(scl, 0),
  I_LSHI(R1, R1, 1), I_SUBI(R2, R2, 1), M_BXZ(ack), M_BX(sendBit),
  M_LABEL(ack), CLOCK_I2C_LINE(sda, 1), CLOCK_I2C_WAIT, CLOCK_I2C_LINE(scl, 1), CLOCK_I2C_WAIT,
  CLOCK_I2C_DATA, CLOCK_I2C_LINE(scl, 0), M_BE(error, 1),
  I_MOVR(R0, R3), M_BE(pointer, 0), M_BE(restart, 1),
  // After the third ACK, read one byte MSB first and return a NACK.
  I_MOVI(R1, 0), I_MOVI(R2, 8),
  M_LABEL(readBit), CLOCK_I2C_WAIT, CLOCK_I2C_LINE(scl, 1), CLOCK_I2C_WAIT, CLOCK_I2C_DATA,
  I_LSHI(R1, R1, 1), I_ORR(R1, R1, R0), CLOCK_I2C_LINE(scl, 0),
  I_SUBI(R2, R2, 1), M_BXZ(nack), M_BX(readBit),
  M_LABEL(nack), CLOCK_I2C_WAIT, CLOCK_I2C_LINE(scl, 1), CLOCK_I2C_WAIT, CLOCK_I2C_LINE(scl, 0), M_BX(stop),
  M_LABEL(pointer), I_MOVI(R3, 1), I_MOVI(R1, regWord), I_LD(R1, R1, 0), M_BX(send),
  M_LABEL(restart), CLOCK_I2C_WAIT, CLOCK_I2C_LINE(scl, 1), CLOCK_I2C_WAIT,
  CLOCK_I2C_LINE(sda, 0), CLOCK_I2C_WAIT, CLOCK_I2C_LINE(scl, 0),
  I_MOVI(R3, 2), I_MOVI(R1, (0x34 << 1) | 1), M_BX(send),
  M_LABEL(error), I_MOVI(R1, 0xffff),
  M_LABEL(stop), CLOCK_I2C_LINE(sda, 0), CLOCK_I2C_WAIT, CLOCK_I2C_LINE(scl, 1), CLOCK_I2C_WAIT, CLOCK_I2C_LINE(sda, 1),
  I_MOVI(R2, regWord), I_ST32(R1, R2, 1, 0), I_LD(R0, R2, 0), M_BE(halt, 3),
  // Leave key flags latched for the normal driver. A bus error also wakes
  // the CPU, so it can recover the bus and keep listening for POWER.
  I_ANDI(R0, R1, clockPress | clockLong | clockShort), M_BE(halt, 0), I_WAKE(),
  M_LABEL(halt), I_HALT()
};
#undef CLOCK_I2C_DATA
#undef CLOCK_I2C_WAIT
#undef CLOCK_I2C_LINE

static void releaseBus() {
  ulp_timer_stop();
  delay(2);  // Finish any in-flight transaction before reclaiming the pins.
  rtc_gpio_deinit(sda);
  rtc_gpio_deinit(scl);
  active = false;
}

static bool arm() {
  size_t words = sizeof(program) / sizeof(program[0]);
  if (ulp_process_macros_and_load(0, program, &words) != ESP_OK || words > regWord) return false;
  Wire.end();
  for (gpio_num_t pin : {sda, scl}) {
    rtc_gpio_init(pin);
    rtc_gpio_set_level(pin, 1);
    rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_OUTPUT_OD);
    rtc_gpio_pullup_en(pin);
  }
  volatile uint32_t* memory = RTC_SLOW_MEM;
  memory[regWord] = 0x03;  // First read the known AXP2101 chip ID on the actual bus.
  memory[valueWord] = 0;
  const bool started = ulp_set_wakeup_period(0, 300000) == ESP_OK && ulp_run(0) == ESP_OK;
  for (int elapsed = 0; started && !(memory[valueWord] & 0xffff) && elapsed < 400; elapsed += 5) delay(5);
  if (started && (memory[valueWord] & 0xffff) == 0x4a && esp_sleep_enable_ulp_wakeup() == ESP_OK) {
    memory[regWord] = 0x49;
    active = true;
    return true;
  }
  releaseBus();
  Wire.begin(sda, scl);
  return false;
}
}
