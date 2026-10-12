#include "web.h"

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#include "auth.h"
#include "config.h"
#include "controller.h"
#include "generated/web_assets.h"
#include "lock.h"
#include "profiles.h"
#include "settings.h"

namespace {

AsyncWebServer g_server(80);

constexpr size_t kMaxJsonBody = 24 * 1024;
constexpr uint32_t kMaxHistoryPerRequest = 900;

// ---------------------------------------------------------------- helpers

void sendJson(AsyncWebServerRequest* req, const JsonDocument& doc, int code = 200) {
  AsyncResponseStream* res = req->beginResponseStream("application/json");
  res->setCode(code);
  res->addHeader("Cache-Control", "no-store");
  serializeJson(doc, *res);
  req->send(res);
}

void sendError(AsyncWebServerRequest* req, int code, const String& msg) {
  JsonDocument doc;
  doc["error"] = msg;
  sendJson(req, doc, code);
}

void sendOk(AsyncWebServerRequest* req) {
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(req, doc);
}

// Replies "ok" or 400 with the controller's error message.
void sendResult(AsyncWebServerRequest* req, const String& err) {
  if (err.isEmpty()) {
    sendOk(req);
  } else {
    sendError(req, 400, err);
  }
}

bool require(AsyncWebServerRequest* req, Role need) {
  Role have = auth::roleOf(req);
  if (have >= need) return true;
  if (have == ROLE_NONE) {
    sendError(req, 401, "Login required");
  } else {
    sendError(req, 403, "Admin password required");
  }
  return false;
}

void onJson(const char* uri, WebRequestMethodComposite method,
            ArJsonRequestHandlerFunction fn) {
  auto* h = new AsyncCallbackJsonWebHandler(uri, fn);
  h->setMethod(method);
  h->setMaxContentLength(kMaxJsonBody);
  g_server.addHandler(h);
}

int idParam(AsyncWebServerRequest* req) {
  if (!req->hasParam("id")) return -1;
  return req->getParam("id")->value().toInt();
}

bool sensorField(JsonVariant json, Sensor& out, Sensor def) {
  if (json["sensor"].isNull()) {
    out = def;
    return true;
  }
  return parseSensor(json["sensor"].as<const char*>(), out);
}

float floatField(JsonVariant json, const char* key) {
  return json[key].is<float>() ? json[key].as<float>() : NAN;
}

bool isOwnHost(AsyncWebServerRequest* req) {
  String host = req->host();
  int colon = host.indexOf(':');
  if (colon >= 0) host = host.substring(0, colon);
  return host.isEmpty() || host == WiFi.softAPIP().toString() ||
         host.equalsIgnoreCase(MDNS_HOSTNAME ".local") || host.equalsIgnoreCase(MDNS_HOSTNAME);
}

void redirectHome(AsyncWebServerRequest* req) {
  req->redirect(String("http://") + WiFi.softAPIP().toString() + "/");
}

void serveAsset(AsyncWebServerRequest* req, const uint8_t* data, size_t len, const char* type) {
  AsyncWebServerResponse* res = req->beginResponse(200, type, data, len);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", "no-cache");
  req->send(res);
}

// ---------------------------------------------------------------- routes

void routeStatic() {
  g_server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!isOwnHost(req)) return redirectHome(req);  // captive-portal probes
    serveAsset(req, INDEX_HTML_GZ, INDEX_HTML_LEN, INDEX_HTML_TYPE);
  });
  g_server.on("/app.css", HTTP_GET, [](AsyncWebServerRequest* req) {
    serveAsset(req, APP_CSS_GZ, APP_CSS_LEN, APP_CSS_TYPE);
  });
  g_server.on("/app.js", HTTP_GET, [](AsyncWebServerRequest* req) {
    serveAsset(req, APP_JS_GZ, APP_JS_LEN, APP_JS_TYPE);
  });
}

