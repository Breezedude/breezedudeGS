// The password is stored as a salted, iterated SHA-256 in the settings, never in
// clear. Sessions live in RTC memory, which a software restart keeps: a firmware
// update or a restart from the web interface does not log anyone out. The RTC
// clock keeps running across such a restart too, a power cycle clears both.
#include "bd_auth.h"

#include <ArduinoJson.h>
#include <string.h>

#include "bd_state.h"
#include "bd_web.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rtc_time.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/sha256.h"

static const char* TAG = "auth";

static constexpr int kSessions = 8;
static constexpr int kPending = 4;
static constexpr int kHashRounds = 1000;
// After this many wrong passwords in a row, logins wait this long.
static constexpr int kFailLimit = 5;
static constexpr uint32_t kFailLockS = 60;
static constexpr uint32_t kMagic = 0xB0A7B0A7;

struct Session {
  char token[BD_AUTH_TOKEN_LEN + 1];
  uint32_t expires_s;
};

static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR Session s_sessions[kSessions];

struct Pending {
  int fd;
  uint32_t expires_s;
};
static Pending s_pending[kPending];

static int s_fails = 0;
static uint32_t s_locked_until_s = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_s() { return (uint32_t)(esp_rtc_get_time_us() / 1000000ULL); }

static void init_sessions() {
  if (s_magic == kMagic) return;
  memset(s_sessions, 0, sizeof(s_sessions));
  s_magic = kMagic;
}

static void hash_password(const char* password, const uint8_t salt[16], uint8_t out[32]) {
  uint8_t digest[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, salt, 16);
  mbedtls_sha256_update(&ctx, (const uint8_t*)password, strlen(password));
  mbedtls_sha256_finish(&ctx, digest);
  for (int i = 1; i < kHashRounds; i++) {
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, salt, 16);
    mbedtls_sha256_update(&ctx, digest, sizeof(digest));
    mbedtls_sha256_finish(&ctx, digest);
  }
  mbedtls_sha256_free(&ctx);
  memcpy(out, digest, sizeof(digest));
}

static bool all_zero(const uint8_t* p, size_t n) {
  uint8_t acc = 0;
  for (size_t i = 0; i < n; i++) acc |= p[i];
  return acc == 0;
}

bool bd_auth_required() {
  settings_lock();
  const bool set = !all_zero(settings.passHash, sizeof(settings.passHash));
  settings_unlock();
  return set;
}

static void new_session(char* token) {
  uint8_t raw[BD_AUTH_TOKEN_LEN / 2];
  esp_fill_random(raw, sizeof(raw));
  for (size_t i = 0; i < sizeof(raw); i++) snprintf(token + 2 * i, 3, "%02x", raw[i]);

  portENTER_CRITICAL(&s_mux);
  init_sessions();
  const uint32_t now = now_s();
  int slot = 0;
  for (int i = 0; i < kSessions; i++) {
    // A free or expired one, else the one closest to expiring.
    if (now >= s_sessions[i].expires_s) {
      slot = i;
      break;
    }
    if (s_sessions[i].expires_s < s_sessions[slot].expires_s) slot = i;
  }
  memcpy(s_sessions[slot].token, token, BD_AUTH_TOKEN_LEN + 1);
  s_sessions[slot].expires_s = now + BD_AUTH_SESSION_S;
  portEXIT_CRITICAL(&s_mux);
}

