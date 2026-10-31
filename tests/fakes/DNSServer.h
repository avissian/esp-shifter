// Minimal host fake of the DNSServer library: records start() parameters and
// counts processNextRequest() calls.
#ifndef TESTS_FAKES_DNSSERVER_H
#define TESTS_FAKES_DNSSERVER_H

#include <string>

#include "Arduino.h"
#include "ESP8266WiFi.h"

class DNSServer {
 public:
  DNSServer() : starts(0), processed(0), port(0) {}

  void start(const uint16_t& newPort, const String& domain,
             const IPAddress& resolvedIP) {
    ++starts;
    port = newPort;
    this->domain = domain.c_str();
    ip = resolvedIP;
  }

  void processNextRequest() { ++processed; }

  void reset() {
    starts = 0;
    processed = 0;
    port = 0;
    domain.clear();
  }

  int starts;
  int processed;
  uint16_t port;
  std::string domain;
  IPAddress ip;
};

#endif  // TESTS_FAKES_DNSSERVER_H
