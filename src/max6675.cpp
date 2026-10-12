#include "max6675.h"

const char* tcFaultName(TcFault f) {
  switch (f) {
    case TcFault::Ok:
      return "ok";
    case TcFault::Open:
      return "open";
    default:
      return "nodev";
  }
}

void Max6675::begin(uint8_t sck, uint8_t so, uint8_t cs) {
  sck_ = sck;
  so_ = so;
  cs_ = cs;
  digitalWrite(cs_, HIGH);
  pinMode(cs_, OUTPUT);
  digitalWrite(cs_, HIGH);
  pinMode(sck_, OUTPUT);
  digitalWrite(sck_, LOW);
  // Pull SO up where possible so a missing chip reads 0xFFFF (detected below).
  // GPIO 34-39 have no internal pull-ups.
  pinMode(so_, so_ < 34 ? INPUT_PULLUP : INPUT);
}

void Max6675::end() {
  if (cs_ >= 0) pinMode(cs_, INPUT);
  cs_ = sck_ = so_ = -1;
}

TcFault Max6675::read(float& celsius) {
  if (cs_ < 0) return TcFault::NoDevice;

  // D15 appears when CS falls; each later bit shifts out on SCK's falling edge.
  digitalWrite(sck_, LOW);
  digitalWrite(cs_, LOW);
  delayMicroseconds(2);
  uint16_t v = 0;
  for (int i = 15; i >= 0; --i) {
    digitalWrite(sck_, LOW);
    delayMicroseconds(2);
    if (digitalRead(so_)) v |= (1u << i);
    digitalWrite(sck_, HIGH);
    delayMicroseconds(2);
  }
  digitalWrite(sck_, LOW);
  digitalWrite(cs_, HIGH);  // starts the next conversion

  // D15 (dummy sign) and D1 (device ID) always read 0 on a real MAX6675.
  if (v & 0x8002) return TcFault::NoDevice;
  if (v & 0x0004) return TcFault::Open;
  celsius = (v >> 3) * 0.25f;
  return TcFault::Ok;
}
