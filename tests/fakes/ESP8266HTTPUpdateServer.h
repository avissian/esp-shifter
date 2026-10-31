// Minimal host fake of ESP8266HTTPUpdateServer. Per the task, the fake only
// RECORDS the login/password the firmware hands over (the update endpoint's
// authentication behavior itself is verified upstream in the ESP8266 core).
#ifndef TESTS_FAKES_ESP8266HTTPUPDATESERVER_H
#define TESTS_FAKES_ESP8266HTTPUPDATESERVER_H

#include <string>

#include "Arduino.h"
#include "ESP8266WebServer.h"

class ESP8266HTTPUpdateServer {
 public:
  ESP8266HTTPUpdateServer() : setups(0), server(0) {}

  void setup(ESP8266WebServer* server, const String& path,
             const String& username, const String& password) {
    ++setups;
    this->server = server;
    this->path = path.c_str();
    this->username = username.c_str();
    this->password = password.c_str();
  }

  void reset() {
    setups = 0;
    server = 0;
    path.clear();
    username.clear();
    password.clear();
  }

  int setups;
  ESP8266WebServer* server;
  std::string path;
  std::string username;
  std::string password;
};

#endif  // TESTS_FAKES_ESP8266HTTPUPDATESERVER_H
