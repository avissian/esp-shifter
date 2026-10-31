// Minimal host fake of the ArduinoOTA library: records hostname/password
// setup, begin() and handle() calls.
#ifndef TESTS_FAKES_ARDUINOOTA_H
#define TESTS_FAKES_ARDUINOOTA_H

#include <string>

#include "Arduino.h"

class ArduinoOTAClass {
 public:
  ArduinoOTAClass()
      : hostnameCalls(0), passwordCalls(0), begins(0), handled(0) {}

  void setHostname(const char* hostname) {
    ++hostnameCalls;
    hostnameValue = hostname;
  }
  void setPassword(const char* password) {
    ++passwordCalls;
    passwordValue = password;
  }
  void begin() { ++begins; }
  void handle() { ++handled; }

  void reset() {
    hostnameCalls = 0;
    passwordCalls = 0;
    begins = 0;
    handled = 0;
    hostnameValue.clear();
    passwordValue.clear();
  }

  int hostnameCalls;
  int passwordCalls;
  int begins;
  int handled;
  std::string hostnameValue;
  std::string passwordValue;
};

ArduinoOTAClass ArduinoOTA;

#endif  // TESTS_FAKES_ARDUINOOTA_H
