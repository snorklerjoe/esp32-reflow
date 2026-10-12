#include "settings.h"

#include <Preferences.h>

#include "config.h"

namespace {

constexpr const char* kNamespace = "reflow";
constexpr const char* kKey = "cfg";
constexpr uint32_t kMagic = 0x52464C31;  // "RFL1"

struct Stored {
  uint32_t magic;
  uint32_t size;
  Settings s;
};

Settings g_settings;

// Pins safe to drive as outputs on an ESP-WROOM-32 dev board. Excluded:
//   0, 2, 5, 12, 15  strapping pins (12 high at boot selects 1.8 V flash!)
//   1, 3             USB serial
//   6-11             internal SPI flash
//   34-39            input only
const uint8_t kOutputPins[] = {4, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
const uint8_t kInputOnlyPins[] = {34, 35, 36, 39};

template <size_t N>
bool inList(const uint8_t (&list)[N], uint8_t pin) {
  for (uint8_t p : list) {
    if (p == pin) return true;
  }
  return false;
}

bool isOutputPin(uint8_t pin) { return inList(kOutputPins, pin); }
bool isInputPin(uint8_t pin) { return isOutputPin(pin) || inList(kInputOnlyPins, pin); }

bool inRange(float v, float lo, float hi) { return !isnan(v) && v >= lo && v <= hi; }

void readFloat(JsonVariantConst in, const char* key, float& dst) {
  if (in[key].is<float>()) dst = in[key].as<float>();
}

void readPin(JsonVariantConst in, const char* key, uint8_t& dst) {
  if (in[key].is<int>()) dst = static_cast<uint8_t>(constrain(in[key].as<int>(), 0, 255));
}

}  // namespace

namespace settings {

Settings& current() { return g_settings; }

Settings defaults() {
  Settings s{};
  s.pins = {cfg::kDefaultPinSsr, cfg::kDefaultPinSck, cfg::kDefaultPinSo,
            cfg::kDefaultPinCsAmbient, cfg::kDefaultPinCsAux};
  s.ssrActiveLow = false;
  s.kp = 5.0f;
  s.ki = 0.05f;
  s.kd = 60.0f;
  s.ssrWindowMs = 1000;
  s.maxTempC = 280.0f;
  s.doorMarginC = 5.0f;
  return s;
}

void load() {
  Stored st{};
  Preferences prefs;
  size_t n = 0;
  if (prefs.begin(kNamespace, false)) {
    n = prefs.getBytes(kKey, &st, sizeof(st));
    prefs.end();
  }
  String err;
  if (n == sizeof(st) && st.magic == kMagic && st.size == sizeof(Settings) &&
      validate(st.s, err)) {
    g_settings = st.s;
  } else {
    g_settings = defaults();
  }
}

bool save() {
  Stored st{kMagic, sizeof(Settings), g_settings};
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  bool ok = prefs.putBytes(kKey, &st, sizeof(st)) == sizeof(st);
  prefs.end();
  return ok;
}

bool validate(const Settings& s, String& err) {
  const struct {
    const char* name;
    uint8_t pin;
    bool inputOnlyOk;
  } pins[] = {
      {"SSR", s.pins.ssr, false},
      {"SCK", s.pins.sck, false},
      {"SO", s.pins.so, true},
      {"CS ambient", s.pins.csAmbient, false},
      {"CS aux", s.pins.csAux, false},
  };
  for (const auto& p : pins) {
    bool ok = p.inputOnlyOk ? isInputPin(p.pin) : isOutputPin(p.pin);
    if (!ok) {
      err = String("GPIO ") + p.pin + " can't be used for " + p.name;
      return false;
    }
  }
  for (size_t i = 0; i < 5; ++i) {
    for (size_t j = i + 1; j < 5; ++j) {
      if (pins[i].pin == pins[j].pin) {
        err = String(pins[i].name) + " and " + pins[j].name + " are both on GPIO " + pins[i].pin;
        return false;
      }
    }
  }
  if (!inRange(s.kp, 0, 1000) || !inRange(s.ki, 0, 100) || !inRange(s.kd, 0, 10000)) {
    err = "PID gains must be non-negative (Kp<=1000, Ki<=100, Kd<=10000)";
    return false;
  }
  if (s.ssrWindowMs < 200 || s.ssrWindowMs > 10000) {
    err = "SSR window must be 200-10000 ms";
    return false;
  }
  if (!inRange(s.maxTempC, 50, 350)) {
    err = "Max temperature must be 50-350 °C";
    return false;
  }
  if (!inRange(s.doorMarginC, 0, 50)) {
    err = "Door margin must be 0-50 °C";
    return false;
  }
  return true;
}

void toJson(const Settings& s, JsonObject out) {
  JsonObject pins = out["pins"].to<JsonObject>();
  pins["ssr"] = s.pins.ssr;
  pins["sck"] = s.pins.sck;
  pins["so"] = s.pins.so;
  pins["csAmbient"] = s.pins.csAmbient;
  pins["csAux"] = s.pins.csAux;
  out["ssrActiveLow"] = s.ssrActiveLow;
  JsonObject pid = out["pid"].to<JsonObject>();
  pid["kp"] = s.kp;
  pid["ki"] = s.ki;
  pid["kd"] = s.kd;
  out["ssrWindowMs"] = s.ssrWindowMs;
  out["maxTempC"] = s.maxTempC;
  out["doorMarginC"] = s.doorMarginC;
}

void fromJson(JsonVariantConst in, Settings& s) {
  JsonVariantConst pins = in["pins"];
  readPin(pins, "ssr", s.pins.ssr);
  readPin(pins, "sck", s.pins.sck);
  readPin(pins, "so", s.pins.so);
  readPin(pins, "csAmbient", s.pins.csAmbient);
  readPin(pins, "csAux", s.pins.csAux);
  if (in["ssrActiveLow"].is<bool>()) s.ssrActiveLow = in["ssrActiveLow"].as<bool>();
  JsonVariantConst pid = in["pid"];
  readFloat(pid, "kp", s.kp);
  readFloat(pid, "ki", s.ki);
  readFloat(pid, "kd", s.kd);
  if (in["ssrWindowMs"].is<int>()) {
    s.ssrWindowMs = static_cast<uint16_t>(constrain(in["ssrWindowMs"].as<int>(), 0, 65535));
  }
  readFloat(in, "maxTempC", s.maxTempC);
  readFloat(in, "doorMarginC", s.doorMarginC);
}

void pinRulesJson(JsonObject out) {
  JsonArray outputs = out["output"].to<JsonArray>();
  for (uint8_t p : kOutputPins) outputs.add(p);
  JsonArray inputs = out["input"].to<JsonArray>();
  for (uint8_t p : kOutputPins) inputs.add(p);
  for (uint8_t p : kInputOnlyPins) inputs.add(p);
}

}  // namespace settings
