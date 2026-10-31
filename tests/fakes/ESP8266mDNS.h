// Minimal host fake of ESP8266mDNS: records begin()/addService()/
// addServiceTxt()/update() usage; begin() result is test-controllable.
#ifndef TESTS_FAKES_ESP8266MDNS_H
#define TESTS_FAKES_ESP8266MDNS_H

#include <string>

#include "Arduino.h"

class MDNSResponder {
 public:
  MDNSResponder()
      : begins(0), beginResult(true), services(0), servicesTxt(0), updates(0) {}

  bool begin(const char* host) {
    ++begins;
    lastHost = host;
    return beginResult;
  }

  void addService(const String& name, const String& /*proto*/,
                  uint16_t /*port*/) {
    ++services;
    lastService = name.c_str();
  }

  void addServiceTxt(const String& /*name*/, const String& /*proto*/,
                     const String& /*key*/, const String& /*value*/) {
    ++servicesTxt;
  }

  void update() { ++updates; }

  void reset() {
    begins = 0;
    beginResult = true;
    services = 0;
    servicesTxt = 0;
    updates = 0;
    lastHost.clear();
    lastService.clear();
  }

  int begins;
  bool beginResult;
  int services;
  int servicesTxt;
  int updates;
  std::string lastHost;
  std::string lastService;
};

MDNSResponder MDNS;

#endif  // TESTS_FAKES_ESP8266MDNS_H
