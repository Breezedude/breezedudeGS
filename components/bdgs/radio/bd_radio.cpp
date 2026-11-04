// FANET / FLARM / ADS-L receive path.
//
// Ported from the Arduino main.cpp with two structural changes:
//   * the receive loop is a task that blocks on the DIO1 interrupt instead of
//     being polled from loop(), and
//   * the FSK scheduler can state how long it may be left alone
//     (flarm_scheduler_next_delay_ms), which is what makes light sleep useful.
// The radio parameters, timings and decode order are unchanged.
#include "bd_diag.h"
#include "bd_radio.h"

#include <sys/time.h>

#include "Arduino.h"
#include "adsl.h"
#include "bd_board.h"
#include "bd_console.h"
#include "bd_net.h"
#include "bd_repeat.h"
#include "bd_state.h"
#include "bd_stats.h"
#include "bd_web.h"
#include "config_gs.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "flarm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ota_gs.h"
#include "sdkconfig.h"
#include "types.h"

static const char* TAG = "radio";

#define LORA_SYNCWORD 0xF1  // SX1262 equivalent is handled by RadioLib

// RadioLib only powers the TCXO (DIO3 on the Heltec V3) when a voltage is
// passed, and the two SX126x classes disagree on their default: SX1262::begin()
// uses 1.6 V, LLCC68::begin() uses 0 (plain crystal). Every begin/beginFSK call
// has to use the value matching its part - getting it wrong leaves the
// oscillator dead and the next command fails with RADIOLIB_ERR_SPI_CMD_FAILED.
#define RADIO_SX1262_TCXO_V 1.6f
#define RADIO_LLCC68_TCXO_V 0.0f

#ifndef OTA_GS_FAST_BW_KHZ
#define OTA_GS_FAST_BW_KHZ 500
#endif
#ifndef OTA_GS_FAST_SF
#define OTA_GS_FAST_SF 7
#endif
#ifndef OTA_GS_FAST_CR
#define OTA_GS_FAST_CR 5
#endif
#ifndef OTA_GS_FAST_SYNCWORD
#define OTA_GS_FAST_SYNCWORD 0x12
#endif
#ifndef OTA_GS_FAST_PREAMBLE
#define OTA_GS_FAST_PREAMBLE 6
#endif

// --- hardware ---------------------------------------------------------------
static EspHal s_hal(BD_PIN_LORA_SCK, BD_PIN_LORA_MISO, BD_PIN_LORA_MOSI, SPI2_HOST,
                    SPI_MASTER_FREQ_8M);

SX1276 radio_sx1276 =
    new Module(&s_hal, BD_PIN_LORA_CS, BD_PIN_LORA_DIO1, BD_PIN_LORA_RESET, RADIOLIB_NC);
LLCC68 radio_llcc68 =
    new Module(&s_hal, BD_PIN_LORA_CS, BD_PIN_LORA_DIO1, BD_PIN_LORA_RESET, BD_PIN_LORA_BUSY);
SX1262 radio_sx1262 =
    new Module(&s_hal, BD_PIN_LORA_CS, BD_PIN_LORA_DIO1, BD_PIN_LORA_RESET, BD_PIN_LORA_BUSY);

RadioModuleType lora_module = RADIO_MODULE_NONE;
PhysicalLayer* radio_phy = nullptr;
RadioMode radio_mode = RADIO_MODE_FANET;

RadioIrqFlag receivedFlag;

static TaskHandle_t s_radio_task = nullptr;
static SemaphoreHandle_t s_radio_mutex = nullptr;

// --- interrupt / light-sleep wake -------------------------------------------
static void IRAM_ATTR dio1_isr(void*) {
  // Level triggered (the only kind that wakes light sleep), so the interrupt has
  // to be masked here and is re-armed once the packet has been read out.
  gpio_intr_disable((gpio_num_t)BD_PIN_LORA_DIO1);
  receivedFlag.setFromIsr();
  if (s_radio_task) {
    vTaskNotifyGiveFromISR(s_radio_task, nullptr);
  }
}

void RadioIrqFlag::arm() {
  rearm_pending_ = false;
  gpio_intr_enable((gpio_num_t)BD_PIN_LORA_DIO1);
  if (gpio_get_level((gpio_num_t)BD_PIN_LORA_DIO1)) {
    flag_ = true;
  }
}

RadioIrqFlag::operator bool() {
  if (flag_) return true;
  if (rearm_pending_) arm();
  return flag_;
}

RadioIrqFlag& RadioIrqFlag::operator=(bool value) {
  flag_ = value;
  // Do not re-arm straight away: the caller is about to read the packet out of
  // the radio, which is what drops DIO1 low again.
  if (!value) rearm_pending_ = true;
  return *this;
}

// ESP-IDF isolates every GPIO while in automatic light sleep
// (CONFIG_PM_SLP_DISABLE_GPIO, pulled in by CONFIG_ESP_WIFI_SLP_IRAM_OPT and
// therefore not switchable from sdkconfig). That saves a few hundred microamps
// but floats the transceiver's control lines on every sleep cycle - chip select
// and reset must not move, or the radio stops delivering packets. The pins are
// excluded individually, which is the mechanism IDF documents for exactly this.
static void radio_pins_keep_state_in_sleep() {
  const int pins[] = {BD_PIN_LORA_CS,  BD_PIN_LORA_RESET, BD_PIN_LORA_BUSY,
                      BD_PIN_LORA_SCK, BD_PIN_LORA_MISO,  BD_PIN_LORA_MOSI};
  for (int pin : pins) {
    if (pin >= 0) gpio_sleep_sel_dis((gpio_num_t)pin);
  }
}

static void irq_pin_setup() {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << BD_PIN_LORA_DIO1;
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_HIGH_LEVEL;
  gpio_config(&cfg);

  // RadioLib's HAL may already have installed the ISR service; try to register
  // first so the normal path does not log an error about that.
  esp_err_t err = gpio_isr_handler_add((gpio_num_t)BD_PIN_LORA_DIO1, dio1_isr, nullptr);
  if (err == ESP_ERR_INVALID_STATE) {
    err = gpio_install_isr_service(0);
    if (err == ESP_OK) {
      err = gpio_isr_handler_add((gpio_num_t)BD_PIN_LORA_DIO1, dio1_isr, nullptr);
    }
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "registering the DIO1 interrupt failed: %s", esp_err_to_name(err));
  }

  // Same pin, two jobs: packet interrupt while awake, wake-up source while
  // asleep. Level triggers are the only ones GPIO light-sleep wake accepts.
  gpio_wakeup_enable((gpio_num_t)BD_PIN_LORA_DIO1, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  gpio_intr_enable((gpio_num_t)BD_PIN_LORA_DIO1);
}

