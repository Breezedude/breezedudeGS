// Board pin map (from Kconfig) plus the handful of GPIO helpers the firmware
// needs. Keeping this in one place is what makes the S3 and C6 targets differ
// by configuration only.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#define BD_PIN_LORA_SCK CONFIG_BDGS_PIN_LORA_SCK
#define BD_PIN_LORA_MISO CONFIG_BDGS_PIN_LORA_MISO
#define BD_PIN_LORA_MOSI CONFIG_BDGS_PIN_LORA_MOSI
#define BD_PIN_LORA_CS CONFIG_BDGS_PIN_LORA_CS
#define BD_PIN_LORA_RESET CONFIG_BDGS_PIN_LORA_RESET
#define BD_PIN_LORA_DIO1 CONFIG_BDGS_PIN_LORA_DIO1
#define BD_PIN_LORA_BUSY CONFIG_BDGS_PIN_LORA_BUSY
#define BD_PIN_LED CONFIG_BDGS_PIN_LED
#define BD_PIN_USER_BUTTON CONFIG_BDGS_PIN_USER_BUTTON
#define BD_PIN_VEXT_CTRL CONFIG_BDGS_PIN_VEXT_CTRL
#define BD_PIN_ADC_CTRL CONFIG_BDGS_PIN_ADC_CTRL
#define BD_PIN_ADC_BATTERY CONFIG_BDGS_PIN_ADC_BATTERY

#ifdef __cplusplus
extern "C" {
#endif

void bd_board_init(void);
void bd_board_led(bool on);
void bd_board_led_blink(uint32_t on_ms);
bool bd_board_button_pressed(void);

// Calls `on_press` (from a task, not the ISR) each time the user button is
// pressed at runtime. The button is a light-sleep wake source, so a press is
// seen immediately even while the station sleeps. The boot-time "hold to reset
// the configuration" check is separate and runs before this is started.
typedef void (*bd_button_cb_t)(void);
void bd_board_button_start(bd_button_cb_t on_press);

// 16 bit station id derived from the efuse MAC, same value the Arduino build
// used for the FANET address and the AP SSID suffix.
uint16_t bd_board_uuid(void);

#ifdef __cplusplus
}
#endif
