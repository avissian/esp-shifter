// Minimal host fake of the Arduino core, sufficient to compile the real
// main.cpp unmodified. Only the API surface the firmware uses is provided.
#ifndef TESTS_FAKES_ARDUINO_H
#define TESTS_FAKES_ARDUINO_H

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdint.h>
#include <string>

#include "fake_state.h"

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

// NodeMCU Amica pin names used by the firmware.
#define LED_BUILTIN 2
#define D1 5
#define D2 4

#define PROGMEM

class __FlashStringHelper;
#define F(x) (reinterpret_cast<const __FlashStringHelper*>(x))
#define FPSTR(p) (reinterpret_cast<const __FlashStringHelper*>(p))

// The subset of Arduino String the firmware uses: construction from C and
// flash strings, concatenation, length, c_str and comparisons.
class String {
 public:
  String() {}
  String(const char* s) : s_(s ? s : "") {}
  String(const __FlashStringHelper* s) : s_((const char*)s) {}

  unsigned int length() const { return (unsigned int)s_.size(); }
  const char* c_str() const { return s_.c_str(); }

  String& operator+=(const char* s) {
    s_ += (s ? s : "");
    return *this;
  }
  String& operator+=(const String& other) {
    s_ += other.s_;
    return *this;
  }
  String& operator+=(char c) {
    s_ += c;
    return *this;
  }
  String operator+(const char* s) const {
    String result(*this);
    result += s;
    return result;
  }
  String operator+(const String& other) const {
    String result(*this);
    result += other;
    return result;
  }
  bool operator==(const char* s) const { return s_ == (s ? s : ""); }
  bool operator==(const String& other) const { return s_ == other.s_; }
  bool operator!=(const char* s) const { return !(*this == s); }
  bool operator!=(const String& other) const { return !(*this == other); }

 private:
  std::string s_;
};

inline String operator+(const char* a, const String& b) {
  String result(a);
  result += b;
  return result;
}

// ---- clock / GPIO: all state lives in fake::state() so tests control it ----

inline unsigned long millis() { return fake::state().nowMs; }
inline unsigned long micros() { return fake::state().nowMs * 1000UL; }

// delay() never blocks the host: it advances the virtual clock and counts.
inline void delay(unsigned long ms) {
  fake::state().delayCalls++;
  fake::state().delayMsTotal += ms;
  fake::state().nowMs += ms;
}

inline void pinMode(uint8_t pin, uint8_t mode) {
  fake::state().pinModes[pin % fake::State::kPinCount] = mode;
}
inline void digitalWrite(uint8_t pin, uint8_t value) {
  fake::state().pinOutputs[pin % fake::State::kPinCount] =
      (value != LOW) ? HIGH : LOW;
}
inline int digitalRead(uint8_t pin) {
  return fake::state().pinInputs[pin % fake::State::kPinCount];
}

// ---- Serial: appends to a string so tests can inspect log output ----

class FakeSerial {
 public:
  void begin(unsigned long) {}
  void print(const char* s) { append(s); }
  void print(char c) { fake::state().serialOutput += c; }
  void print(const String& s) { append(s.c_str()); }
  void println() { newline(); }
  void println(const char* s) {
    append(s);
    newline();
  }
  void println(const String& s) {
    append(s.c_str());
    newline();
  }
  void printf(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    append(buffer);
  }

 private:
  static void append(const char* s) {
    fake::state().serialOutput += (s ? s : "");
  }
  static void newline() { fake::state().serialOutput += '\n'; }
};

FakeSerial Serial;

// ESP.restart() is counted instead of rebooting the host process.
// random() is a deterministic LCG on the real 32-bit range (the ESP8266 SDK
// returns a 32-bit long; keeping the host return type at 32 bits preserves
// the firmware's %08x varargs formatting without host warnings).
class EspClass {
 public:
  void restart() { ++fake::state().restarts; }

  unsigned int random() {
    fake::State& s = fake::state();
    const unsigned int value = s.randomState;
    s.randomHistory.push_back(value);
    s.randomState = s.randomState * 1664525u + 1013904223u;
    return value;
  }
};

EspClass ESP;

#endif  // TESTS_FAKES_ARDUINO_H