// --- radio bring-up ---------------------------------------------------------
bool init_lora_radio() {
  if (radio_sx1276.begin(868.2f, 250.0f, 7, 5, LORA_SYNCWORD, 10, 12, 0) == RADIOLIB_ERR_NONE) {
    radio_phy = (PhysicalLayer*)&radio_sx1276;
    lora_module = RADIO_MODULE_SX1276;
    ESP_LOGI(TAG, "radio: LoRa SX1276");
    return true;
  }

  if (radio_llcc68.begin(868.2f, 250.0f, 7, 5, LORA_SYNCWORD, 10, 12, RADIO_LLCC68_TCXO_V) ==
      RADIOLIB_ERR_NONE) {
    radio_phy = (PhysicalLayer*)&radio_llcc68;
    lora_module = RADIO_MODULE_LLCC68;
    radio_phy->setOutputPower(22);
    static_cast<SX126x*>(radio_phy)->setRegulatorDCDC();
    ESP_LOGI(TAG, "radio: LoRa LLCC68");
    return true;
  }

  if (radio_sx1262.begin(868.2f, 250.0f, 7, 5, LORA_SYNCWORD, 10, 12, RADIO_SX1262_TCXO_V) ==
      RADIOLIB_ERR_NONE) {
    radio_phy = (PhysicalLayer*)&radio_sx1262;
    lora_module = RADIO_MODULE_SX1262;
    radio_phy->setOutputPower(22);
    static_cast<SX126x*>(radio_phy)->setRegulatorDCDC();
    ESP_LOGI(TAG, "radio: LoRa SX1262");
    return true;
  }

  radio_phy = nullptr;
  lora_module = RADIO_MODULE_NONE;
  return false;
}

void configure_lora_radio(bool fastMode) {
  if (radio_phy == nullptr) {
    return;
  }

  radio_phy->standby();
  radio_phy->setFrequency(868.2f);

  switch (lora_module) {
    case RADIO_MODULE_SX1276:
      radio_sx1276.setBandwidth((float)(fastMode ? OTA_GS_FAST_BW_KHZ : 250));
      radio_sx1276.setSpreadingFactor(fastMode ? OTA_GS_FAST_SF : 7);
      radio_sx1276.setCodingRate(fastMode ? OTA_GS_FAST_CR : 5);
      radio_sx1276.setSyncWord(fastMode ? OTA_GS_FAST_SYNCWORD : LORA_SYNCWORD);
      radio_sx1276.setPreambleLength(fastMode ? OTA_GS_FAST_PREAMBLE : 12);
      break;
    case RADIO_MODULE_LLCC68:
      radio_llcc68.setBandwidth((float)(fastMode ? OTA_GS_FAST_BW_KHZ : 250));
      radio_llcc68.setSpreadingFactor(fastMode ? OTA_GS_FAST_SF : 7);
      radio_llcc68.setCodingRate(fastMode ? OTA_GS_FAST_CR : 5);
      radio_llcc68.setSyncWord(fastMode ? OTA_GS_FAST_SYNCWORD : LORA_SYNCWORD);
      radio_llcc68.setPreambleLength(fastMode ? OTA_GS_FAST_PREAMBLE : 12);
      break;
    case RADIO_MODULE_SX1262:
      radio_sx1262.setBandwidth((float)(fastMode ? OTA_GS_FAST_BW_KHZ : 250));
      radio_sx1262.setSpreadingFactor(fastMode ? OTA_GS_FAST_SF : 7);
      radio_sx1262.setCodingRate(fastMode ? OTA_GS_FAST_CR : 5);
      radio_sx1262.setSyncWord(fastMode ? OTA_GS_FAST_SYNCWORD : LORA_SYNCWORD);
      radio_sx1262.setPreambleLength(fastMode ? OTA_GS_FAST_PREAMBLE : 12);
      break;
    default:
      break;
  }

  receivedFlag = false;
  radio_mode = RADIO_MODE_FANET;
  radio_phy->startReceive();
}

#if CONFIG_BDGS_ENABLE_FSK_RX

// ── FANET / FLARM / ADS-L radio time sharing ─────────────────────────
//
// The board has a single transceiver and the three protocols are mutually
// exclusive: FANET is LoRa, FLARM and ADS-L are 2-GFSK with different sync
// words. Only one can be received at a time.
//
// FLARM does not transmit continuously: in zone 1 (Europe) it uses 868.2 MHz
// between 400-800 ms and 868.4 MHz between 850-1200 ms after each UTC second,
// and the band is silent in between (window boundaries from GXAirCom
// lib/FANETLORA/radio/fmac.h). A stationary station with NTP can therefore aim
// straight at those windows instead of sweeping blindly.
//
// ADS-L has no such structure - it uses CSMA with a random backoff (spec D.4),
// so a sender may transmit at any time. Its capture rate is simply proportional
// to dwell time, which means the ADS-L window can be placed anywhere.
//
// The guard band is wider than GXAirCom's 30 ms because NTP jitters more than a
// GPS PPS pulse. Light sleep adds its own wake latency (~1 ms at 160 MHz), well
// inside that guard.
#define FLARM_GUARD_MS 60
#define FLARM_LOW_BEGIN_MS (400 - FLARM_GUARD_MS)
#define FLARM_HIGH_BEGIN_MS (850 - FLARM_GUARD_MS)
#define FLARM_WINDOW_END_MS (1200 + FLARM_GUARD_MS)

// ADS-L dwell, split evenly over both channels. Sized to match the FLARM
// excursion so either protocol costs the same share of FANET time.
#define ADSL_CHANNEL_MS 460
#define ADSL_WINDOW_END_MS (2 * ADSL_CHANNEL_MS)

// Seconds between the start of one window and the next. Rotating rather than
// fixed so we never keep hitting (or keep missing) the same transmit slot of a
// given aircraft.
static const uint8_t FSK_PERIOD_SPARING[3] = {8, 9, 10};   // ~10 % off FANET, default
static const uint8_t FSK_PERIOD_BALANCED[3] = {3, 4, 5};   // ~23 %
static const uint8_t FSK_PERIOD_PRIORITY[3] = {1, 1, 1};   // ~90 %

// Blind fallback when there is no NTP time, same weighting as above.
static const uint16_t FSK_BLIND_LORA_MS[3] = {18000, 8000, 200};
#define FSK_BLIND_WINDOW_MS 1000

static uint8_t fsk_period_idx = 0;
static uint32_t fsk_anchor_ms = 0;   // millis() of the UTC second the window belongs to
static time_t fsk_anchor_sec = 0;    // that same second, in UTC
static time_t fsk_next_cycle = 0;    // earliest UTC second for the next window
static uint32_t fsk_blind_switch_ms = 0;
// Which protocol the next window serves; only meaningful when both are enabled.
static bool fsk_next_is_adsl = false;

uint32_t flarm_rx_frames = 0;
uint32_t flarm_crc_errors = 0;
uint32_t flarm_decode_fail = 0;
uint32_t flarm_decoded = 0;

