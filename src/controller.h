#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

enum class Mode : uint8_t { Idle, Manual, Hold, Reflow, Autotune };
enum class Sensor : uint8_t { Ambient = 0, Aux = 1 };

const char* sensorName(Sensor s);
bool parseSensor(const char* name, Sensor& out);

// Oven control: sensors, SSR output, closed-loop modes, safety, history.
// All functions except begin()/loop() expect the caller to hold LockGuard.
// Command functions return "" on success, otherwise a user-facing error.
namespace controller {

void begin();
void loop();  // call continuously from the Arduino loop

String setManual(bool on);  // off == stop everything
String startHold(float setpoint, Sensor sensor);
String startReflow(int profileId, Sensor sensor);
String startAutotune(float target, Sensor sensor, float hysteresis, int cycles);
String applyAutotune(const char* rule);
void stop(const char* reason);

// Call after settings::current() changes.
void onSettingsChanged(bool pinsChanged);

void statusJson(JsonObject out);
void autotuneJson(JsonObject out);
void historyJson(Print& out, uint32_t since, uint32_t maxSamples);

}  // namespace controller
