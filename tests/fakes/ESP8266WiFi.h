// Minimal host fake of ESP8266WiFi: a test-driven station status, recorded
// mode/AP/STA calls, and TCP server/client objects backed by shared
// connection records the test can inspect and feed directly.
#ifndef TESTS_FAKES_ESP8266WIFI_H
#define TESTS_FAKES_ESP8266WIFI_H

#include <memory>
#include <string>
#include <vector>

#include "Arduino.h"

enum wl_status_t { WL_DISCONNECTED = 0, WL_CONNECTED = 3 };

enum WiFiMode_t { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 };

class IPAddress {
 public:
  IPAddress() : a_(0), b_(0), c_(0), d_(0) {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
      : a_(a), b_(b), c_(c), d_(d) {}

  String toString() const {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", a_, b_, c_, d_);
    return String(buf);
  }

  bool operator==(const IPAddress& other) const {
    return a_ == other.a_ && b_ == other.b_ && c_ == other.c_ &&
           d_ == other.d_;
  }

  uint8_t operator[](int i) const {
    return i == 0 ? a_ : i == 1 ? b_ : i == 2 ? c_ : d_;
  }

 private:
  uint8_t a_, b_, c_, d_;
};

// One TCP connection between the firmware and a test-driven peer.
struct FakeTcpConnection {
  FakeTcpConnection() : connected(true), rxPos(0) {}
  bool connected;
  std::string rx;   // peer -> firmware (test appends here)
  std::size_t rxPos;
  std::string tx;   // firmware -> peer (test inspects here)
  IPAddress peer;
};

typedef std::shared_ptr<FakeTcpConnection> FakeTcpConnectionPtr;

class WiFiClient {
 public:
  WiFiClient() {}
  explicit WiFiClient(FakeTcpConnectionPtr conn) : conn_(conn) {}

  bool connected() const { return conn_ && conn_->connected; }
  int available() {
    return conn_ ? static_cast<int>(conn_->rx.size() - conn_->rxPos) : 0;
  }
  int read() {
    if (!conn_ || conn_->rxPos >= conn_->rx.size()) return -1;
    return static_cast<unsigned char>(conn_->rx[conn_->rxPos++]);
  }
  size_t write(const uint8_t* buf, size_t size) {
    if (!conn_ || !conn_->connected) return 0;
    conn_->tx.append(reinterpret_cast<const char*>(buf), size);
    return size;
  }
  void stop() {
    if (conn_) conn_->connected = false;
  }
  IPAddress remoteIP() const { return conn_ ? conn_->peer : IPAddress(); }

  // Test access to the underlying connection record.
  FakeTcpConnectionPtr conn_;
};

class WiFiServer {
 public:
  explicit WiFiServer(uint16_t port) : port(port), begins(0), noDelayCalls(0) {}

  void begin() { ++begins; }
  void setNoDelay(bool) { ++noDelayCalls; }
  bool hasClient() const { return !pending.empty(); }
  WiFiClient accept() {
    if (pending.empty()) return WiFiClient();
    WiFiClient client(pending.front());
    pending.erase(pending.begin());
    return client;
  }

  // Test hook: a new peer knocks on the listening socket.
  void offerConnection(FakeTcpConnectionPtr conn) { pending.push_back(conn); }

  void reset() {
    pending.clear();
    begins = 0;
    noDelayCalls = 0;
  }

  uint16_t port;  // kept across reset(): the firmware constructs the server once
  int begins;
  int noDelayCalls;
  std::vector<FakeTcpConnectionPtr> pending;
};

class WiFiClass {
 public:
  WiFiClass()
      : wlStatus(WL_CONNECTED),
        staIp(IPAddress(192, 168, 1, 50)),
        apIp(IPAddress(192, 168, 4, 1)),
        persistentCalls(0),
        beginCalls(0),
        softApCalls(0),
        reconnectCalls(0),
        hostnameCalls(0),
        persistentValue(false),
        currentMode(WIFI_OFF) {}

  wl_status_t status() { return wlStatus; }
  void mode(WiFiMode_t m) {
    currentMode = m;
    modeHistory.push_back(m);
  }
  void persistent(bool p) {
    ++persistentCalls;
    persistentValue = p;
  }
  void begin(const char* ssid, const char* pass) {
    ++beginCalls;
    beginSsid = ssid;
    beginPass = pass;
  }
  bool softAP(const char* ssid, const char* psk = "", int = 1, int = 0,
              int = 4) {
    ++softApCalls;
    softApSsid = ssid;
    softApPass = psk;
    return true;
  }
  IPAddress localIP() { return staIp; }
  IPAddress softAPIP() { return apIp; }
  void hostname(const char* name) {
    ++hostnameCalls;
    hostnameValue = name;
  }
  void reconnect() { ++reconnectCalls; }
  String macAddress() { return String("5C:CF:7F:11:22:33"); }

  void reset() {
    wlStatus = WL_CONNECTED;
    staIp = IPAddress(192, 168, 1, 50);
    apIp = IPAddress(192, 168, 4, 1);
    modeHistory.clear();
    persistentCalls = beginCalls = softApCalls = reconnectCalls = 0;
    hostnameCalls = 0;
    persistentValue = false;
    currentMode = WIFI_OFF;
    beginSsid.clear();
    beginPass.clear();
    softApSsid.clear();
    softApPass.clear();
    hostnameValue.clear();
  }

  // Test knobs and recorded calls.
  wl_status_t wlStatus;
  IPAddress staIp;
  IPAddress apIp;
  std::vector<WiFiMode_t> modeHistory;
  int persistentCalls;
  int beginCalls;
  int softApCalls;
  int reconnectCalls;
  int hostnameCalls;
  bool persistentValue;
  WiFiMode_t currentMode;
  std::string beginSsid;
  std::string beginPass;
  std::string softApSsid;
  std::string softApPass;
  std::string hostnameValue;
};

WiFiClass WiFi;

#endif  // TESTS_FAKES_ESP8266WIFI_H