uint32_t adsl_rx_frames = 0;
uint32_t adsl_crc_errors = 0;
uint32_t adsl_decode_fail = 0;
uint32_t adsl_decoded = 0;

bool flarm_verbose_ws = false;
bool adsl_verbose_ws = false;
uint32_t flarm_switch_us_max = 0;
uint32_t flarm_entry_late_ms_max = 0;

static const uint8_t* fsk_periods() {
  switch (settings.flarmDutyProfile) {
    case 1:
      return FSK_PERIOD_BALANCED;
    case 2:
      return FSK_PERIOD_PRIORITY;
    default:
      return FSK_PERIOD_SPARING;
  }
}

// The FLARM v7 packet only carries the low bits of latitude/longitude, so
// without a configured station position nothing can be reconstructed. ADS-L
// transmits absolute coordinates and has no such requirement.
static bool flarm_position_known() {
  return !(fabsf(settings.latitude - 47.0f) < 0.001f &&
           fabsf(settings.longitude - 12.0f) < 0.001f);
}

bool flarm_enabled() {
  return settings.receiveFlarm && radio_phy != nullptr && flarm_position_known();
}

bool adsl_enabled() { return settings.receiveAdsl && radio_phy != nullptr; }

bool fanet_enabled() { return settings.receiveFanet; }

static bool mode_is_flarm(RadioMode m) {
  return m == RADIO_MODE_FLARM_LOW || m == RADIO_MODE_FLARM_HIGH;
}

static bool mode_is_adsl(RadioMode m) {
  return m == RADIO_MODE_ADSL_LOW || m == RADIO_MODE_ADSL_HIGH;
}

// Applies the sync word and frame length of one of the two GFSK protocols. They
// share the entire modem configuration - 100 kbps, +-50 kHz, BT 0.5, Manchester,
// same channels - and differ only in these two settings.
static void radio_apply_fsk_protocol(RadioMode mode) {
  const bool adsl = mode_is_adsl(mode);
  static const uint8_t flarmSync[6] = {0xA5, 0xA9, 0x55, 0x66, 0x65, 0x96};

  // Sync word must be set before the packet length: RadioLib derives the
  // preamble detector length from the sync word and preamble length together.
  if (adsl) {
    radio_phy->setSyncWord((uint8_t*)ADSL_SYNCWORD, ADSL_SYNCWORD_LEN);
  } else {
    radio_phy->setSyncWord((uint8_t*)flarmSync, sizeof(flarmSync));
  }

  const uint8_t rawLen = adsl ? ADSL_RAW_LEN : FLARM_RAW_LEN;
  switch (lora_module) {
    case RADIO_MODULE_SX1276:
      radio_sx1276.fixedPacketLengthMode(rawLen);
      break;
    case RADIO_MODULE_LLCC68:
      radio_llcc68.fixedPacketLengthMode(rawLen);
      break;
    case RADIO_MODULE_SX1262:
      radio_sx1262.fixedPacketLengthMode(rawLen);
      break;
    default:
      break;
  }
}

static bool radio_apply_mode(RadioMode mode) {
  if (radio_phy == nullptr) return false;

  const bool highChannel = (mode == RADIO_MODE_FLARM_HIGH || mode == RADIO_MODE_ADSL_HIGH);
  const float freq = highChannel ? FLARM_FREQ_HIGH_MHZ : FLARM_FREQ_LOW_MHZ;

  // Staying inside FSK only needs a retune plus, when the protocol changes, a
  // new sync word - far cheaper than a full modem reconfiguration.
  if (mode != RADIO_MODE_FANET && radio_mode != RADIO_MODE_FANET) {
    radio_phy->standby();
    if (mode_is_adsl(mode) != mode_is_adsl(radio_mode)) {
      radio_apply_fsk_protocol(mode);
    }
    if (radio_phy->setFrequency(freq) != RADIOLIB_ERR_NONE) return false;
    radio_mode = mode;
    receivedFlag = false;
    radio_phy->startReceive();
    return true;
  }

  if (mode == RADIO_MODE_FANET) {
    // Coming back from GFSK the packet type has to be switched, and only
    // begin() does that - the individual setters all refuse on the wrong modem.
    int16_t st = RADIOLIB_ERR_UNKNOWN;
    switch (lora_module) {
      case RADIO_MODULE_SX1276:
        st = radio_sx1276.begin(868.2f, 250.0f, 7, 5, LORA_SYNCWORD, 10, 12, 0);
        break;
      case RADIO_MODULE_LLCC68:
        st = radio_llcc68.begin(868.2f, 250.0f, 7, 5, LORA_SYNCWORD, 10, 12, RADIO_LLCC68_TCXO_V);
        break;
      case RADIO_MODULE_SX1262:
        st = radio_sx1262.begin(868.2f, 250.0f, 7, 5, LORA_SYNCWORD, 10, 12, RADIO_SX1262_TCXO_V);
        break;
      default:
        return false;
    }
    if (st != RADIOLIB_ERR_NONE) {
      ESP_LOGW(TAG, "LoRa begin failed, code %d", st);
      return false;
    }
    if (lora_module != RADIO_MODULE_SX1276) {
      radio_phy->setOutputPower(22);
      static_cast<SX126x*>(radio_phy)->setRegulatorDCDC();
    }
    configure_lora_radio(false);  // resets radio_mode and restarts RX
    return true;
  }

  // Shared GFSK PHY of FLARM (GXAirCom lib/FANETLORA/radio/LoRa.cpp) and ADS-L
  // (EASA spec C.2): 2-GFSK, 100 kbps, +-50 kHz deviation, BT 0.5, 8 bit
  // preamble, 6 byte sync word, Manchester encoded payload, no hardware CRC and
  // no whitening. Sync word and frame length are set per protocol below.
  int16_t state = RADIOLIB_ERR_UNKNOWN;

  switch (lora_module) {
    case RADIO_MODULE_SX1276:
      state = radio_sx1276.beginFSK(freq, 100.0f, 50.0f, 125.0f, 10, 1, false);
      break;
    case RADIO_MODULE_LLCC68:
      state = radio_llcc68.beginFSK(freq, 100.0f, 50.0f, 117.3f, 10, 8, RADIO_LLCC68_TCXO_V, false);
      break;
    case RADIO_MODULE_SX1262:
      state = radio_sx1262.beginFSK(freq, 100.0f, 50.0f, 117.3f, 10, 8, RADIO_SX1262_TCXO_V, false);
      break;
    default:
      return false;
  }
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGW(TAG, "beginFSK failed, code %d", state);
    return false;
  }

  if (lora_module != RADIO_MODULE_SX1276) {
    static_cast<SX126x*>(radio_phy)->setRegulatorDCDC();
  }

  radio_phy->setDataShaping(RADIOLIB_SHAPING_0_5);

  // CRC and whitening are module specific and not part of PhysicalLayer. Both
  // payload CRCs are checked in software because the hardware engine cannot see
  // through the Manchester encoding.
  switch (lora_module) {
    case RADIO_MODULE_SX1276:
      radio_sx1276.setCRC(false);
      break;
    case RADIO_MODULE_LLCC68:
      radio_llcc68.setCRC(0);
      radio_llcc68.setWhitening(false);
      break;
    case RADIO_MODULE_SX1262:
      radio_sx1262.setCRC(0);
      radio_sx1262.setWhitening(false);
      break;
    default:
      break;
  }

  radio_apply_fsk_protocol(mode);

  radio_mode = mode;
  receivedFlag = false;
  radio_phy->startReceive();
  return true;
}

