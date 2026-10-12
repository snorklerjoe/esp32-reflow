#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <vector>

struct ProfilePoint {
  float t;  // seconds from start
  float c;  // setpoint, °C
};

struct Profile {
  int id = 0;
  String name;
  String description;
  std::vector<ProfilePoint> points;

  float duration() const { return points.empty() ? 0 : points.back().t; }
  float peak() const;
  // Linearly interpolated setpoint at time t; `slope` gets °C/s of that segment.
  float setpointAt(float t, float* slope = nullptr) const;
};

// Reflow profiles stored as JSON files in LittleFS (/profiles/<id>.json).
namespace profiles {

bool begin();
// Summaries of all profiles (including their points; profiles are small).
void listJson(JsonArray out);
bool load(int id, Profile& p);
// Assigns p.id on success.
bool create(Profile& p, String& err);
bool save(const Profile& p, String& err);
bool remove(int id);

// "time_s,temp_c" rows; optional header line, '#' comments, , ; or tab separators.
bool parseCsv(const String& csv, std::vector<ProfilePoint>& pts, String& err);
bool validateMeta(const String& name, const String& description, String& err);
void toJson(const Profile& p, JsonObject out);

}  // namespace profiles
