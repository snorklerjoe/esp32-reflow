#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

struct PinConfig {
  uint8_t ssr;
  uint8_t sck;
  uint8_t so;
  uint8_t csAmbient;
  uint8_t csAux;

  bool operator==(const PinConfig& o) const {
    return ssr == o.ssr && sck == o.sck && so == o.so && csAmbient == o.csAmbient &&
           csAux == o.csAux;
  }
  bool operator!=(const PinConfig& o) const { return !(*this == o); }
};

struct Settings {
  PinConfig pins;
  bool ssrActiveLow;
  float kp;  // %/°C
  float ki;  // %/(°C·s)
  float kd;  // %·s/°C
  uint16_t ssrWindowMs;
  float maxTempC;     // over-temperature cutoff, any sensor
  float doorMarginC;  // show "open door" when this far above a falling setpoint
};

namespace settings {

Settings& current();
Settings defaults();
void load();
bool save();

bool validate(const Settings& s, String& err);
void toJson(const Settings& s, JsonObject out);
// Overlays any fields present in `in` onto `s`.
void fromJson(JsonVariantConst in, Settings& s);
// Allowed pins and notes, for the Settings UI.
void pinRulesJson(JsonObject out);

}  // namespace settings