static void radio_set_mode(RadioMode mode) {
  if (mode == radio_mode) return;
  uint32_t t0 = (uint32_t)micros();
  if (!radio_apply_mode(mode)) {
    // Leave the radio on FANET rather than in an unknown state.
    if (mode != RADIO_MODE_FANET) radio_apply_mode(RADIO_MODE_FANET);
    return;
  }
  uint32_t dt = (uint32_t)micros() - t0;
  if (dt > flarm_switch_us_max) flarm_switch_us_max = dt;
}

void flarm_scheduler_tick() {
  const bool wantFlarm = flarm_enabled();
  const bool wantAdsl = adsl_enabled();

  // Nothing on FSK to listen for: park on LoRa. That also covers the case where
  // every protocol is switched off - the radio then simply idles on FANET.
  if (!wantFlarm && !wantAdsl) {
    if (radio_mode != RADIO_MODE_FANET) radio_set_mode(RADIO_MODE_FANET);
    return;
  }

  // Debug mode: stay on FSK permanently and just alternate the two channels, so
  // a transmitter under development is heard regardless of whether it respects
  // the FLARM time slots. This gives up FANET reception while it is active.
  if (flarm_verbose_ws || adsl_verbose_ws) {
    const bool debugAdsl = adsl_verbose_ws && wantAdsl;
    const RadioMode lowMode = debugAdsl ? RADIO_MODE_ADSL_LOW : RADIO_MODE_FLARM_LOW;
    const RadioMode highMode = debugAdsl ? RADIO_MODE_ADSL_HIGH : RADIO_MODE_FLARM_HIGH;
    uint32_t now = millis();
    if (radio_mode != lowMode && radio_mode != highMode) {
      radio_set_mode(lowMode);
      fsk_blind_switch_ms = now;
    } else if (now - fsk_blind_switch_ms >= 1000) {
      radio_set_mode(radio_mode == lowMode ? highMode : lowMode);
      fsk_blind_switch_ms = now;
    }
    return;
  }

  const uint8_t* periods = fsk_periods();
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  const bool timeValid = tv.tv_sec > 1600000000;  // clock has seen NTP
  const uint32_t msInSec = timeValid ? (uint32_t)(tv.tv_usec / 1000) : 0;

  // With FANET switched off there is no budget to divide: the radio stays on FSK
  // permanently and only the channels (and, if both are on, the protocols) take
  // turns. No LoRa modem switches happen at all in this mode.
  if (!fanet_enabled()) {
    bool pickAdsl = wantAdsl;
    if (wantFlarm && wantAdsl) {
      uint32_t block = timeValid ? (uint32_t)(tv.tv_sec / 2) : (millis() / 2000);
      pickAdsl = (block & 1) != 0;
    }

    if (pickAdsl) {
      if (millis() - fsk_blind_switch_ms >= ADSL_CHANNEL_MS || !mode_is_adsl(radio_mode)) {
        radio_set_mode(radio_mode == RADIO_MODE_ADSL_LOW ? RADIO_MODE_ADSL_HIGH
                                                         : RADIO_MODE_ADSL_LOW);
        fsk_blind_switch_ms = millis();
      }
    } else if (timeValid) {
      const bool high =
          (msInSec >= FLARM_HIGH_BEGIN_MS) || (msInSec < (FLARM_WINDOW_END_MS - 1000));
      radio_set_mode(high ? RADIO_MODE_FLARM_HIGH : RADIO_MODE_FLARM_LOW);
    } else {
      if (millis() - fsk_blind_switch_ms >= FSK_BLIND_WINDOW_MS || !mode_is_flarm(radio_mode)) {
        radio_set_mode(radio_mode == RADIO_MODE_FLARM_LOW ? RADIO_MODE_FLARM_HIGH
                                                          : RADIO_MODE_FLARM_LOW);
        fsk_blind_switch_ms = millis();
      }
    }
    return;
  }

  // Pick the protocol for the upcoming window. With both enabled they alternate,
  // so the combined time away from FANET stays within the duty profile.
  const bool useAdsl = wantAdsl && (!wantFlarm || fsk_next_is_adsl);

  // Without a valid clock there is no second boundary to aim at. ADS-L does not
  // need one anyway; FLARM then degrades to a blind sweep.
  if (!timeValid) {
    uint32_t now = millis();
    uint32_t elapsed = now - fsk_blind_switch_ms;
    uint8_t profile = settings.flarmDutyProfile <= 2 ? settings.flarmDutyProfile : 0;
    const RadioMode lowMode = useAdsl ? RADIO_MODE_ADSL_LOW : RADIO_MODE_FLARM_LOW;
    const RadioMode highMode = useAdsl ? RADIO_MODE_ADSL_HIGH : RADIO_MODE_FLARM_HIGH;
    if (radio_mode == RADIO_MODE_FANET) {
      if (elapsed >= FSK_BLIND_LORA_MS[profile]) {
        radio_set_mode(lowMode);
        fsk_blind_switch_ms = now;
      }
    } else if (elapsed >= FSK_BLIND_WINDOW_MS) {
      if (radio_mode == lowMode) {
        radio_set_mode(highMode);
      } else {
        radio_set_mode(RADIO_MODE_FANET);
        fsk_next_is_adsl = !fsk_next_is_adsl;
      }
      fsk_blind_switch_ms = now;
    }
    return;
  }

  const uint32_t now = millis();

  switch (radio_mode) {
    case RADIO_MODE_FANET: {
      if (tv.tv_sec < fsk_next_cycle) return;
      if (useAdsl) {
        // No transmit slots to hit, so start as soon as the cycle is due.
        fsk_anchor_ms = now;
        fsk_anchor_sec = tv.tv_sec;
        radio_set_mode(RADIO_MODE_ADSL_LOW);
        break;
      }
      // Only start at the head of the low window. Entering late would catch a
      // fragment of it, and simply waiting for the next second costs nothing
      // once the cycle is already due.
      if (msInSec < FLARM_LOW_BEGIN_MS || msInSec >= FLARM_HIGH_BEGIN_MS) return;
      fsk_anchor_ms = now - msInSec;
      fsk_anchor_sec = tv.tv_sec;
      {
        uint32_t late = msInSec - FLARM_LOW_BEGIN_MS;
        if (late > flarm_entry_late_ms_max) flarm_entry_late_ms_max = late;
      }
      radio_set_mode(RADIO_MODE_FLARM_LOW);
      break;
    }

    case RADIO_MODE_FLARM_LOW:
      if (now - fsk_anchor_ms >= FLARM_HIGH_BEGIN_MS) {
        radio_set_mode(RADIO_MODE_FLARM_HIGH);
      }
      break;

    case RADIO_MODE_ADSL_LOW:
      if (now - fsk_anchor_ms >= ADSL_CHANNEL_MS) {
        radio_set_mode(RADIO_MODE_ADSL_HIGH);
      }
      break;

    case RADIO_MODE_FLARM_HIGH:
    case RADIO_MODE_ADSL_HIGH: {
      const uint32_t windowEnd =
          mode_is_adsl(radio_mode) ? ADSL_WINDOW_END_MS : FLARM_WINDOW_END_MS;
      if (now - fsk_anchor_ms >= windowEnd) {
        radio_set_mode(RADIO_MODE_FANET);
        // Measured from the window's own second, not from now: a FLARM window
        // ends 260 ms into the following second, so using tv.tv_sec here would
        // silently stretch every period by one second.
        fsk_next_cycle = fsk_anchor_sec + periods[fsk_period_idx];
        fsk_period_idx = (fsk_period_idx + 1) % 3;
        fsk_next_is_adsl = !fsk_next_is_adsl;
      }
      break;
    }
  }
}

