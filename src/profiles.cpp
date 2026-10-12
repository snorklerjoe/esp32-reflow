#include "profiles.h"

#include <LittleFS.h>

#include <algorithm>

#include "config.h"

namespace {

constexpr const char* kDir = "/profiles";

String pathFor(int id) { return String(kDir) + "/" + id + ".json"; }

std::vector<int> listIds() {
  std::vector<int> ids;
  File dir = LittleFS.open(kDir);
  if (!dir || !dir.isDirectory()) return ids;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String name = f.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!name.endsWith(".json")) continue;
    int id = name.toInt();
    if (id > 0) ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

bool writeFile(const Profile& p, String& err) {
  JsonDocument doc;
  doc["name"] = p.name;
  doc["description"] = p.description;
  JsonArray pts = doc["points"].to<JsonArray>();
  for (const auto& pt : p.points) {
    JsonArray row = pts.add<JsonArray>();
    row.add(pt.t);
    row.add(pt.c);
  }
  File f = LittleFS.open(pathFor(p.id), "w");
  if (!f) {
    err = "Couldn't write to flash";
    return false;
  }
  size_t n = serializeJson(doc, f);
  f.close();
  if (n == 0) {
    err = "Flash write failed (storage full?)";
    return false;
  }
  return true;
}

struct Seed {
  const char* name;
  const char* description;
  const char* csv;
};

const Seed kSeeds[] = {
    {"SAC305 (lead-free)",
     "Typical Sn96.5/Ag3.0/Cu0.5 ramp-soak-spike profile. Peak 245 °C, about 60 s above "
     "the 217 °C liquidus.",
     "time_s,temp_c\n0,25\n90,150\n180,180\n210,217\n230,240\n250,245\n270,217\n330,150\n"
     "390,80\n"},
    {"Sn63/Pb37 (leaded)",
     "Classic eutectic leaded solder profile. Peak 215 °C, about 50 s above the 183 °C "
     "liquidus.",
     "time_s,temp_c\n0,25\n75,140\n165,170\n195,183\n215,215\n230,215\n250,183\n310,120\n"
     "370,60\n"},
};

void seedDefaults() {
  for (const auto& s : kSeeds) {
    Profile p;
    p.name = s.name;
    p.description = s.description;
    String err;
    if (profiles::parseCsv(s.csv, p.points, err)) profiles::create(p, err);
  }
}

bool parseNumber(const char*& s, float& out) {
  while (*s == ' ' || *s == '\t' || *s == '"') ++s;
  char* end = nullptr;
  double v = strtod(s, &end);
  if (end == s) return false;
  s = end;
  while (*s == ' ' || *s == '"') ++s;
  out = static_cast<float>(v);
  return isfinite(out);
}

}  // namespace

float Profile::peak() const {
  float m = 0;
  for (const auto& p : points) m = std::max(m, p.c);
  return m;
}

float Profile::setpointAt(float t, float* slope) const {
  if (slope) *slope = 0;
  if (points.empty()) return NAN;
  if (t <= points.front().t) return points.front().c;
  for (size_t i = 1; i < points.size(); ++i) {
    if (t <= points[i].t) {
      const auto& a = points[i - 1];
      const auto& b = points[i];
      float s = (b.c - a.c) / (b.t - a.t);
      if (slope) *slope = s;
      return a.c + s * (t - a.t);
    }
  }
  return points.back().c;
}

namespace profiles {

bool begin() {
  if (!LittleFS.begin(true)) return false;
  if (!LittleFS.exists(kDir)) LittleFS.mkdir(kDir);
  if (listIds().empty()) seedDefaults();
  return true;
}

void listJson(JsonArray out) {
  for (int id : listIds()) {
    Profile p;
    if (load(id, p)) toJson(p, out.add<JsonObject>());
  }
}

bool load(int id, Profile& p) {
  File f = LittleFS.open(pathFor(id), "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) return false;
  p.id = id;
  p.name = doc["name"] | "";
  p.description = doc["description"] | "";
  p.points.clear();
  for (JsonArrayConst row : doc["points"].as<JsonArrayConst>()) {
    p.points.push_back({row[0].as<float>(), row[1].as<float>()});
  }
  return true;
}

bool create(Profile& p, String& err) {
  std::vector<int> ids = listIds();
  if (ids.size() >= cfg::kMaxProfiles) {
    err = String("Profile limit reached (") + cfg::kMaxProfiles + "); delete one first";
    return false;
  }
  p.id = ids.empty() ? 1 : ids.back() + 1;
  return writeFile(p, err);
}

bool save(const Profile& p, String& err) {
  if (!LittleFS.exists(pathFor(p.id))) {
    err = "Profile not found";
    return false;
  }
  return writeFile(p, err);
}

bool remove(int id) { return LittleFS.remove(pathFor(id)); }

bool parseCsv(const String& csv, std::vector<ProfilePoint>& pts, String& err) {
  pts.clear();
  bool firstContentLine = true;
  int lineNo = 0;
  size_t start = 0;
  const size_t len = csv.length();
  while (start < len) {
    int nl = csv.indexOf('\n', start);
    size_t end = nl < 0 ? len : static_cast<size_t>(nl);
    String line = csv.substring(start, end);
    start = end + 1;
    ++lineNo;
    line.trim();
    if (line.isEmpty() || line[0] == '#') continue;

    const char* s = line.c_str();
    float t, c;
    bool ok = parseNumber(s, t);
    if (ok) {
      if (*s == ',' || *s == ';' || *s == '\t') ++s;
      ok = parseNumber(s, c);
    }
    if (!ok) {
      if (firstContentLine) {  // header row
        firstContentLine = false;
        continue;
      }
      err = String("Line ") + lineNo + ": expected \"time_s,temp_c\"";
      return false;
    }
    firstContentLine = false;

    if (t < 0 || t > 6 * 3600) {
      err = String("Line ") + lineNo + ": time must be 0-21600 s";
      return false;
    }
    if (c < 0 || c > 350) {
      err = String("Line ") + lineNo + ": temperature must be 0-350 °C";
      return false;
    }
    if (!pts.empty() && t <= pts.back().t) {
      err = String("Line ") + lineNo + ": times must strictly increase";
      return false;
    }
    if (pts.size() >= cfg::kMaxProfilePoints) {
      err = String("Too many points (max ") + cfg::kMaxProfilePoints + ")";
      return false;
    }
    pts.push_back({t, c});
  }
  if (pts.size() < 2) {
    err = "A profile needs at least 2 points";
    return false;
  }
  return true;
}

bool validateMeta(const String& name, const String& description, String& err) {
  if (name.isEmpty()) {
    err = "Name can't be empty";
    return false;
  }
  if (name.length() > cfg::kMaxNameLen) {
    err = String("Name is too long (max ") + cfg::kMaxNameLen + ")";
    return false;
  }
  if (description.length() > cfg::kMaxDescriptionLen) {
    err = String("Description is too long (max ") + cfg::kMaxDescriptionLen + ")";
    return false;
  }
  return true;
}

void toJson(const Profile& p, JsonObject out) {
  out["id"] = p.id;
  out["name"] = p.name;
  out["description"] = p.description;
  out["duration"] = p.duration();
  out["peak"] = p.peak();
  JsonArray pts = out["points"].to<JsonArray>();
  for (const auto& pt : p.points) {
    JsonArray row = pts.add<JsonArray>();
    row.add(pt.t);
    row.add(pt.c);
  }
}

}  // namespace profiles
