// Write protection of the web interface. Reading is open to everyone; changes
// need a session, which a password or a press of the button on the station
// opens for one hour. Without a password set, everything is open as before.
#pragma once

#include <stddef.h>
#include <stdint.h>

static constexpr size_t BD_AUTH_TOKEN_LEN = 32;  // hex characters

bool bd_auth_required();

// Starts a session for the right password. `token` takes BD_AUTH_TOKEN_LEN + 1
// characters. False for a wrong password, and for any while a run of wrong
// ones has the login locked; `retry_s` then says for how long.
bool bd_auth_login(const char* password, char* token, uint32_t* retry_s);

// Whether the token belongs to a session that has not expired; its remaining
// seconds go to `left_s`. Also true for anyone while no password is set.
bool bd_auth_allowed(const char* token, uint32_t* left_s = nullptr);
void bd_auth_logout(const char* token);

// Sets the password, or removes it when empty. Ends all other sessions.
void bd_auth_set_password(const char* password, const char* keep_token);

// The forgotten-password way in: a client asks, and a press of the button on
// the station within two minutes gives it a session, which is sent to it.
void bd_auth_button_request(int sockfd);
void bd_auth_button_pressed();

static constexpr uint32_t BD_AUTH_SESSION_S = 3600;
static constexpr uint32_t BD_AUTH_BUTTON_WINDOW_S = 120;