// How long the scheduler may be left alone. This is the whole point of the task
// rewrite: when only FANET is active the answer is "until a packet arrives", and
// the CPU can stay in light sleep for seconds at a time.
uint32_t flarm_scheduler_next_delay_ms() {
  if (radio_phy == nullptr) return UINT32_MAX;

  const bool wantFlarm = flarm_enabled();
  const bool wantAdsl = adsl_enabled();
  if (!wantFlarm && !wantAdsl) return UINT32_MAX;

  if (flarm_verbose_ws || adsl_verbose_ws) {
    uint32_t elapsed = millis() - fsk_blind_switch_ms;
    return elapsed >= 1000 ? 1 : (1000 - elapsed);
  }

  struct timeval tv;
  gettimeofday(&tv, nullptr);
  const bool timeValid = tv.tv_sec > 1600000000;
  const uint32_t now = millis();

  auto remaining = [](uint32_t deadline, uint32_t now_ms) -> uint32_t {
    return (int32_t)(deadline - now_ms) > 0 ? (deadline - now_ms) : 1;
  };

  if (!fanet_enabled()) {
    if (mode_is_adsl(radio_mode)) return remaining(fsk_blind_switch_ms + ADSL_CHANNEL_MS, now);
    if (timeValid) {
      // Channel follows the FLARM slot boundaries each second. The tick keeps
      // the high channel until FLARM_WINDOW_END_MS has passed into the next
      // second, so that overrun is the first boundary to aim at.
      const uint32_t msInSec = (uint32_t)(tv.tv_usec / 1000);
      const uint32_t highOverrun = FLARM_WINDOW_END_MS - 1000;
      uint32_t nextBoundary;
      if (msInSec < highOverrun) {
        nextBoundary = highOverrun - msInSec;  // high -> low
      } else if (msInSec < FLARM_HIGH_BEGIN_MS) {
        nextBoundary = FLARM_HIGH_BEGIN_MS - msInSec;  // low -> high
      } else {
        nextBoundary = 1000 - msInSec + highOverrun;  // high, across the second
      }
      return nextBoundary > 0 ? nextBoundary : 1;
    }
    return remaining(fsk_blind_switch_ms + FSK_BLIND_WINDOW_MS, now);
  }

  if (!timeValid) {
    uint8_t profile = settings.flarmDutyProfile <= 2 ? settings.flarmDutyProfile : 0;
    uint32_t window = radio_mode == RADIO_MODE_FANET ? FSK_BLIND_LORA_MS[profile]
                                                     : FSK_BLIND_WINDOW_MS;
    return remaining(fsk_blind_switch_ms + window, now);
  }

  switch (radio_mode) {
    case RADIO_MODE_FANET: {
      const uint32_t msInSec = (uint32_t)(tv.tv_usec / 1000);
      int64_t secondsToGo = (int64_t)fsk_next_cycle - (int64_t)tv.tv_sec;
      if (secondsToGo > 0) {
        // Aim at the start of the low window in the cycle's second.
        int64_t ms = secondsToGo * 1000 + (int64_t)FLARM_LOW_BEGIN_MS - (int64_t)msInSec;
        if (ms < 1) ms = 1;
        return (uint32_t)(ms > 1000 ? 1000 : ms);  // re-check at least every second
      }
      // Cycle is due: wait for the window to open, or retry next second.
      if (msInSec < FLARM_LOW_BEGIN_MS) return FLARM_LOW_BEGIN_MS - msInSec;
      if (msInSec >= FLARM_HIGH_BEGIN_MS) return (1000 - msInSec) + FLARM_LOW_BEGIN_MS;
      return 1;
    }
    case RADIO_MODE_FLARM_LOW:
      return remaining(fsk_anchor_ms + FLARM_HIGH_BEGIN_MS, now);
    case RADIO_MODE_ADSL_LOW:
      return remaining(fsk_anchor_ms + ADSL_CHANNEL_MS, now);
    case RADIO_MODE_FLARM_HIGH:
      return remaining(fsk_anchor_ms + FLARM_WINDOW_END_MS, now);
    case RADIO_MODE_ADSL_HIGH:
      return remaining(fsk_anchor_ms + ADSL_WINDOW_END_MS, now);
  }
  return 10;
}

// ── FLARM / ADS-L frame debugging ───────────────────────────────────────────
static String flarm_hex(const uint8_t* buf, size_t len) {
  String out;
  out.reserve(len * 3);
  char b[4];
  for (size_t i = 0; i < len; i++) {
    snprintf(b, sizeof(b), "%02X ", buf[i]);
    out += b;
  }
  out.trim();
  return out;
}

// Dumps everything an encoder could get wrong, in the order the receive chain
// checks it: on-air bytes, Manchester result, CRC and the plaintext header.
static void flarm_verbose_dump(const uint8_t* raw, const uint8_t* payload, int rssi) {
  flarm_debug_print(String("FLARM raw(52) rssi=") + rssi + "dBm: " + flarm_hex(raw, FLARM_RAW_LEN));
  flarm_debug_print(String("FLARM manchester-decoded(26): ") +
                    flarm_hex(payload, FLARM_PAYLOAD_LEN));

  uint16_t received = ((uint16_t)payload[24] << 8) | payload[25];
  uint16_t computed = flarm_crc(payload);
  char hdr[192];
  snprintf(hdr, sizeof(hdr),
           "FLARM header: addr=%02X%02X%02X byte3=0x%02X (type=%u addr_type=%u) crc rx=%04X "
           "calc=%04X %s",
           payload[2], payload[1], payload[0], payload[3], payload[3] & 0x0F,
           (payload[3] >> 4) & 0x07, received, computed, (received == computed) ? "OK" : "MISMATCH");
  flarm_debug_print(hdr);
}

