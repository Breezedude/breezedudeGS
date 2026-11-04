#include "Arduino.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_clk_tree.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/adc_types.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char* TAG = "compat";

EspClass ESP;

// --- timing -----------------------------------------------------------------
uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

uint64_t micros() { return (uint64_t)esp_timer_get_time(); }

void delay(uint32_t ms) {
  if (ms == 0) {
    taskYIELD();
    return;
  }
  // Round up to a full tick so short delays never busy-wait; at 1000 Hz a tick
  // is 1 ms, which is what the Arduino code expected anyway.
  vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

void delayMicroseconds(uint32_t us) { esp_rom_delay_us(us); }

void yield() { taskYIELD(); }

// --- GPIO -------------------------------------------------------------------
void pinMode(uint8_t pin, uint8_t mode) {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << pin;
  cfg.mode = (mode == OUTPUT) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
  cfg.pull_up_en = (mode == INPUT_PULLUP) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = (mode == INPUT_PULLDOWN) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&cfg);
}

void digitalWrite(uint8_t pin, uint8_t level) { gpio_set_level((gpio_num_t)pin, level ? 1 : 0); }

int digitalRead(uint8_t pin) { return gpio_get_level((gpio_num_t)pin); }

// --- ADC --------------------------------------------------------------------
// One shared oneshot unit, created lazily. Only the battery divider uses it.
static adc_oneshot_unit_handle_t s_adc_unit = nullptr;
static adc_cali_handle_t s_adc_cali = nullptr;
static bool s_adc_cali_valid = false;

static bool adc_pin_to_channel(uint8_t pin, adc_channel_t* channel) {
  adc_unit_t unit;
  return adc_oneshot_io_to_channel((int)pin, &unit, channel) == ESP_OK && unit == ADC_UNIT_1;
}

static bool adc_ensure_unit() {
  if (s_adc_unit) return true;

  adc_oneshot_unit_init_cfg_t init = {};
  init.unit_id = ADC_UNIT_1;
  init.ulp_mode = ADC_ULP_MODE_DISABLE;
  if (adc_oneshot_new_unit(&init, &s_adc_unit) != ESP_OK) {
    ESP_LOGE(TAG, "adc_oneshot_new_unit failed");
    s_adc_unit = nullptr;
    return false;
  }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_curve_fitting_config_t cali = {};
  cali.unit_id = ADC_UNIT_1;
  cali.atten = ADC_ATTEN_DB_12;
  cali.bitwidth = ADC_BITWIDTH_DEFAULT;
  s_adc_cali_valid = adc_cali_create_scheme_curve_fitting(&cali, &s_adc_cali) == ESP_OK;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
  adc_cali_line_fitting_config_t cali = {};
  cali.unit_id = ADC_UNIT_1;
  cali.atten = ADC_ATTEN_DB_12;
  cali.bitwidth = ADC_BITWIDTH_DEFAULT;
  s_adc_cali_valid = adc_cali_create_scheme_line_fitting(&cali, &s_adc_cali) == ESP_OK;
#endif
  return true;
}

static bool adc_ensure_channel(uint8_t pin, adc_channel_t* channel) {
  if (!adc_ensure_unit()) return false;
  if (!adc_pin_to_channel(pin, channel)) {
    ESP_LOGE(TAG, "GPIO %u is not an ADC1 pin", pin);
    return false;
  }
  adc_oneshot_chan_cfg_t chan = {};
  chan.atten = ADC_ATTEN_DB_12;
  chan.bitwidth = ADC_BITWIDTH_DEFAULT;
  return adc_oneshot_config_channel(s_adc_unit, *channel, &chan) == ESP_OK;
}

uint16_t analogRead(uint8_t pin) {
  adc_channel_t channel;
  if (!adc_ensure_channel(pin, &channel)) return 0;
  int raw = 0;
  if (adc_oneshot_read(s_adc_unit, channel, &raw) != ESP_OK) return 0;
  return (uint16_t)raw;
}

uint32_t analogReadMilliVolts(uint8_t pin) {
  adc_channel_t channel;
  if (!adc_ensure_channel(pin, &channel)) return 0;
  int raw = 0;
  if (adc_oneshot_read(s_adc_unit, channel, &raw) != ESP_OK) return 0;
  int millivolts = 0;
  if (s_adc_cali_valid && adc_cali_raw_to_voltage(s_adc_cali, raw, &millivolts) == ESP_OK) {
    return (uint32_t)millivolts;
  }
  // No calibration data in efuse: fall back to the nominal 12 dB full scale.
  return (uint32_t)((raw * 3100) / 4095);
}

// --- CPU --------------------------------------------------------------------
// Frequency is owned by esp_pm (DFS) in this firmware, see bd_power.
bool setCpuFrequencyMhz(uint32_t mhz) {
  ESP_LOGW(TAG, "setCpuFrequencyMhz(%u) ignored - DFS is managed by esp_pm", (unsigned)mhz);
  return false;
}

uint32_t getCpuFrequencyMhz() {
  uint32_t hz = 0;
  esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &hz);
  return hz / 1000000;
}

void EspClass::restart() { esp_restart(); }

uint32_t EspClass::getFreeHeap() { return (uint32_t)esp_get_free_heap_size(); }

uint32_t EspClass::getMinFreeHeap() { return (uint32_t)esp_get_minimum_free_heap_size(); }

uint64_t EspClass::getEfuseMac() {
  uint8_t mac[8] = {0};
  if (esp_efuse_mac_get_default(mac) != ESP_OK) return 0;
  uint64_t value = 0;
  for (int i = 0; i < 6; i++) {
    value |= ((uint64_t)mac[i]) << (8 * i);
  }
  return value;
}

const char* EspClass::getChipModel() { return CONFIG_IDF_TARGET; }

// --- random -----------------------------------------------------------------
long random(long max_exclusive) {
  if (max_exclusive <= 0) return 0;
  return (long)(esp_random() % (uint32_t)max_exclusive);
}

long random(long min_inclusive, long max_exclusive) {
  if (max_exclusive <= min_inclusive) return min_inclusive;
  return min_inclusive + random(max_exclusive - min_inclusive);
}

void randomSeed(unsigned long) { /* hardware RNG, nothing to seed */ }
