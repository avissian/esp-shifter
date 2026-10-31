// Minimal host fake of ESP8266WebServer. Handlers are stored in
// registration order and handleClient() dispatches to the FIRST route
// matching both URI and method — like the linked-list RequestHandler chain
// of the real SDK (a POST-only route never swallows a GET). Basic-auth
// behavior itself (parsing/comparison of the Authorization header) belongs
// to the upstream SDK and is modeled here as plain credentials on the
// simulated request; requestAuthentication() records and produces the 401
// the real server sends.
#ifndef TESTS_FAKES_ESP8266WEBSERVER_H
#define TESTS_FAKES_ESP8266WEBSERVER_H

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "Arduino.h"

enum HTTPMethod { HTTP_ANY, HTTP_GET, HTTP_POST, HTTP_HEAD, HTTP_OPTIONS };

struct FakeHttpRequest {
  FakeHttpRequest() : method(HTTP_GET), hasAuth(false) {}
  HTTPMethod method;
  std::string uri;
  bool hasAuth;
  std::string user;
  std::string password;
  std::vector<std::pair<std::string, std::string> > args;  // form parameters
};

struct FakeHttpResponse {
  FakeHttpResponse() : code(0), sent(false), authRequested(false) {}
  int code;
  bool sent;
  std::string contentType;
  std::string body;
  std::vector<std::pair<std::string, std::string> > headers;
  bool authRequested;
};

class ESP8266WebServer {
 public:
  typedef std::function<void()> THandlerFunction;

  struct Route {
    std::string uri;
    HTTPMethod method;
    THandlerFunction handler;
  };

  explicit ESP8266WebServer(int port = 80)
      : port(port), begins(0), handled(0), notFoundHandled(0),
        hasNotFound(false) {}

  void on(const String& uri, HTTPMethod method, THandlerFunction handler) {
    Route route;
    route.uri = uri.c_str();
    route.method = method;
    route.handler = handler;
    routes.push_back(route);
  }

  void on(const String& uri, THandlerFunction handler) {
    on(uri, HTTP_ANY, handler);
  }

  void onNotFound(THandlerFunction handler) {
    notFound = handler;
    hasNotFound = true;
  }

  void begin() { ++begins; }

  void handleClient() {
    res = FakeHttpResponse();
    ++handled;
    const Route* match = 0;
    for (std::size_t i = 0; i < routes.size(); ++i) {
      if (routes[i].uri == req.uri &&
          (routes[i].method == HTTP_ANY || routes[i].method == req.method)) {
        match = &routes[i];
        break;  // first registration that can handle the request wins
      }
    }
    if (match) {
      match->handler();
    } else if (hasNotFound) {
      ++notFoundHandled;
      notFound();
    }
  }

  void send(int code, const char* contentType, const String& content) {
    res.code = code;
    res.sent = true;
    res.contentType = contentType ? contentType : "";
    res.body = content.c_str();
  }

  void sendHeader(const String& name, const String& value, bool = false) {
    res.headers.push_back(
        std::make_pair(name.c_str(), value.c_str()));
  }

  // The upstream SDK parses and compares the Basic credentials; the fake just
  // checks them against the simulated request.
  bool authenticate(const char* user, const char* password) {
    return req.hasAuth && req.user == user && req.password == password;
  }

  void requestAuthentication() {
    // The real server answers 401 directly from this call.
    res.authRequested = true;
    res.code = 401;
    res.sent = true;
    res.headers.push_back(std::make_pair("WWW-Authenticate",
                                         "Basic realm=\"Login Required\""));
  }

  String arg(const char* name) {
    for (std::size_t i = 0; i < req.args.size(); ++i) {
      if (req.args[i].first == name)
        return String(req.args[i].second.c_str());
    }
    return String();
  }

  // ---- test control: simulate an incoming request ----
  void setRequest(HTTPMethod method, const char* uri) {
    req = FakeHttpRequest();
    res = FakeHttpResponse();
    req.method = method;
    req.uri = uri;
  }
  void setBasicAuth(const char* user, const char* password) {
    req.hasAuth = true;
    req.user = user;
    req.password = password;
  }
  void setArg(const char* name, const char* value) {
    req.args.push_back(std::make_pair(name, value));
  }
  void clearRequest() { req = FakeHttpRequest(); }

  void reset() {
    req = FakeHttpRequest();
    res = FakeHttpResponse();
    routes.clear();
    hasNotFound = false;
    begins = handled = notFoundHandled = 0;
  }

  int port;  // kept across reset(): the firmware constructs the server once
  int begins;
  int handled;
  int notFoundHandled;
  FakeHttpRequest req;
  FakeHttpResponse res;
  std::vector<Route> routes;

 private:
  bool hasNotFound;
  THandlerFunction notFound;
};

#endif  // TESTS_FAKES_ESP8266WEBSERVER_H
