#include "bd_board.h"

#include "Arduino.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "board";
static esp_timer_handle_t s_led_timer = nullptr;

static void led_off_cb(void*) { bd_board_led(false); }

// A software restart leaves the GPIO peripheral as it was. A level interrupt
// still armed on one of these pins then fires as soon as the first driver
// installs the GPIO interrupt service, before anyone registered a handler that
// could switch it off - the station hangs in the interrupt until the watchdog
// restarts it, which again keeps the GPIO state. Only a power cycle helped.
static void disarm_level_interrupts() {
  const int pins[] = {BD_PIN_USER_BUTTON, BD_PIN_LORA_DIO1};
  for (int pin : pins) {
    if (pin < 0) continue;
    gpio_intr_disable((gpio_num_t)pin);
    gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_DISABLE);
    gpio_wakeup_disable((gpio_num_t)pin);
  }
}

void bd_board_init(void) {
  disarm_level_interrupts();
  if (BD_PIN_LED >= 0) {
    pinMode(BD_PIN_LED, OUTPUT);
    digitalWrite(BD_PIN_LED, 0);
  }
  if (BD_PIN_USER_BUTTON >= 0) {
    pinMode(BD_PIN_USER_BUTTON, INPUT_PULLUP);
  }
  if (BD_PIN_VEXT_CTRL >= 0) {
    // Active low on Heltec boards; unused rail, held off to save power.
    pinMode(BD_PIN_VEXT_CTRL, OUTPUT);
    digitalWrite(BD_PIN_VEXT_CTRL, 1);
  }
  if (BD_PIN_ADC_CTRL >= 0) {
    pinMode(BD_PIN_ADC_CTRL, OUTPUT);
    digitalWrite(BD_PIN_ADC_CTRL, 0);
  }

  if (!s_led_timer) {
    esp_timer_create_args_t args = {};
    args.callback = led_off_cb;
    args.name = "led_off";
    esp_timer_create(&args, &s_led_timer);
  }

  ESP_LOGI(TAG, "board ready: uuid %04x, lora cs=%d irq=%d busy=%d", bd_board_uuid(),
           BD_PIN_LORA_CS, BD_PIN_LORA_DIO1, BD_PIN_LORA_BUSY);
}

void bd_board_led(bool on) {
  if (BD_PIN_LED >= 0) digitalWrite(BD_PIN_LED, on ? 1 : 0);
}

void bd_board_led_blink(uint32_t on_ms) {
  if (BD_PIN_LED < 0 || !s_led_timer) return;
  bd_board_led(true);
  esp_timer_stop(s_led_timer);
  esp_timer_start_once(s_led_timer, (uint64_t)on_ms * 1000ULL);
}

bool bd_board_button_pressed(void) {
  if (BD_PIN_USER_BUTTON < 0) return false;
  return digitalRead(BD_PIN_USER_BUTTON) == 0;
}

// --- runtime button ---------------------------------------------------------
static TaskHandle_t s_button_task = nullptr;
static bd_button_cb_t s_button_cb = nullptr;

static void IRAM_ATTR button_isr(void*) {
  // Level triggered like the radio line (GPIO light-sleep wake accepts levels
  // only), so mask it until the press has been handled and released.
  gpio_intr_disable((gpio_num_t)BD_PIN_USER_BUTTON);
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(s_button_task, &woken);
  portYIELD_FROM_ISR(woken);
}

static void button_task(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(30));  // debounce
    if (bd_board_button_pressed()) {
      if (s_button_cb) s_button_cb();
      while (bd_board_button_pressed()) vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(30));  // release bounce
    gpio_intr_enable((gpio_num_t)BD_PIN_USER_BUTTON);
  }
}

void bd_board_button_start(bd_button_cb_t on_press) {
  if (BD_PIN_USER_BUTTON < 0 || s_button_task) return;
  s_button_cb = on_press;
  xTaskCreate(button_task, "button", 3072, nullptr, 3, &s_button_task);

  const gpio_num_t pin = (gpio_num_t)BD_PIN_USER_BUTTON;
  gpio_set_intr_type(pin, GPIO_INTR_LOW_LEVEL);
  esp_err_t err = gpio_isr_handler_add(pin, button_isr, nullptr);
  if (err == ESP_ERR_INVALID_STATE) {
    err = gpio_install_isr_service(0);
    if (err == ESP_OK) err = gpio_isr_handler_add(pin, button_isr, nullptr);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "registering the button interrupt failed: %s", esp_err_to_name(err));
    return;
  }
  // Also keeps the pin (and its pull-up) out of the sleep-time GPIO isolation.
  gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  gpio_intr_enable(pin);
}

uint16_t bd_board_uuid(void) {
  uint8_t mac[8] = {0};
  if (esp_efuse_mac_get_default(mac) != ESP_OK) return 0;
  // Arduino's ESP.getEfuseMac() returns the MAC with byte 0 in the LSB, and the
  // old firmware masked the low 16 bits of that value.
  return (uint16_t)(mac[0] | ((uint16_t)mac[1] << 8));
}