void routeAuth() {
  onJson("/api/login", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant& json) {
    if (auth::lockedOut()) return sendError(req, 429, "Too many attempts; wait 30 s");
    Role role = auth::checkPassword(json["password"] | "");
    if (role == ROLE_NONE) {
      auth::noteFailure();
      return sendError(req, 401, "Wrong password");
    }
    auth::noteSuccess();
    String token = auth::createSession(role);
    JsonDocument doc;
    doc["ok"] = true;
    doc["role"] = auth::roleName(role);
    doc["token"] = token;
    AsyncResponseStream* res = req->beginResponseStream("application/json");
    res->addHeader("Set-Cookie", auth::cookieFor(token));
    res->addHeader("Cache-Control", "no-store");
    serializeJson(doc, *res);
    req->send(res);
  });

  onJson("/api/elevate", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant& json) {
    if (!require(req, ROLE_USER)) return;
    if (auth::lockedOut()) return sendError(req, 429, "Too many attempts; wait 30 s");
    if (auth::checkPassword(json["password"] | "") != ROLE_ADMIN) {
      auth::noteFailure();
      return sendError(req, 403, "Wrong admin password");
    }
    auth::noteSuccess();
    auth::setRole(req, ROLE_ADMIN);
    JsonDocument doc;
    doc["ok"] = true;
    doc["role"] = "admin";
    sendJson(req, doc);
  });

  g_server.on("/api/logout", HTTP_POST, [](AsyncWebServerRequest* req) {
    auth::destroySession(req);
    AsyncWebServerResponse* res =
        req->beginResponse(200, "application/json", "{\"ok\":true}");
    res->addHeader("Set-Cookie", auth::clearCookie());
    req->send(res);
  });

  g_server.on("/api/session", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["role"] = auth::roleName(auth::roleOf(req));
    doc["version"] = FW_VERSION;
    doc["ssid"] = AP_SSID;
    sendJson(req, doc);
  });
}

void routeControl() {
  g_server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!require(req, ROLE_USER)) return;
    JsonDocument doc;
    {
      LockGuard lock;
      controller::statusJson(doc.to<JsonObject>());
    }
    doc["role"] = auth::roleName(auth::roleOf(req));
    sendJson(req, doc);
  });

  g_server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!require(req, ROLE_USER)) return;
    uint32_t since = req->hasParam("since") ? req->getParam("since")->value().toInt() : 0;
    uint32_t maxN = kMaxHistoryPerRequest;
    if (req->hasParam("max")) {
      maxN = constrain(req->getParam("max")->value().toInt(), 1, (long)kMaxHistoryPerRequest);
    }
    AsyncResponseStream* res = req->beginResponseStream("application/json");
    res->addHeader("Cache-Control", "no-store");
    {
      LockGuard lock;
      controller::historyJson(*res, since, maxN);
    }
    req->send(res);
  });

  onJson("/api/heater", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant& json) {
    if (!require(req, ROLE_USER)) return;
    if (!json["on"].is<bool>()) return sendError(req, 400, "Expected {\"on\": true|false}");
    LockGuard lock;
    sendResult(req, controller::setManual(json["on"].as<bool>()));
  });

  g_server.on("/api/stop", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!require(req, ROLE_USER)) return;
    LockGuard lock;
    controller::stop("Stopped by user");
    sendOk(req);
  });

  onJson("/api/hold", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant& json) {
    if (!require(req, ROLE_USER)) return;
    Sensor sensor;
    if (!sensorField(json, sensor, Sensor::Aux)) {
      return sendError(req, 400, "sensor must be \"aux\" or \"ambient\"");
    }
    LockGuard lock;
    sendResult(req, controller::startHold(floatField(json, "setpoint"), sensor));
  });

  onJson("/api/reflow/start", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant& json) {
    if (!require(req, ROLE_USER)) return;
    Sensor sensor;
    if (!sensorField(json, sensor, Sensor::Aux)) {
      return sendError(req, 400, "sensor must be \"aux\" or \"ambient\"");
    }
    if (!json["profileId"].is<int>()) return sendError(req, 400, "profileId is required");
    LockGuard lock;
    sendResult(req, controller::startReflow(json["profileId"].as<int>(), sensor));
  });

  onJson("/api/reflow/override", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant& json) {
    if (!require(req, ROLE_USER)) return;
    bool on = json["forceOn"] | false, off = json["forceOff"] | false;
    bool hasOn = json["forceOn"].is<bool>(), hasOff = json["forceOff"].is<bool>();
    if (!hasOn && !hasOff) return sendError(req, 400, "Expected {\"forceOn\": bool} and/or {\"forceOff\": bool}");
    LockGuard lock;
    sendResult(req, controller::setReflowOverride(hasOn ? &on : nullptr, hasOff ? &off : nullptr));
  });

  onJson("/api/autotune/start", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant& json) {
    if (!require(req, ROLE_USER)) return;
    Sensor sensor;
    if (!sensorField(json, sensor, Sensor::Aux)) {
      return sendError(req, 400, "sensor must be \"aux\" or \"ambient\"");
    }
    float target = floatField(json, "target");
    float hyst = json["hysteresis"].is<float>() ? json["hysteresis"].as<float>() : 2.0f;
    int cycles = json["cycles"] | 3;
    LockGuard lock;
    sendResult(req, controller::startAutotune(target, sensor, hyst, cycles));
  });

  g_server.on("/api/autotune", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!require(req, ROLE_USER)) return;
    JsonDocument doc;
    {
      LockGuard lock;
      controller::autotuneJson(doc.to<JsonObject>());
    }
    sendJson(req, doc);
  });

  onJson("/api/autotune/apply", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant& json) {
    if (!require(req, ROLE_ADMIN)) return;
    LockGuard lock;
    sendResult(req, controller::applyAutotune(json["rule"] | ""));
  });
}

