#include "auth.h"

#include <ESPAsyncWebServer.h>

#include "config.h"

namespace {

constexpr const char* kCookieName = "rf_session";

struct Session {
  bool used = false;
  char token[33] = {};
  Role role = ROLE_NONE;
  uint32_t lastSeen = 0;
};

Session g_sessions[cfg::kMaxSessions];
uint8_t g_failures = 0;
uint32_t g_lockoutStart = 0;

// Compares without an early exit so timing doesn't leak the password prefix.
bool constantTimeEquals(const String& a, const char* b) {
  size_t la = a.length(), lb = strlen(b);
  size_t n = max(la, lb);
  uint8_t diff = la != lb;
  for (size_t i = 0; i < n; ++i) {
    char ca = i < la ? a[i] : 0;
    char cb = i < lb ? b[i] : 0;
    diff |= ca ^ cb;
  }
  return diff == 0;
}

String tokenFrom(AsyncWebServerRequest* req) {
  if (req->hasHeader("Authorization")) {
    String h = req->header("Authorization");
    if (h.startsWith("Bearer ")) return h.substring(7);
  }
  if (req->hasHeader("Cookie")) {
    String c = req->header("Cookie");
    String key = String(kCookieName) + "=";
    int i = c.indexOf(key);
    if (i >= 0) {
      int start = i + key.length();
      int end = c.indexOf(';', start);
      return c.substring(start, end < 0 ? c.length() : end);
    }
  }
  return "";
}

Session* find(const String& token) {
  if (token.length() != 32) return nullptr;
  uint32_t now = millis();
  for (auto& s : g_sessions) {
    if (!s.used) continue;
    if (now - s.lastSeen > cfg::kSessionIdleMs) {
      s = Session{};
      continue;
    }
    if (constantTimeEquals(token, s.token)) return &s;
  }
  return nullptr;
}

}  // namespace

namespace auth {

const char* roleName(Role r) {
  switch (r) {
    case ROLE_ADMIN:
      return "admin";
    case ROLE_USER:
      return "user";
    default:
      return "none";
  }
}

Role roleOf(AsyncWebServerRequest* req) {
  Session* s = find(tokenFrom(req));
  if (!s) return ROLE_NONE;
  s->lastSeen = millis();
  return s->role;
}

Role checkPassword(const String& password) {
  // Evaluate both so timing doesn't reveal which one matched.
  bool admin = constantTimeEquals(password, ADMIN_PASSWORD);
  bool user = constantTimeEquals(password, WEB_PASSWORD);
  if (admin) return ROLE_ADMIN;
  if (user) return ROLE_USER;
  return ROLE_NONE;
}

String createSession(Role role) {
  // Reuse a free slot, or evict the least recently used session.
  Session* slot = &g_sessions[0];
  for (auto& s : g_sessions) {
    if (!s.used) {
      slot = &s;
      break;
    }
    if (s.lastSeen < slot->lastSeen) slot = &s;
  }
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 32; i += 8) {
    uint32_t r = esp_random();
    for (int j = 0; j < 8; ++j) slot->token[i + j] = hex[(r >> (j * 4)) & 0xF];
  }
  slot->token[32] = 0;
  slot->used = true;
  slot->role = role;
  slot->lastSeen = millis();
  return String(slot->token);
}

bool setRole(AsyncWebServerRequest* req, Role role) {
  Session* s = find(tokenFrom(req));
  if (!s) return false;
  s->role = role;
  return true;
}

void destroySession(AsyncWebServerRequest* req) {
  Session* s = find(tokenFrom(req));
  if (s) *s = Session{};
}

String cookieFor(const String& token) {
  return String(kCookieName) + "=" + token + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
         String(cfg::kSessionIdleMs / 1000);
}

String clearCookie() { return String(kCookieName) + "=; Path=/; HttpOnly; Max-Age=0"; }

bool lockedOut() {
  if (g_failures < cfg::kLoginFailuresBeforeLockout) return false;
  if (millis() - g_lockoutStart < cfg::kLoginLockoutMs) return true;
  g_failures = 0;
  return false;
}

void noteFailure() {
  if (++g_failures >= cfg::kLoginFailuresBeforeLockout) g_lockoutStart = millis();
}

void noteSuccess() { g_failures = 0; }

}  // namespace auth
