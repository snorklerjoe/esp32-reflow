#pragma once

#include <Arduino.h>

// Secrets are injected from .env by scripts/load_env.py.
#ifndef WEB_PASSWORD
#error "WEB_PASSWORD is not set. Copy .env.example to .env and fill it in."
#endif
#ifndef ADMIN_PASSWORD
#error "ADMIN_PASSWORD is not set. Copy .env.example to .env and fill it in."
#endif
#ifndef AP_SSID
#define AP_SSID "ReflowOven"
#endif

#define FW_VERSION "1.0.0"
#define MDNS_HOSTNAME "reflow"

namespace cfg {

// Default GPIO assignments; changeable at runtime from Settings (admin).
constexpr uint8_t kDefaultPinSsr = 25;
constexpr uint8_t kDefaultPinSck = 18;
constexpr uint8_t kDefaultPinSo = 19;
constexpr uint8_t kDefaultPinCsAmbient = 16;
constexpr uint8_t kDefaultPinCsAux = 17;

// MAX6675 conversions take up to 220 ms; reading faster aborts them.
constexpr uint32_t kSensorPeriodMs = 250;
// Consecutive bad reads of the feedback sensor before a closed-loop run aborts.
constexpr uint8_t kSensorFaultLimit = 8;

constexpr uint32_t kHistoryPeriodMs = 1000;
constexpr size_t kHistoryLen = 3600;  // one hour at 1 Hz

constexpr size_t kMaxProfiles = 24;
constexpr size_t kMaxProfilePoints = 128;
constexpr size_t kMaxNameLen = 48;
constexpr size_t kMaxDescriptionLen = 600;

constexpr uint32_t kAutotuneTimeoutMs = 60UL * 60UL * 1000UL;

constexpr size_t kMaxSessions = 12;
constexpr uint32_t kSessionIdleMs = 12UL * 3600UL * 1000UL;
constexpr uint8_t kLoginFailuresBeforeLockout = 5;
constexpr uint32_t kLoginLockoutMs = 30UL * 1000UL;

}  // namespace cfg
