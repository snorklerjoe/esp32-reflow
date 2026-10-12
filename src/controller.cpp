#include "controller.h"

#include "config.h"
#include "lock.h"
#include "max6675.h"
#include "pid.h"
#include "profiles.h"
#include "settings.h"

namespace {

struct SensorState {
  float c = NAN;
  float lastGood = NAN;
  TcFault fault = TcFault::NoDevice;
  uint8_t badCount = 0;
};

struct Sample {
  int16_t amb;  // tenths of °C, kNoValue if unavailable
  int16_t aux;
  int16_t sp;
  uint8_t duty;
};
constexpr int16_t kNoValue = INT16_MIN;

struct RunInfo {
  bool exists = false;
  bool active = false;
  Mode kind = Mode::Idle;
  Sensor sensor = Sensor::Aux;
  uint32_t startMs = 0;
  uint32_t endMs = 0;
  uint32_t startSeq = 0;
  uint32_t endSeq = 0;
  String result;  // "running", "complete", "stopped", "failed"
  int profileId = 0;
  String profileName;
  float duration = 0;
};

struct AutotuneRule {
  const char* id;
  const char* name;
  float kpFactor, tiFactor, tdFactor;  // Kp = f*Ku, Ti = f*Tu, Td = f*Tu
};
const AutotuneRule kRules[] = {
    {"none", "No overshoot", 0.20f, 0.50f, 0.333f},
    {"some", "Some overshoot", 0.33f, 0.50f, 0.333f},
    {"classic", "Classic Ziegler-Nichols", 0.60f, 0.50f, 0.125f},
};

struct Autotune {
  float target = 0;
  float hyst = 0;
  Sensor sensor = Sensor::Aux;
  int cycles = 0;  // cycles to measure (after one discarded settling cycle)
  bool relayOn = true;
  bool haveOnEdge = false;
  uint32_t lastOnMs = 0;
  float cycMax = 0, cycMin = 0;
  int cyclesDone = 0;
  int measured = 0;
  float sumPeriod = 0, sumAmp = 0;
  float lastPeriod = NAN, lastAmp = NAN;
  bool hasResult = false;
  float ku = 0, tu = 0, amp = 0;
};

constexpr float kRelayAmplitude = 50.0f;  // relay swings 0 <-> 100 %

Max6675 g_tc[2];
SensorState g_sens[2];
PinConfig g_pins;
bool g_pinsActive = false;

Mode g_mode = Mode::Idle;
Sensor g_fb = Sensor::Aux;
float g_setpoint = NAN;
float g_duty = 0;
bool g_ssrOn = false;
uint32_t g_windowStart = 0;
Pid g_pid;

RunInfo g_run;
Profile g_profile;
bool g_openDoor = false;
Autotune g_at;

Sample g_hist[cfg::kHistoryLen];
uint32_t g_histCount = 0;
uint32_t g_lastSensorMs = 0;
uint32_t g_lastHistMs = 0;

uint32_t g_eventId = 0;
const char* g_eventLevel = "info";
String g_eventText;

const char* modeName(Mode m) {
  switch (m) {
    case Mode::Manual:
      return "manual";
    case Mode::Hold:
      return "hold";
    case Mode::Reflow:
      return "reflow";
    case Mode::Autotune:
      return "autotune";
    default:
      return "idle";
  }
}

bool closedLoop(Mode m) { return m == Mode::Hold || m == Mode::Reflow || m == Mode::Autotune; }

void event(const char* level, const String& text) {
  ++g_eventId;
  g_eventLevel = level;
  g_eventText = text;
  Serial.printf("[%s] %s\n", level, text.c_str());
}

int16_t toTenths(float c) { return isnan(c) ? kNoValue : static_cast<int16_t>(lroundf(c * 10)); }

void writeSsr(bool on) {
  g_ssrOn = on;
  if (!g_pinsActive) return;
  bool level = settings::current().ssrActiveLow ? !on : on;
  digitalWrite(g_pins.ssr, level ? HIGH : LOW);
}

void releasePins() {
  if (!g_pinsActive) return;
  writeSsr(false);
  g_tc[0].end();
  g_tc[1].end();
  pinMode(g_pins.ssr, INPUT);
  pinMode(g_pins.sck, INPUT);
  pinMode(g_pins.so, INPUT);
  g_pinsActive = false;
}

void claimPins() {
  const Settings& s = settings::current();
  g_pins = s.pins;
  bool offLevel = s.ssrActiveLow;
  digitalWrite(g_pins.ssr, offLevel ? HIGH : LOW);
  pinMode(g_pins.ssr, OUTPUT);
  g_pinsActive = true;
  writeSsr(false);
  g_tc[0].begin(g_pins.sck, g_pins.so, g_pins.csAmbient);
  g_tc[1].begin(g_pins.sck, g_pins.so, g_pins.csAux);
  for (auto& st : g_sens) st = SensorState{};
}

// Time-proportioning: the SSR is on for duty% of each window.
void updateSsr(uint32_t now) {
  uint32_t win = settings::current().ssrWindowMs;
  if (now - g_windowStart >= win) g_windowStart += win * ((now - g_windowStart) / win);
  bool on;
  if (g_duty <= 0) {
    on = false;
  } else if (g_duty >= 100) {
    on = true;
  } else {
    on = (now - g_windowStart) < static_cast<uint32_t>(g_duty * win / 100.0f);
  }
  if (on != g_ssrOn) writeSsr(on);
}

void readSensors() {
  for (int i = 0; i < 2; ++i) {
    float c = NAN;
    SensorState& s = g_sens[i];
    s.fault = g_tc[i].read(c);
    if (s.fault == TcFault::Ok) {
      s.c = c;
      s.lastGood = c;
      s.badCount = 0;
    } else {
      s.c = NAN;
      if (s.badCount < 255) ++s.badCount;
    }
  }
}

// Feedback value, bridging brief glitches with the last good reading.
bool feedback(float& pv) {
  const SensorState& s = g_sens[static_cast<int>(g_fb)];
  if (s.fault == TcFault::Ok) {
    pv = s.c;
    return true;
  }
  if (s.badCount < cfg::kSensorFaultLimit && !isnan(s.lastGood)) {
    pv = s.lastGood;
    return true;
  }
  return false;
}

void endRun(const char* result) {
  g_mode = Mode::Idle;
  g_duty = 0;
  g_setpoint = NAN;
  g_openDoor = false;
  writeSsr(false);
  if (g_run.active) {
    g_run.active = false;
    g_run.endMs = millis();
    g_run.endSeq = g_histCount;
    g_run.result = result;
  }
}

void fail(const String& why) {
  endRun("failed");
  event("error", why);
}

void beginRun(Mode kind, Sensor sensor) {
  g_run = RunInfo{};
  g_run.exists = true;
  g_run.active = true;
  g_run.kind = kind;
  g_run.sensor = sensor;
  g_run.startMs = millis();
  g_run.startSeq = g_histCount;
  g_run.result = "running";
  g_mode = kind;
  g_fb = sensor;
  g_openDoor = false;
}

String checkFeedbackReady(Sensor s) {
  const SensorState& st = g_sens[static_cast<int>(s)];
  if (st.fault != TcFault::Ok) {
    return String("The ") + sensorName(s) + " thermocouple isn't reading (" +
           tcFaultName(st.fault) + ")";
  }
  return "";
}

void safetyCheck() {
  const float limit = settings::current().maxTempC;
  for (int i = 0; i < 2; ++i) {
    const SensorState& s = g_sens[i];
    if (s.fault == TcFault::Ok && s.c > limit && (g_mode != Mode::Idle || g_ssrOn)) {
      fail(String("Over-temperature: ") + sensorName(static_cast<Sensor>(i)) + " at " +
           String(s.c, 1) + " °C (limit " + String(limit, 0) + " °C). Heater off.");
      return;
    }
  }
}

void autotuneFinish() {
  g_at.tu = g_at.sumPeriod / g_at.measured;
  g_at.amp = g_at.sumAmp / g_at.measured;
  float a = g_at.amp;
  float denom = a > g_at.hyst ? sqrtf(a * a - g_at.hyst * g_at.hyst) : a;
  g_at.ku = 4.0f * kRelayAmplitude / (PI * denom);
  g_at.hasResult = true;
  endRun("complete");
  event("info", String("Autotune complete: Ku=") + String(g_at.ku, 2) + ", Tu=" +
                    String(g_at.tu, 0) + " s");
}

void autotuneTick(float pv, uint32_t now) {
  if (now - g_run.startMs > cfg::kAutotuneTimeoutMs) {
    fail("Autotune timed out before completing its cycles");
    return;
  }
  bool switchedOn = false;
  if (g_at.relayOn && pv >= g_at.target + g_at.hyst) {
    g_at.relayOn = false;
  } else if (!g_at.relayOn && pv <= g_at.target - g_at.hyst) {
    g_at.relayOn = true;
    switchedOn = true;
  }
  g_at.cycMax = max(g_at.cycMax, pv);
  g_at.cycMin = min(g_at.cycMin, pv);

  if (switchedOn) {
    if (g_at.haveOnEdge) {  // one full on->on cycle finished
      float period = (now - g_at.lastOnMs) / 1000.0f;
      float amp = (g_at.cycMax - g_at.cycMin) / 2.0f;
      g_at.lastPeriod = period;
      g_at.lastAmp = amp;
      ++g_at.cyclesDone;
      if (g_at.cyclesDone > 1) {  // the first cycle is still settling
        g_at.sumPeriod += period;
        g_at.sumAmp += amp;
        ++g_at.measured;
      }
      if (g_at.measured >= g_at.cycles) {
        autotuneFinish();
        return;
      }
    }
    g_at.haveOnEdge = true;
    g_at.lastOnMs = now;
    g_at.cycMax = g_at.cycMin = pv;
  }
  g_setpoint = g_at.target;
  g_duty = g_at.relayOn ? 100 : 0;
}

void reflowTick(float pv, uint32_t now) {
  float t = (now - g_run.startMs) / 1000.0f;
  if (t >= g_profile.duration()) {
    endRun("complete");
    event("info", String("Reflow \"") + g_profile.name + "\" complete");
    return;
  }
  float slope;
  g_setpoint = g_profile.setpointAt(t, &slope);
  g_duty = g_pid.update(g_setpoint, pv, cfg::kSensorPeriodMs / 1000.0f);

  // Cooling segment and the oven can't shed heat fast enough: open the door.
  // Latch until the oven gets back down to the setpoint.
  const float margin = settings::current().doorMarginC;
  if (slope < -0.01f && pv > g_setpoint + margin) {
    if (!g_openDoor) event("warn", "Open the oven door");
    g_openDoor = true;
  } else if (slope >= -0.01f || pv <= g_setpoint) {
    g_openDoor = false;
  }
}

void controlTick(uint32_t now) {
  readSensors();
  safetyCheck();

  if (!closedLoop(g_mode)) {
    g_duty = g_mode == Mode::Manual ? 100 : 0;
    return;
  }
  float pv;
  if (!feedback(pv)) {
    fail(String("Lost the ") + sensorName(g_fb) + " thermocouple (" +
         tcFaultName(g_sens[static_cast<int>(g_fb)].fault) + "). Heater off.");
    return;
  }
  switch (g_mode) {
    case Mode::Hold:
      g_duty = g_pid.update(g_setpoint, pv, cfg::kSensorPeriodMs / 1000.0f);
      break;
    case Mode::Reflow:
      reflowTick(pv, now);
      break;
    case Mode::Autotune:
      autotuneTick(pv, now);
      break;
    default:
      break;
  }
}

void recordSample() {
  Sample& s = g_hist[g_histCount % cfg::kHistoryLen];
  s.amb = toTenths(g_sens[0].c);
  s.aux = toTenths(g_sens[1].c);
  s.sp = closedLoop(g_mode) ? toTenths(g_setpoint) : kNoValue;
  s.duty = static_cast<uint8_t>(lroundf(g_duty));
  ++g_histCount;
}

void printTenths(Print& out, int16_t v) {
  if (v == kNoValue) {
    out.print("null");
    return;
  }
  int32_t x = v;
  if (x < 0) {
    out.print('-');
    x = -x;
  }
  out.printf("%ld.%ld", static_cast<long>(x / 10), static_cast<long>(x % 10));
}

void sensorJson(JsonObject o, const SensorState& s) {
  if (s.fault == TcFault::Ok) {
    o["c"] = s.c;
  } else {
    o["c"] = nullptr;
  }
  o["fault"] = tcFaultName(s.fault);
}

}  // namespace