void handle_flarm() {
  if (radio_phy == nullptr || !receivedFlag) {
    return;
  }
  receivedFlag = false;

  uint8_t raw[FLARM_RAW_LEN];
  int state = radio_phy->readData(raw, FLARM_RAW_LEN);
  radio_phy->startReceive();  // keep listening for the rest of the window
  if (state != RADIOLIB_ERR_NONE) {
    return;
  }
  flarm_rx_frames++;

  uint8_t payload[FLARM_PAYLOAD_LEN];
  flarm_manchester_decode(raw, payload);

  if (flarm_verbose_ws) {
    flarm_verbose_dump(raw, payload, (int)radio_phy->getRSSI());
  }

  // A failing CRC means the radio parameters are right but the frame was noise
  // or a collision; a failing decode points at the key schedule instead. Keeping
  // the two apart is what makes a field problem diagnosable.
  if (!flarm_check_crc(payload)) {
    flarm_crc_errors++;
    bd_stats_fsk_error();
    return;
  }

  FlarmTarget target;
  if (!flarm_decode_payload(payload, settings.latitude, settings.longitude, time(nullptr),
                            &target)) {
    flarm_decode_fail++;
    bd_stats_fsk_error();
    if (flarm_verbose_ws) {
      flarm_debug_print(String("FLARM decode failed: ") +
                        (flarm_debug_note[0] ? flarm_debug_note
                                             : "no matching timestamp in +/-2 s"));
    }
    return;
  }
  flarm_decoded++;
  bd_stats_rx(BD_STAT_FLARM, NAN, radio_phy->getRSSI());

  if (flarm_verbose_ws) {
    char line[192];
    snprintf(line, sizeof(line),
             "FLARM ok v%u addr=%06X type=%u acft=%u %.5f/%.5f alt=%.0fm spd=%.0fkm/h climb=%.1f "
             "crs=%.0f%s%s",
             target.protoVersion, (unsigned)target.addr, target.addrType, target.aircraftType,
             target.lat, target.lon, target.altitude, target.speed, target.climb, target.course,
             target.noTrack ? " NOTRACK" : "", target.stealth ? " STEALTH" : "");
    flarm_debug_print(line);
  }

  trackingData td;
  td.vid = (uint8_t)(target.addr >> 16);
  td.fanet_id = (uint16_t)(target.addr & 0xFFFF);
  td.devId = FANET2String(td.vid, td.fanet_id);
  td.rssi = (int)radio_phy->getRSSI();
  td.snr = 0;  // GFSK has no SNR figure
  td.timestamp = time(nullptr);
  td.lat = target.lat;
  td.lon = target.lon;
  td.alt = target.altitude;
  td.hdop = 0;
  td.aircraftType = flarm_to_fanet_acft(target.aircraftType);
  td.adressType = flarm_origin_prefix(target.addrType);
  td.speed = target.speed;
  td.climb = target.climb;
  td.heading = target.course;
  td.state = target.onGround ? state_Other : state_Flying;
  td.srcProto = TRACK_SRC_FLARM;
  // Both flags mean "do not publish me"; keep them locally for debugging but
  // never forward them.
  td.noTrack = target.noTrack || target.stealth;
  td.onlineTracking = !td.noTrack;
  td.nativeAircraftType = target.aircraftType;

  store_lock();
  int i = storeTrackingData(td);
  bool publish = (millis() - trackingStore[i].last_send) > 800;  // filter forwarded duplicates
  String json;
  if (publish) {
    trackingStore[i].last_send = millis();
    json = packTracking(&trackingStore[i]);
  }
  store_unlock();

  if (publish) {
    bd_ws_broadcast(json);
    if (!td.noTrack && aprs.connected()) {
      aprs_lock();
      aprs.sendTrackingData(&td);
      aprs_unlock();
    }
  }
}

// Dumps an ADS-L frame at every stage an encoder could get wrong.
static void adsl_verbose_dump(const uint8_t* raw, const uint8_t* frame, int rssi) {
  flarm_debug_print(String("ADSL raw(48) rssi=") + rssi + "dBm: " + flarm_hex(raw, ADSL_RAW_LEN));
  flarm_debug_print(String("ADSL manchester-decoded(24): ") + flarm_hex(frame, ADSL_FRAME_LEN));
  // Show both values, not just OK/MISMATCH: when they differ, the difference is
  // what tells a sender author whether the coverage or the algorithm is off.
  uint32_t rxCrc = ((uint32_t)frame[21] << 16) | ((uint32_t)frame[22] << 8) | frame[23];
  uint32_t calcCrc = adsl_crc_expected(frame);
  char hdr[192];
  snprintf(hdr, sizeof(hdr),
           "ADSL header: version=0x%02X (proto=%u sig=%u key=%u) crc rx=%06X calc=%06X %s", frame[0],
           frame[0] & 0x0F, (frame[0] >> 4) & 1, (frame[0] >> 5) & 3, (unsigned)rxCrc,
           (unsigned)calcCrc, adsl_check_crc(frame) ? "OK" : "MISMATCH");
  flarm_debug_print(hdr);
}

