#include "HardwareSerial.h"

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#define UART_PORT UART_NUM_0
#define UART_RX_BUFFER 1024
#define UART_TX_BUFFER 2048
#define USB_RX_BUFFER 512
#define USB_TX_BUFFER 2048

HardwareSerial Serial(-1);
HardwareSerial Serial0(0);

static bool s_uart_ready = false;
static bool s_usb_ready = false;
static bool s_log_enabled = true;
static SemaphoreHandle_t s_write_lock = nullptr;
static vprintf_like_t s_prev_log_handler = nullptr;

static int serial_log_vprintf(const char* format, va_list args) {
  if (!s_log_enabled) return 0;
  return (int)Serial.vprintf(format, args);
}

static void serial_hw_init(bool with_usb) {
  if (!s_write_lock) s_write_lock = xSemaphoreCreateMutex();

  if (!s_uart_ready) {
    uart_config_t cfg = {};
    cfg.baud_rate = 115200;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    // XTAL, not the default APB: dynamic frequency scaling moves APB between
    // 80 and 40 MHz, and output that bypasses the driver's PM lock (WiFi blob,
    // ROM printf) then came out at the wrong baud rate. The web installer
    // parses this port (GS_READY, GS_IP, GS_CONFIG replies).
    cfg.source_clk = UART_SCLK_XTAL;
    if (uart_driver_install(UART_PORT, UART_RX_BUFFER, UART_TX_BUFFER, 0, nullptr, 0) == ESP_OK &&
        uart_param_config(UART_PORT, &cfg) == ESP_OK) {
      s_uart_ready = true;
    }
  }

  if (with_usb && !s_usb_ready) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = USB_RX_BUFFER;
    cfg.tx_buffer_size = USB_TX_BUFFER;
    s_usb_ready = usb_serial_jtag_driver_install(&cfg) == ESP_OK;
  }
}

void HardwareSerial::begin(unsigned long baud, uint32_t, int8_t, int8_t) {
  serial_hw_init(port_ < 0);
  if (s_uart_ready && baud != 0) {
    uart_set_baudrate(UART_PORT, (uint32_t)baud);
  }
}

void HardwareSerial::end() { flush(); }

void HardwareSerial::setDebugOutput(bool enable) {
  s_log_enabled = enable;
  if (enable && !s_prev_log_handler) {
    s_prev_log_handler = esp_log_set_vprintf(serial_log_vprintf);
  }
}

size_t HardwareSerial::write(const uint8_t* buffer, size_t size) {
  if (!buffer || size == 0) return 0;
  if (!s_uart_ready && !s_usb_ready) {
    // Before begin(): fall back to the ROM console so early boot logs survive.
    return fwrite(buffer, 1, size, stdout);
  }

  if (s_write_lock) xSemaphoreTake(s_write_lock, pdMS_TO_TICKS(100));
  size_t written = 0;
  if (s_uart_ready) {
    int n = uart_write_bytes(UART_PORT, (const char*)buffer, size);
    if (n > 0) written = (size_t)n;
  }
  if (port_ < 0 && s_usb_ready) {
    // Zero timeout: with no host attached the data is dropped instead of
    // stalling the calling task.
    int n = usb_serial_jtag_write_bytes(buffer, size, 0);
    if (n > 0 && (size_t)n > written) written = (size_t)n;
  }
  if (s_write_lock) xSemaphoreGive(s_write_lock);
  return written;
}

size_t HardwareSerial::write(uint8_t c) { return write(&c, 1); }

void HardwareSerial::flush() {
  if (s_uart_ready) uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(100));
}

int HardwareSerial::available() {
  if (peeked_ >= 0) return 1;
  size_t pending = 0;
  if (s_uart_ready && uart_get_buffered_data_len(UART_PORT, &pending) == ESP_OK && pending > 0) {
    return (int)pending;
  }
  if (port_ < 0 && s_usb_ready) {
    uint8_t probe = 0;
    int n = usb_serial_jtag_read_bytes(&probe, 1, 0);
    if (n == 1) {
      peeked_ = probe;
      return 1;
    }
  }
  return 0;
}

int HardwareSerial::read() {
  if (peeked_ >= 0) {
    int value = peeked_;
    peeked_ = -1;
    return value;
  }
  uint8_t byte = 0;
  if (s_uart_ready && uart_read_bytes(UART_PORT, &byte, 1, 0) == 1) return byte;
  if (port_ < 0 && s_usb_ready && usb_serial_jtag_read_bytes(&byte, 1, 0) == 1) return byte;
  return -1;
}

int HardwareSerial::peek() {
  if (peeked_ < 0) peeked_ = read();
  return peeked_;
}

bool serial_usb_host_connected() { return s_usb_ready && usb_serial_jtag_is_connected(); }