bool bd_auth_login(const char* password, char* token, uint32_t* retry_s) {
  const uint32_t now = now_s();
  *retry_s = 0;
  if (now < s_locked_until_s) {
    *retry_s = s_locked_until_s - now;
    return false;
  }

  uint8_t salt[16], stored[32], actual[32];
  settings_lock();
  memcpy(salt, settings.passSalt, sizeof(salt));
  memcpy(stored, settings.passHash, sizeof(stored));
  settings_unlock();
  hash_password(password ? password : "", salt, actual);

  uint8_t diff = 0;
  for (size_t i = 0; i < sizeof(actual); i++) diff |= actual[i] ^ stored[i];
  if (diff != 0 || all_zero(stored, sizeof(stored))) {
    if (++s_fails >= kFailLimit) {
      s_fails = 0;
      s_locked_until_s = now + kFailLockS;
      *retry_s = kFailLockS;
      ESP_LOGW(TAG, "%d wrong passwords, logins paused for %lu s", kFailLimit, (unsigned long)kFailLockS);
    }
    return false;
  }
  s_fails = 0;
  new_session(token);
  return true;
}

bool bd_auth_allowed(const char* token, uint32_t* left_s) {
  if (!bd_auth_required()) {
    if (left_s) *left_s = 0;
    return true;
  }
  if (!token || strlen(token) != BD_AUTH_TOKEN_LEN) return false;
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  init_sessions();
  const uint32_t now = now_s();
  for (const Session& s : s_sessions) {
    if (now < s.expires_s && memcmp(s.token, token, BD_AUTH_TOKEN_LEN) == 0) {
      ok = true;
      if (left_s) *left_s = s.expires_s - now;
      break;
    }
  }
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

void bd_auth_logout(const char* token) {
  if (!token) return;
  portENTER_CRITICAL(&s_mux);
  init_sessions();
  for (Session& s : s_sessions) {
    if (memcmp(s.token, token, BD_AUTH_TOKEN_LEN) == 0) s.expires_s = 0;
  }
  portEXIT_CRITICAL(&s_mux);
}

void bd_auth_set_password(const char* password, const char* keep_token) {
  uint8_t salt[16] = {}, hash[32] = {};
  if (password && password[0]) {
    esp_fill_random(salt, sizeof(salt));
    hash_password(password, salt, hash);
  }
  settings_lock();
  memcpy(settings.passSalt, salt, sizeof(salt));
  memcpy(settings.passHash, hash, sizeof(hash));
  settings_unlock();
  save_preferences();

  portENTER_CRITICAL(&s_mux);
  init_sessions();
  for (Session& s : s_sessions) {
    if (!keep_token || memcmp(s.token, keep_token, BD_AUTH_TOKEN_LEN) != 0) s.expires_s = 0;
  }
  portEXIT_CRITICAL(&s_mux);
  ESP_LOGW(TAG, "password %s", password && password[0] ? "set" : "removed");
}

void bd_auth_button_request(int sockfd) {
  const uint32_t now = now_s();
  portENTER_CRITICAL(&s_mux);
  int slot = 0;
  for (int i = 0; i < kPending; i++) {
    if (s_pending[i].fd == sockfd || now >= s_pending[i].expires_s) {
      slot = i;
      break;
    }
    if (s_pending[i].expires_s < s_pending[slot].expires_s) slot = i;
  }
  s_pending[slot] = {sockfd, now + BD_AUTH_BUTTON_WINDOW_S};
  portEXIT_CRITICAL(&s_mux);
}

void bd_auth_button_pressed() {
  Pending approved[kPending];
  int count = 0;
  const uint32_t now = now_s();
  portENTER_CRITICAL(&s_mux);
  for (Pending& p : s_pending) {
    if (now < p.expires_s) approved[count++] = p;
    p.expires_s = 0;
  }
  portEXIT_CRITICAL(&s_mux);

  for (int i = 0; i < count; i++) {
    char token[BD_AUTH_TOKEN_LEN + 1];
    new_session(token);
    JsonDocument doc;
    doc["auth"]["token"] = token;
    doc["auth"]["expiresIn"] = BD_AUTH_SESSION_S;
    doc["auth"]["via"] = "button";
    String out;
    serializeJson(doc, out);
    bd_ws_send(approved[i].fd, out);
    ESP_LOGW(TAG, "session opened by the button for client %d", approved[i].fd);
  }
}