void handle_adsl() {
  if (radio_phy == nullptr || !receivedFlag) {
    return;
  }
  receivedFlag = false;

  uint8_t raw[ADSL_RAW_LEN];
  int state = radio_phy->readData(raw, ADSL_RAW_LEN);
  radio_phy->startReceive();  // keep listening for the rest of the window
  if (state != RADIOLIB_ERR_NONE) {
    return;
  }
  adsl_rx_frames++;

  uint8_t frame[ADSL_FRAME_LEN];
  adsl_manchester_decode(raw, frame);

  if (adsl_verbose_ws) {
    adsl_verbose_dump(raw, frame, (int)radio_phy->getRSSI());
  }

  const bool crcOk = adsl_check_crc(frame);
  if (!crcOk) {
    adsl_crc_errors++;
    bd_stats_fsk_error();
    // In debug mode keep going: the payload has enough internal structure
    // (message type, address table, plausible position) to tell a broken CRC
    // apart from a broken payload. Such frames are never stored or forwarded.
    if (!adsl_verbose_ws) return;
  }

  AdslTarget target;
  if (!adsl_decode_frame(frame, &target)) {
    adsl_decode_fail++;
    // A frame whose CRC already failed was counted above.
    if (crcOk) bd_stats_fsk_error();
    if (adsl_verbose_ws) {
      flarm_debug_print(String("ADSL decode failed: ") +
                        (adsl_debug_note[0] ? adsl_debug_note : "unknown reason"));
    }
    return;
  }
  adsl_decoded++;

  trackingData td;
  td.vid = (uint8_t)(target.addr >> 16);
  td.fanet_id = (uint16_t)(target.addr & 0xFFFF);
  td.devId = FANET2String(td.vid, td.fanet_id);
  td.rssi = (int)radio_phy->getRSSI();
  td.snr = 0;  // GFSK has no SNR figure
  td.timestamp = time(nullptr);
  td.lat = target.lat;
  td.lon = target.lon;
  td.alt = target.altitude;
  td.hdop = 0;
  td.aircraftType = adsl_to_fanet_acft(target.aircraftCat);
  td.adressType = adsl_origin_prefix(target.addrTable);
  td.speed = target.speed;
  td.climb = target.climb;
  td.heading = target.course;
  // Flight state 1 is explicitly "on ground"; 0 means the sender did not say.
  td.state = (target.flightState == 1) ? state_Other : state_Flying;
  td.srcProto = TRACK_SRC_ADSL;
  // ADS-L has no stealth or no-track flag - privacy is handled by using a
  // random address instead, so there is nothing to filter here.
  td.noTrack = false;
  td.onlineTracking = true;
  td.nativeAircraftType = target.aircraftCat;

  if (adsl_verbose_ws) {
    char line[208];
    snprintf(line, sizeof(line),
             "ADSL %s addr=%06X table=%u cat=%u %.5f/%.5f alt=%.0fm spd=%.0fkm/h climb=%.1f "
             "crs=%.0f%s",
             crcOk ? "ok" : "payload ok but CRC BAD (not stored)", (unsigned)target.addr,
             target.addrTable, target.aircraftCat, target.lat, target.lon, target.altitude,
             target.speed, target.climb, target.course, target.relayed ? " RELAYED" : "");
    flarm_debug_print(line);
  }

  // Only a CRC-verified frame is trustworthy enough to publish.
  if (!crcOk) return;
  bd_stats_rx(BD_STAT_ADSL, NAN, td.rssi);

  store_lock();
  int i = storeTrackingData(td);
  bool publish = (millis() - trackingStore[i].last_send) > 800;
  String json;
  if (publish) {
    trackingStore[i].last_send = millis();
    json = packTracking(&trackingStore[i]);
  }
  store_unlock();

  if (publish) {
    bd_ws_broadcast(json);
    if (aprs.connected()) {
      aprs_lock();
      aprs.sendTrackingData(&td);
      aprs_unlock();
    }
  }
}

#else  // CONFIG_BDGS_ENABLE_FSK_RX

// FLARM and ADS-L are compiled out on flash constrained targets. The rest of the
// firmware keeps calling these, so they stay as no-ops rather than being
// #ifdef'd at every call site.
bool flarm_enabled() { return false; }
bool adsl_enabled() { return false; }
bool fanet_enabled() { return true; }
void flarm_scheduler_tick() {}
uint32_t flarm_scheduler_next_delay_ms() { return UINT32_MAX; }

uint32_t flarm_rx_frames = 0, flarm_crc_errors = 0, flarm_decode_fail = 0, flarm_decoded = 0;
uint32_t adsl_rx_frames = 0, adsl_crc_errors = 0, adsl_decode_fail = 0, adsl_decoded = 0;
uint32_t flarm_switch_us_max = 0, flarm_entry_late_ms_max = 0;
bool flarm_verbose_ws = false;
bool adsl_verbose_ws = false;

static bool mode_is_adsl(RadioMode) { return false; }

#endif  // CONFIG_BDGS_ENABLE_FSK_RX

