#pragma once

#include <Arduino.h>

class AsyncWebServerRequest;

enum Role : uint8_t { ROLE_NONE = 0, ROLE_USER = 1, ROLE_ADMIN = 2 };

// In-memory session tokens, sent as an HttpOnly cookie or "Authorization: Bearer".
// Only touched from the AsyncTCP task, so no locking is needed.
namespace auth {

const char* roleName(Role r);
Role roleOf(AsyncWebServerRequest* req);
Role checkPassword(const String& password);
String createSession(Role role);
bool setRole(AsyncWebServerRequest* req, Role role);
void destroySession(AsyncWebServerRequest* req);
String cookieFor(const String& token);
String clearCookie();

// Simple brute-force throttle for password checks.
bool lockedOut();
void noteFailure();
void noteSuccess();

}  // namespace auth
