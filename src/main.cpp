// Reflow Oven Controller — NodeMCU ESP-32S v1.1
//
// Opens an open Wi-Fi AP and serves a password-protected web app at
// http://192.168.4.1/ (or http://reflow.local/) for controlling a toaster-oven
// reflow setup: one SSR driving the heaters, two MAX6675 thermocouple readers.

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "config.h"
#include "controller.h"
#include "lock.h"
#include "profiles.h"
#include "settings.h"
#include "web.h"

namespace {
DNSServer g_dns;
const IPAddress kApIp(192, 168, 4, 1);
}  // namespace

void setup() {
  Serial.begin(115200);
  stateMutex();  // create before any other task can race for it

  // Get the SSR pin driven to "off" as early as possible.
  settings::load();
  controller::begin();

  if (!profiles::begin()) Serial.println("LittleFS mount failed; profiles unavailable");

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(kApIp, kApIp, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, nullptr, 6, 0, 8);  // open network, channel 6, up to 8 clients

  // Answer every DNS query with our IP so phones show a captive-portal sheet.
  g_dns.setErrorReplyCode(DNSReplyCode::NoError);
  g_dns.start(53, "*", kApIp);

  if (MDNS.begin(MDNS_HOSTNAME)) MDNS.addService("http", "tcp", 80);

  web::begin();
  Serial.printf("Reflow controller v%s: AP \"%s\" at http://%s/\n", FW_VERSION, AP_SSID,
                kApIp.toString().c_str());
}

void loop() {
  g_dns.processNextRequest();
  controller::loop();
  delay(2);
}