const char* sensorName(Sensor s) { return s == Sensor::Ambient ? "ambient" : "aux"; }

bool parseSensor(const char* name, Sensor& out) {
  if (!name) return false;
  if (!strcmp(name, "ambient")) {
    out = Sensor::Ambient;
    return true;
  }
  if (!strcmp(name, "aux")) {
    out = Sensor::Aux;
    return true;
  }
  return false;
}

namespace controller {

void begin() {
  LockGuard lock;
  claimPins();
  const Settings& s = settings::current();
  g_pid.setGains(s.kp, s.ki, s.kd);
  uint32_t now = millis();
  g_windowStart = now;
  g_lastSensorMs = now;
  g_lastHistMs = now;
}

void loop() {
  LockGuard lock;
  uint32_t now = millis();
  if (now - g_lastSensorMs >= cfg::kSensorPeriodMs) {
    g_lastSensorMs += cfg::kSensorPeriodMs;
    if (now - g_lastSensorMs > 5 * cfg::kSensorPeriodMs) g_lastSensorMs = now;
    controlTick(now);
  }
  if (now - g_lastHistMs >= cfg::kHistoryPeriodMs) {
    g_lastHistMs += cfg::kHistoryPeriodMs;
    if (now - g_lastHistMs > 5 * cfg::kHistoryPeriodMs) g_lastHistMs = now;
    recordSample();
  }
  updateSsr(now);
}

String setManual(bool on) {
  if (!on) {
    stop("Heater off");
    return "";
  }
  if (g_mode != Mode::Idle && g_mode != Mode::Manual) {
    return String("A ") + modeName(g_mode) + " run is active; stop it first";
  }
  g_mode = Mode::Manual;
  g_duty = 100;
  updateSsr(millis());
  return "";
}

String startHold(float setpoint, Sensor sensor) {
  const float limit = settings::current().maxTempC;
  if (isnan(setpoint) || setpoint < 0 || setpoint > limit - 5) {
    return String("Setpoint must be 0-") + String(limit - 5, 0) + " °C";
  }
  if (g_mode == Mode::Reflow || g_mode == Mode::Autotune) {
    return String("A ") + modeName(g_mode) + " run is active; stop it first";
  }
  String err = checkFeedbackReady(sensor);
  if (!err.isEmpty()) return err;

  if (g_mode == Mode::Hold) {  // live setpoint change
    if (sensor != g_fb) {
      g_fb = sensor;
      g_run.sensor = sensor;
      g_pid.reset(g_sens[static_cast<int>(sensor)].c);
    }
    g_setpoint = setpoint;
    return "";
  }
  beginRun(Mode::Hold, sensor);
  g_setpoint = setpoint;
  g_pid.reset(g_sens[static_cast<int>(sensor)].c);
  event("info", String("Holding ") + String(setpoint, 1) + " °C on " + sensorName(sensor));
  return "";
}

String startReflow(int profileId, Sensor sensor) {
  if (g_mode == Mode::Reflow || g_mode == Mode::Autotune || g_mode == Mode::Hold) {
    return String("A ") + modeName(g_mode) + " run is active; stop it first";
  }
  Profile p;
  if (!profiles::load(profileId, p)) return "Profile not found";
  if (p.points.size() < 2) return "Profile has too few points";
  const float limit = settings::current().maxTempC;
  if (p.peak() > limit - 5) {
    return String("Profile peak ") + String(p.peak(), 0) + " °C is too close to the " +
           String(limit, 0) + " °C over-temperature limit";
  }
  String err = checkFeedbackReady(sensor);
  if (!err.isEmpty()) return err;

  g_profile = std::move(p);
  beginRun(Mode::Reflow, sensor);
  g_run.profileId = g_profile.id;
  g_run.profileName = g_profile.name;
  g_run.duration = g_profile.duration();
  g_setpoint = g_profile.setpointAt(0);
  g_pid.reset(g_sens[static_cast<int>(sensor)].c);
  event("info", String("Reflow \"") + g_profile.name + "\" started");
  return "";
}

String startAutotune(float target, Sensor sensor, float hysteresis, int cycles) {
  const float limit = settings::current().maxTempC;
  if (isnan(target) || target < 50 || target > limit - 20) {
    return String("Target must be 50-") + String(limit - 20, 0) + " °C";
  }
  if (isnan(hysteresis) || hysteresis < 0.25f || hysteresis > 10) {
    return "Hysteresis must be 0.25-10 °C";
  }
  if (cycles < 2 || cycles > 10) return "Cycles must be 2-10";
  if (g_mode == Mode::Reflow || g_mode == Mode::Autotune || g_mode == Mode::Hold) {
    return String("A ") + modeName(g_mode) + " run is active; stop it first";
  }
  String err = checkFeedbackReady(sensor);
  if (!err.isEmpty()) return err;

  g_at = Autotune{};
  g_at.target = target;
  g_at.hyst = hysteresis;
  g_at.sensor = sensor;
  g_at.cycles = cycles;
  g_at.relayOn = g_sens[static_cast<int>(sensor)].c < target;
  beginRun(Mode::Autotune, sensor);
  g_setpoint = target;
  event("info", String("Autotune started around ") + String(target, 0) + " °C");
  return "";
}

String applyAutotune(const char* rule) {
  if (!g_at.hasResult) return "No autotune result to apply";
  for (const auto& r : kRules) {
    if (!rule || strcmp(rule, r.id)) continue;
    Settings& s = settings::current();
    float kp = r.kpFactor * g_at.ku;
    float ti = r.tiFactor * g_at.tu;
    float td = r.tdFactor * g_at.tu;
    s.kp = kp;
    s.ki = ti > 0 ? kp / ti : 0;
    s.kd = kp * td;
    g_pid.setGains(s.kp, s.ki, s.kd);
    if (!settings::save()) return "Gains applied but couldn't be saved to flash";
    event("info", String("Applied ") + r.name + " PID gains");
    return "";
  }
  return "Unknown rule";
}

void stop(const char* reason) {
  bool wasActive = g_mode != Mode::Idle;
  endRun("stopped");
  if (wasActive && reason) event("info", reason);
}

void onSettingsChanged(bool pinsChanged) {
  const Settings& s = settings::current();
  g_pid.setGains(s.kp, s.ki, s.kd);
  if (pinsChanged) {
    stop("Stopped: GPIO pins changed");
    releasePins();
    claimPins();
  } else {
    writeSsr(g_ssrOn);  // re-apply in case polarity changed
  }
}

void statusJson(JsonObject o) {
  uint32_t now = millis();
  o["mode"] = modeName(g_mode);
  o["heater"] = g_ssrOn;
  o["duty"] = roundf(g_duty * 10) / 10;
  JsonObject sens = o["sensors"].to<JsonObject>();
  sensorJson(sens["ambient"].to<JsonObject>(), g_sens[0]);
  sensorJson(sens["aux"].to<JsonObject>(), g_sens[1]);

  if (closedLoop(g_mode)) {
    o["setpoint"] = g_setpoint;
    o["sensor"] = sensorName(g_fb);
  } else {
    o["setpoint"] = nullptr;
    o["sensor"] = nullptr;
  }
  if (g_mode == Mode::Hold || g_mode == Mode::Reflow) {
    JsonObject pid = o["pid"].to<JsonObject>();
    pid["p"] = g_pid.p;
    pid["i"] = g_pid.i;
    pid["d"] = g_pid.d;
  }
  const Settings& s = settings::current();
  JsonObject gains = o["gains"].to<JsonObject>();
  gains["kp"] = s.kp;
  gains["ki"] = s.ki;
  gains["kd"] = s.kd;
  o["maxTempC"] = s.maxTempC;

  if (g_run.exists) {
    JsonObject run = o["run"].to<JsonObject>();
    run["kind"] = modeName(g_run.kind);
    run["active"] = g_run.active;
    run["result"] = g_run.result;
    run["sensor"] = sensorName(g_run.sensor);
    run["startSeq"] = g_run.startSeq;
    if (g_run.active) {
      run["endSeq"] = nullptr;
    } else {
      run["endSeq"] = g_run.endSeq;
    }
    run["elapsed"] = ((g_run.active ? now : g_run.endMs) - g_run.startMs) / 1000.0f;
    if (g_run.kind == Mode::Reflow) {
      run["profileId"] = g_run.profileId;
      run["profileName"] = g_run.profileName;
      run["duration"] = g_run.duration;
      run["openDoor"] = g_openDoor;
    }
  } else {
    o["run"] = nullptr;
  }
  if (g_mode == Mode::Autotune || g_at.hasResult) autotuneJson(o["autotune"].to<JsonObject>());

  JsonObject ev = o["event"].to<JsonObject>();
  ev["id"] = g_eventId;
  ev["level"] = g_eventLevel;
  ev["text"] = g_eventText;

  o["seq"] = g_histCount;
  o["uptime"] = now / 1000;
}

void autotuneJson(JsonObject o) {
  o["active"] = g_mode == Mode::Autotune;
  o["target"] = g_at.target;
  o["hysteresis"] = g_at.hyst;
  o["sensor"] = sensorName(g_at.sensor);
  o["cycles"] = g_at.cycles;
  o["cyclesDone"] = g_at.cyclesDone;
  o["measured"] = g_at.measured;
  o["relayOn"] = g_at.relayOn;
  o["phase"] = !g_at.haveOnEdge ? "heating" : g_at.cyclesDone == 0 ? "settling" : "cycling";
  if (!isnan(g_at.lastPeriod)) {
    o["lastPeriod"] = g_at.lastPeriod;
    o["lastAmplitude"] = g_at.lastAmp;
  }
  if (!g_at.hasResult) return;
  JsonObject res = o["result"].to<JsonObject>();
  res["ku"] = g_at.ku;
  res["tu"] = g_at.tu;
  res["amplitude"] = g_at.amp;
  JsonArray rules = res["rules"].to<JsonArray>();
  for (const auto& r : kRules) {
    JsonObject jr = rules.add<JsonObject>();
    float kp = r.kpFactor * g_at.ku;
    float ti = r.tiFactor * g_at.tu;
    jr["id"] = r.id;
    jr["name"] = r.name;
    jr["kp"] = kp;
    jr["ki"] = ti > 0 ? kp / ti : 0;
    jr["kd"] = kp * r.tdFactor * g_at.tu;
  }
}

void historyJson(Print& out, uint32_t since, uint32_t maxSamples) {
  uint32_t first = g_histCount > cfg::kHistoryLen ? g_histCount - cfg::kHistoryLen : 0;
  if (since < first) since = first;
  if (since > g_histCount) since = g_histCount;
  uint32_t end = min(g_histCount, since + maxSamples);
  out.printf("{\"from\":%lu,\"next\":%lu,\"period\":%lu,\"s\":[", static_cast<unsigned long>(since),
             static_cast<unsigned long>(end),
             static_cast<unsigned long>(cfg::kHistoryPeriodMs / 1000));
  for (uint32_t seq = since; seq < end; ++seq) {
    const Sample& s = g_hist[seq % cfg::kHistoryLen];
    out.print(seq == since ? "[" : ",[");
    printTenths(out, s.amb);
    out.print(',');
    printTenths(out, s.aux);
    out.print(',');
    printTenths(out, s.sp);
    out.print(',');
    out.print(s.duty);
    out.print(']');
  }
  out.print("]}");
}

}  // namespace controller