void routeProfiles() {
  g_server.on("/api/profiles", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!require(req, ROLE_USER)) return;
    JsonDocument doc;
    {
      LockGuard lock;
      profiles::listJson(doc["profiles"].to<JsonArray>());
    }
    doc["max"] = cfg::kMaxProfiles;
    sendJson(req, doc);
  });

  onJson("/api/profiles", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant& json) {
    if (!require(req, ROLE_USER)) return;
    Profile p;
    p.name = json["name"] | "";
    p.name.trim();
    p.description = json["description"] | "";
    String err;
    if (!profiles::validateMeta(p.name, p.description, err) ||
        !profiles::parseCsv(json["csv"] | "", p.points, err)) {
      return sendError(req, 400, err);
    }
    LockGuard lock;
    if (!profiles::create(p, err)) return sendError(req, 400, err);
    JsonDocument doc;
    profiles::toJson(p, doc["profile"].to<JsonObject>());
    doc["ok"] = true;
    sendJson(req, doc);
  });

  g_server.on("/api/profile", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!require(req, ROLE_USER)) return;
    Profile p;
    LockGuard lock;
    if (!profiles::load(idParam(req), p)) return sendError(req, 404, "Profile not found");
    JsonDocument doc;
    profiles::toJson(p, doc.to<JsonObject>());
    sendJson(req, doc);
  });

  onJson("/api/profile", HTTP_PUT, [](AsyncWebServerRequest* req, JsonVariant& json) {
    if (!require(req, ROLE_USER)) return;
    LockGuard lock;
    Profile p;
    if (!profiles::load(idParam(req), p)) return sendError(req, 404, "Profile not found");
    if (json["name"].is<const char*>()) {
      p.name = json["name"].as<const char*>();
      p.name.trim();
    }
    if (json["description"].is<const char*>()) p.description = json["description"].as<const char*>();
    String err;
    if (json["csv"].is<const char*>() && !profiles::parseCsv(json["csv"].as<const char*>(), p.points, err)) {
      return sendError(req, 400, err);
    }
    if (!profiles::validateMeta(p.name, p.description, err) || !profiles::save(p, err)) {
      return sendError(req, 400, err);
    }
    JsonDocument doc;
    profiles::toJson(p, doc["profile"].to<JsonObject>());
    doc["ok"] = true;
    sendJson(req, doc);
  });

  g_server.on("/api/profile", HTTP_DELETE, [](AsyncWebServerRequest* req) {
    if (!require(req, ROLE_USER)) return;
    LockGuard lock;
    if (!profiles::remove(idParam(req))) return sendError(req, 404, "Profile not found");
    sendOk(req);
  });
}

void routeSettings() {
  g_server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!require(req, ROLE_ADMIN)) return;
    JsonDocument doc;
    {
      LockGuard lock;
      settings::toJson(settings::current(), doc["settings"].to<JsonObject>());
    }
    settings::toJson(settings::defaults(), doc["defaults"].to<JsonObject>());
    settings::pinRulesJson(doc["rules"].to<JsonObject>());
    sendJson(req, doc);
  });

  onJson("/api/settings", HTTP_PUT, [](AsyncWebServerRequest* req, JsonVariant& json) {
    if (!require(req, ROLE_ADMIN)) return;
    LockGuard lock;
    Settings next = settings::current();
    settings::fromJson(json, next);
    String err;
    if (!settings::validate(next, err)) return sendError(req, 400, err);
    bool pinsChanged = next.pins != settings::current().pins;
    settings::current() = next;
    controller::onSettingsChanged(pinsChanged);
    if (!settings::save()) return sendError(req, 500, "Applied, but saving to flash failed");
    JsonDocument doc;
    doc["ok"] = true;
    doc["pinsChanged"] = pinsChanged;
    settings::toJson(next, doc["settings"].to<JsonObject>());
    sendJson(req, doc);
  });
}

void routeFallback() {
  g_server.onNotFound([](AsyncWebServerRequest* req) {
    if (req->url().startsWith("/api/")) return sendError(req, 404, "No such endpoint");
    redirectHome(req);  // also makes phones pop up the captive-portal sheet
  });
}

}  // namespace

namespace web {

void begin() {
  routeStatic();
  routeAuth();
  routeControl();
  routeProfiles();
  routeSettings();
  routeFallback();
  g_server.begin();
}

}  // namespace web
