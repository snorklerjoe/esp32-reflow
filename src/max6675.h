#pragma once

#include <Arduino.h>

enum class TcFault : uint8_t { Ok, Open, NoDevice };

const char* tcFaultName(TcFault f);

// Bit-banged MAX6675 reader. The chip is read-only and tops out at 4.3 MHz,
// so software SPI works on any GPIO and lets several chips share SCK/SO.
class Max6675 {
 public:
  void begin(uint8_t sck, uint8_t so, uint8_t cs);
  void end();
  TcFault read(float& celsius);

 private:
  int sck_ = -1;
  int so_ = -1;
  int cs_ = -1;
};