void handle_fanet() {
  if (radio_phy == nullptr) {
    return;
  }

#if CONFIG_BDGS_ENABLE_FSK_RX
  if (radio_mode != RADIO_MODE_FANET) {
    if (mode_is_adsl(radio_mode))
      handle_adsl();
    else
      handle_flarm();
    return;
  }
#endif

  if (!receivedFlag) return;
  receivedFlag = false;

  int numBytes = radio_phy->getPacketLength();
  if (numBytes <= 0 || numBytes > 255) {
    radio_phy->startReceive();
    return;
  }

  uint8_t byteArr[256];
  int state = radio_phy->readData(byteArr, numBytes);
  radio_phy->startReceive();

  if (state == RADIOLIB_ERR_NONE) {
    fanet_rx_count++;
    bd_stats_rx(BD_STAT_FANET, radio_phy->getSNR(), radio_phy->getRSSI());
  } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
    ESP_LOGD(TAG, "FANET CRC error");
    bd_stats_crc_error();
    return;
  } else {
    ESP_LOGD(TAG, "FANET read failed, code %d", state);
    return;
  }

  bd_board_led_blink(20);
  bd_repeat_note(byteArr, (size_t)numBytes, radio_phy->getSNR(), radio_phy->getRSSI());

  fanet_header* header = (fanet_header*)byteArr;
  bd_diag_note_packet(header->vendor, header->address, header->type, radio_phy->getRSSI(), radio_phy->getSNR());
  if (header->type == FANET_PCK_TYPE_WEATHER) {
    weatherData wd;
    if (unpack_weatherdata(byteArr, (size_t)numBytes, &wd, radio_phy->getSNR(),
                           radio_phy->getRSSI())) {
      store_lock();
      int i = storeWeatherData(wd);
      bool publish = (millis() - weatherStore[i].last_send) > 3000;  // filter duplicates
      String json;
      float lat = 0, lon = 0;
      if (publish) {
        weatherStore[i].last_send = millis() + 2;
        json = packWeather(&weatherStore[i]);
        lat = weatherStore[i].lat;
        lon = weatherStore[i].lon;
      }
      weatherData forwardCopy;
      if (publish) forwardCopy.assign(weatherStore[i]);
      store_unlock();

      if (publish) {
        bd_ws_broadcast(json);

        // check if distance is within 300km
        if (distance(settings.latitude, settings.longitude, lat, lon) < 300) {
          if (wifiConnected && settings.sendBreezedude) {
            send_wd_to_frontend(&forwardCopy);
          }
          if (settings.sendAPRS && aprs.connected()) {
            aprs_lock();
            bool sent = aprs.sendWeatherData(&wd);
            aprs_unlock();
            if (!sent) ESP_LOGW(TAG, "APRS submit weather failed");
          }
        }
      }
    } else {
      bd_stats_decode_fail();
    }
  }

  else if (header->type == FANET_PCK_TYPE_NAME) {
    char name[64];
    size_t nameLen = (size_t)numBytes > 4 ? (size_t)numBytes - 4 : 0;
    if (nameLen > sizeof(name) - 1) nameLen = sizeof(name) - 1;
    memset(name, '\0', sizeof(name));
    memcpy(name, &byteArr[4], nameLen);
    setDeviceName(header->vendor, header->address, name);

    // filter forwarded duplicates (3s window per device)
    static struct {
      uint8_t vid;
      uint16_t fid;
      uint32_t last_send;
    } nameDedup[MAX_DEVICES] = {};
    uint32_t now_ms = millis();
    int slot = -1, free_slot = -1;
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (nameDedup[i].last_send && nameDedup[i].vid == header->vendor &&
          nameDedup[i].fid == header->address) {
        slot = i;
        break;
      }
      if (!nameDedup[i].last_send && free_slot < 0) free_slot = i;
    }
    bool name_is_dup = (slot >= 0) && (now_ms - nameDedup[slot].last_send < 3000);
    if (!name_is_dup) {
      int use = (slot >= 0) ? slot : free_slot;
      if (use >= 0) nameDedup[use] = {(uint8_t)header->vendor, (uint16_t)header->address, now_ms};
      if (settings.sendAPRS && aprs.connected()) {
        aprs_lock();
        aprs.sendNameData(FANET2String(header->vendor, header->address), name,
                          radio_phy->getSNR());
        aprs_unlock();
      }
    }
  }

  else if (header->type == FANET_PCK_TYPE_HW_INFO) {
    hwInfoData hi;
    if (unpack_hwinfo_t0a(byteArr, numBytes, &hi, radio_phy->getRSSI(), radio_phy->getSNR())) {
      store_lock();
      int hi_idx = storeHwInfoData(hi);
      bool publish = (millis() - hwInfoStore[hi_idx].last_send) > 3000;
      if (publish) hwInfoStore[hi_idx].last_send = millis();
      store_unlock();

      if (publish) {
        bd_ws_broadcast(packHwInfo(&hi));
        if (wifiConnected && settings.sendBreezedude) {
          send_hwinfo_to_frontend(&hi);
        }
        // Only HW_INFO type 2 guarantees the sender remains in RX mode afterwards.
        if (hi.rawAfterBuildDate[0] == 2) {
          ota_gs_try_update(hi);
          config_gs_try_update(hi);
        }
      }
    } else {
      bd_stats_decode_fail();
    }
  }

  else if (header->type == FANET_PCK_TYPE_TRACKING) {
    trackingData td;
    if (unpack_trackingdata(byteArr, &td, radio_phy->getSNR(), radio_phy->getRSSI())) {
      store_lock();
      int i = storeTrackingData(td);  // sort and get position
      bool publish = (millis() - trackingStore[i].last_send) > 800;
      String json;
      if (publish) {
        trackingStore[i].last_send = millis();
        json = packTracking(&trackingStore[i]);  // complete set including name
      }
      store_unlock();

      if (publish) {
        bd_ws_broadcast(json);
        if (aprs.connected()) {
          aprs_lock();
          aprs.sendTrackingData(&td);
          aprs_unlock();
        }
      }
    } else {
      bd_stats_decode_fail();
    }
  }

  else if (header->type == FANET_PCK_TYPE_GROUND_TRACKING) {
    trackingData td;
    if (unpack_ground_trackingdata(byteArr, &td, radio_phy->getSNR(), radio_phy->getRSSI())) {
      store_lock();
      int i = storeTrackingData(td);
      bool publish = (millis() - trackingStore[i].last_send) > 800;
      String json;
      if (publish) {
        trackingStore[i].last_send = millis();
        json = packTracking(&trackingStore[i]);
      }
      store_unlock();

      if (publish) bd_ws_broadcast(json);
    } else {
      bd_stats_decode_fail();
    }
  }
}

// Samples the channel RSSI between packets: the noise floor the receiver has to
// beat. Only in FANET (LoRa) mode, and only while no packet is pending - a
// sample taken during a reception would measure that packet instead.
static volatile float s_noise_floor_dbm = NAN;

static void sample_noise_floor() {
  static uint32_t lastSampleMs = 0;
  if (radio_phy == nullptr || radio_mode != RADIO_MODE_FANET || receivedFlag) return;
  if (lastSampleMs != 0 && millis() - lastSampleMs < 10000) return;
  lastSampleMs = millis();

  float rssi = NAN;
  switch (lora_module) {
    case RADIO_MODULE_SX1276:
      rssi = radio_sx1276.getRSSI(false, true);
      break;
    case RADIO_MODULE_LLCC68:
      rssi = radio_llcc68.getRSSI(false);
      break;
    case RADIO_MODULE_SX1262:
      rssi = radio_sx1262.getRSSI(false);
      break;
    default:
      return;
  }
  bd_stats_noise(rssi);
  if (!isfinite(rssi)) return;
  s_noise_floor_dbm = isnan(s_noise_floor_dbm) ? rssi : s_noise_floor_dbm + 0.1f * (rssi - s_noise_floor_dbm);
}

float bd_radio_noise_floor() { return s_noise_floor_dbm; }

// --- task -------------------------------------------------------------------
void bd_radio_lock() {
  if (s_radio_mutex) xSemaphoreTakeRecursive(s_radio_mutex, portMAX_DELAY);
}

void bd_radio_unlock() {
  if (s_radio_mutex) xSemaphoreGiveRecursive(s_radio_mutex);
}

void bd_radio_notify() {
  if (s_radio_task) xTaskNotifyGive(s_radio_task);
}

static void radio_task(void*) {
  for (;;) {
    uint32_t next = flarm_scheduler_next_delay_ms();
    const uint32_t repeatNext = bd_repeat_next_delay_ms();
    if (repeatNext < next) next = repeatNext;
    // Without an FSK schedule there is nothing to wake up for except a packet.
    // The ceiling only exists so a settings change is picked up even if the
    // notification was missed.
    TickType_t wait = pdMS_TO_TICKS(next == UINT32_MAX ? 10000 : next);
    if (wait == 0) wait = 1;

    receivedFlag.arm();
    ulTaskNotifyTake(pdTRUE, wait);

    bd_radio_lock();
    handle_fanet();
    bd_repeat_tick();
    flarm_scheduler_tick();
    sample_noise_floor();
    bd_radio_unlock();
  }
}

bool bd_radio_start() {
  s_radio_mutex = xSemaphoreCreateRecursiveMutex();

  if (!init_lora_radio()) {
    ESP_LOGW(TAG, "no radio module found - continuing without LoRa");
    return false;
  }

  int state = radio_phy->startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "startReceive failed, code %d", state);
    radio_phy = nullptr;
    return false;
  }

  irq_pin_setup();
  radio_pins_keep_state_in_sleep();

  // Priority above the network stack: a missed FLARM window cannot be caught up,
  // a delayed HTTP response can.
  // The LoRa OTA gateway (ota_gs, config_gs) runs inside this task: backend
  // queries, manifest JSON and the whole sensor transfer. 5 KB overflowed.
  xTaskCreate(radio_task, "radio", 8192, nullptr, 6, &s_radio_task);
  return true;
}
