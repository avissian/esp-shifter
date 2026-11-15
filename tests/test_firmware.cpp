// Host-side regression tests for the ESP-Shifter firmware (main.cpp).
//
// The real main.cpp is compiled into this test together with minimal host
// fakes of the Arduino / ESP8266 SDK (tests/fakes/*). Everything a test
// needs to control — the virtual clock, GPIO levels, EEPROM bytes, WiFi
// status, TCP peers, HTTP requests and the deterministic ESP.random()
// sequence — lives in the fakes; the firmware itself is not modified.
//
// Covered firmware behavior:
//   1. setup() with a stored config and OTA password arms the web updater
//      with username "admin" and the real stored password (an empty OTA
//      password arms no updater at all).
//   2. Normal-mode GET /setup and POST /save are registered and guarded by
//      Basic auth: 401 without/with wrong credentials, save+reboot with the
//      correct ones; with an empty OTA password both answer 403 and never
//      touch EEPROM.
//   3. Boot-scoped setup token: setupHttpRoutes() draws it from ESP.random()
//      once, serveSetupForm() embeds it as a hidden field, and handleSave()
//      accepts only the live token (missing/foreign token -> 403 without an
//      EEPROM write or a reboot).
//   4. After startSetupMode() from normal mode, "/" serves the setup form
//      (not the old status page), GET /save does not touch EEPROM (the route
//      is POST-only -> captive redirect) and POST /save in AP mode works
//      without auth but with the token.
//   5. On WiFi loss loop() never delays, calls WiFi.reconnect() exactly once
//      per episode and opens the setup AP after the 20 s timeout; holding
//      both buttons for 5 s enters setup mode while offline.
//   6. Valid OBC feedback does not advance the clock or block; a short button
//      press/release is delivered even with pending TCP data; Haptic pattern
//      0 stops the LED flash; replacing the OBC client resets the
//      half-parsed message.
//
// Build/run: scripts/test.sh (${CXX:-c++} -std=c++11 -Wall -Wextra -Werror
// -pedantic, -Itests/fakes). No third-party libraries.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "fakes/fake_state.h"
#include "../main.cpp"

namespace {

// Spec constants the firmware must obey. Deliberately NOT taken from the
// macros in main.cpp so the tests fail if the firmware drifts from spec.
const unsigned long kSpecSetupHoldMs = 5000UL;     // both buttons held 5 s
const unsigned long kSpecWifiTimeoutMs = 20000UL;  // AP fallback after 20 s

int g_checks = 0;
int g_failures = 0;

// ------------------------------------------------------------ assertions ---

std::string printableOrHex(const std::string& s) {
  std::string out;
  char byte[8];
  for (std::size_t i = 0; i < s.size() && i < 48; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0x20 && c <= 0x7e) {
      out += static_cast<char>(c);
    } else {
      std::snprintf(byte, sizeof byte, "\\x%02x", c);
      out += byte;
    }
  }
  return out;
}

void expectTrue(bool ok, int line, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "line %d: failed: %s\n", line, what);
  }
}

#define EXPECT_TRUE(expr) expectTrue((expr), __LINE__, #expr)

void expectEq(long long actual, long long expected, int line, const char* what) {
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    std::fprintf(stderr, "line %d: %s: expected %lld, got %lld\n", line, what,
                 expected, actual);
  }
}

#define EXPECT_EQ(actual, expected) expectEq((actual), (expected), __LINE__, #actual)

inline const char* toCStr(const char* s) { return s; }
inline const char* toCStr(const std::string& s) { return s.c_str(); }
inline const char* toCStr(const String& s) { return s.c_str(); }

template <typename Actual, typename Expected>
void expectStrImpl(const char* label, const Actual& actual,
                   const Expected& expected, int line) {
  ++g_checks;
  if (std::strcmp(toCStr(actual), toCStr(expected)) != 0) {
    ++g_failures;
    std::fprintf(stderr, "line %d: %s: expected \"%s\", got \"%s\"\n", line,
                 label, toCStr(expected), toCStr(actual));
  }
}

#define EXPECT_STR(label, actual, expected) \
  expectStrImpl((label), (actual), (expected), __LINE__)

void expectContainsImpl(const char* label, const std::string& haystack,
                        const std::string& needle, int line, bool expected) {
  ++g_checks;
  const bool found = haystack.find(needle) != std::string::npos;
  if (found != expected) {
    ++g_failures;
    std::fprintf(stderr, "line %d: %s: expected %s \"%s\"\n", line, label,
                 expected ? "to contain" : "not to contain",
                 printableOrHex(needle).c_str());
  }
}

#define EXPECT_CONTAINS(label, haystack, needle) \
  expectContainsImpl((label), (haystack), std::string(needle), __LINE__, true)
#define EXPECT_NOT_CONTAINS(label, haystack, needle) \
  expectContainsImpl((label), (haystack), std::string(needle), __LINE__, false)

void expectBytesPrefix(const char* label, const std::string& actual,
                       const unsigned char* prefix, std::size_t count,
                       int line) {
  ++g_checks;
  const bool ok = actual.size() >= count &&
                  actual.compare(0, count,
                                 reinterpret_cast<const char*>(prefix),
                                 count) == 0;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "line %d: %s: expected prefix \"%s\", got \"%s\"\n",
                 line, label, printableOrHex(std::string(
                                reinterpret_cast<const char*>(prefix),
                                count)).c_str(),
                 printableOrHex(actual).c_str());
  }
}

#define EXPECT_BYTES_PREFIX(label, actual, prefix, count) \
  expectBytesPrefix((label), (actual), (prefix), (count), __LINE__)

// ------------------------------------------------------- world handling ---

void resetFirmwareGlobals() {
  std::memset(&cfg, 0, sizeof cfg);
  haveConfig = false;
  setupMode = false;
  lastStatusMs = 0;
  setupHoldStart = 0;
  setupHoldActive = false;
  ledFlashing = false;
  ledFlashStart = 0;
  wifiRecovering = false;
  wifiRecoveryStart = 0;
  std::memset(setupToken, 0, sizeof setupToken);
  Button freshUp = { PIN_UP, BTN_SHIFT_UP, false, false, 0 };
  Button freshDown = { PIN_DOWN, BTN_SHIFT_DOWN, false, false, 0 };
  btnUp = freshUp;
  btnDown = freshDown;
  obcClient = WiFiClient();
  obcParser.reset();
}

void resetAll() {
  fake::state() = fake::State();
  EEPROM.reset();
  WiFi.reset();
  MDNS.reset();
  ArduinoOTA.reset();
  httpServer.reset();
  httpUpdater.reset();
  dnsServer.reset();
  obcServer.reset();
  resetFirmwareGlobals();
}

void advance(unsigned long ms) { fake::state().nowMs += ms; }

void pressPin(uint8_t pin) { fake::state().pinInputs[pin] = LOW; }
void releasePin(uint8_t pin) { fake::state().pinInputs[pin] = HIGH; }

// Full button click: raw level change, debounce settle, stable press, then
// the mirrored sequence for the release. Requires loop() calls at each step,
// exactly like on hardware.
void clickButton(Button& b) {
  pressPin(b.pin);
  loop();  // raw level change noticed, debounce timer starts
  advance(DEBOUNCE_MS + 1);
  loop();  // stable press -> ButtonState(press)
  releasePin(b.pin);
  loop();
  advance(DEBOUNCE_MS + 1);
  loop();  // stable release -> ButtonState(release)
}

void storeConfig(const char* ssid, const char* wifiPass, const char* otaPass) {
  Config c;
  std::memset(&c, 0, sizeof c);
  c.magic = CFG_MAGIC;
  std::strncpy(c.ssid, ssid, sizeof c.ssid - 1);
  std::strncpy(c.pass, wifiPass, sizeof c.pass - 1);
  std::strncpy(c.otaPass, otaPass, sizeof c.otaPass - 1);
  c.crc = cfgChecksum(c);
  std::memcpy(EEPROM.rawData(), &c, sizeof c);
}

Config peekConfig() {
  Config c;
  std::memset(&c, 0, sizeof c);
  std::memcpy(&c, EEPROM.rawData(), sizeof c);
  return c;
}

std::string eepromSnapshot() {
  return std::string(reinterpret_cast<const char*>(EEPROM.rawData()),
                     EEPROMClass::kSize);
}

// Boot with a valid stored config and the station already associated, so
// setup() takes the "normal mode" path without waiting.
void bootNormal(const char* ssid = "HomeNet", const char* wifiPass = "wifipass",
                const char* otaPass = "ota-secret-9") {
  storeConfig(ssid, wifiPass, otaPass);
  WiFi.wlStatus = WL_CONNECTED;
  setup();
}

typedef std::vector<std::pair<std::string, std::string> > FakeArgs;

FakeArgs makeSaveArgs(const char* ssid, const char* wifiPass,
                      const char* otaPass, bool withToken = true) {
  FakeArgs args;
  args.push_back(std::make_pair(std::string("ssid"), std::string(ssid)));
  args.push_back(std::make_pair(std::string("pass"), std::string(wifiPass)));
  args.push_back(std::make_pair(std::string("ota"), std::string(otaPass)));
  if (withToken)
    args.push_back(std::make_pair(std::string("token"),
                                  std::string(setupToken)));
  return args;
}

// Simulates one HTTP request against the fake server and returns a copy of
// the response. The request is cleared afterwards so later firmware loop()
// calls do not redispatch it.
FakeHttpResponse performRequest(HTTPMethod method, const char* uri,
                                const char* authUser = 0,
                                const char* authPass = 0,
                                const FakeArgs& args = FakeArgs()) {
  httpServer.setRequest(method, uri);
  for (std::size_t i = 0; i < args.size(); ++i)
    httpServer.setArg(args[i].first.c_str(), args[i].second.c_str());
  if (authUser) httpServer.setBasicAuth(authUser, authPass);
  httpServer.handleClient();
  const FakeHttpResponse copy = httpServer.res;
  httpServer.clearRequest();
  return copy;
}

// A new OBC peer knocks; one loop() lets the firmware accept it.
FakeTcpConnectionPtr connectObcPeer(int lastOctet = 7) {
  FakeTcpConnectionPtr conn(new FakeTcpConnection);
  conn->peer = IPAddress(10, 0, 0, static_cast<uint8_t>(lastOctet));
  obcServer.offerConnection(conn);
  loop();
  return conn;
}

void peerSend(const FakeTcpConnectionPtr& conn, const unsigned char* bytes,
              std::size_t count) {
  conn->rx.append(reinterpret_cast<const char*>(bytes), count);
}

const unsigned char kStatusFrame[] = {0x02, 0xFF, 0x01};  // DeviceStatus

// --------------------------------------------- setup(): OTA and routes ---

void testSetupArmsOtaUpdaterWithStoredPassword() {
  resetAll();
  bootNormal("HomeNet", "wifipass", "ota-secret-9");

  EXPECT_TRUE(!setupMode);
  EXPECT_EQ(httpUpdater.setups, 1);
  EXPECT_TRUE(httpUpdater.server == &httpServer);
  EXPECT_STR("ota path", httpUpdater.path, "/update");
  EXPECT_STR("ota user", httpUpdater.username, "admin");  // non-empty admin
  EXPECT_STR("ota pass", httpUpdater.password, "ota-secret-9");  // real one
  EXPECT_EQ(ArduinoOTA.begins, 1);
  EXPECT_STR("arduino ota pass", ArduinoOTA.passwordValue, "ota-secret-9");

  EXPECT_EQ(obcServer.begins, 1);
  EXPECT_TRUE(obcServer.port == OBC_PORT);
  EXPECT_EQ(httpServer.begins, 1);
  EXPECT_EQ(MDNS.begins, 1);
  EXPECT_EQ(MDNS.services, 1);
  EXPECT_EQ(MDNS.servicesTxt, 6);
  EXPECT_EQ(WiFi.beginCalls, 1);
  EXPECT_STR("wifi ssid", WiFi.beginSsid, "HomeNet");
  EXPECT_STR("wifi pass", WiFi.beginPass, "wifipass");
  EXPECT_EQ(EEPROM.begins, 1);  // loadConfig() read the stored image
  EXPECT_EQ(fake::state().delayCalls, 0);  // already connected at boot

  // Routes registered once, in registration order (fake keeps the order).
  EXPECT_EQ(httpServer.routes.size(), 5);
  EXPECT_STR("route 0", httpServer.routes[0].uri, "/");
  EXPECT_TRUE(httpServer.routes[0].method == HTTP_GET);
  EXPECT_STR("route 1", httpServer.routes[1].uri, "/setup");
  EXPECT_TRUE(httpServer.routes[1].method == HTTP_GET);
  EXPECT_STR("route 2", httpServer.routes[2].uri, "/save");
  EXPECT_TRUE(httpServer.routes[2].method == HTTP_POST);
  EXPECT_STR("route 3", httpServer.routes[3].uri, "/generate_204");
  EXPECT_TRUE(httpServer.routes[3].method == HTTP_GET);
  EXPECT_STR("route 4", httpServer.routes[4].uri, "/fwlink");
  EXPECT_TRUE(httpServer.routes[4].method == HTTP_GET);
}

void testSetupTokenIsBootScoped() {
  resetAll();
  bootNormal();

  // The token is drawn exactly once (four ESP.random() calls) at boot...
  EXPECT_EQ(fake::state().randomHistory.size(), 4);
  EXPECT_EQ(std::strlen(setupToken), 32);
  // C++11 does not specify argument evaluation order: Clang and GCC can draw
  // the four random values in opposite orders. Verify the complete multiset
  // of emitted blocks, not an incidental compiler-specific order.
  std::vector<std::string> expectedBlocks;
  std::vector<std::string> actualBlocks;
  for (std::size_t i = 0; i < 4; ++i) {
    char block[9];
    std::snprintf(block, sizeof block, "%08x", fake::state().randomHistory[i]);
    expectedBlocks.push_back(block);
    actualBlocks.push_back(std::string(setupToken + i * 8, 8));
  }
  std::sort(expectedBlocks.begin(), expectedBlocks.end());
  std::sort(actualBlocks.begin(), actualBlocks.end());
  EXPECT_TRUE(actualBlocks == expectedBlocks);
  const std::string tokenAtBoot(setupToken);

  // ...and survives a later switch into setup mode (no redraw).
  WiFi.wlStatus = WL_DISCONNECTED;
  loop();  // loss detected
  advance(kSpecWifiTimeoutMs + 1);
  loop();  // timeout -> startSetupMode()
  EXPECT_TRUE(setupMode);
  EXPECT_EQ(fake::state().randomHistory.size(), 4);
  EXPECT_STR("token stable", setupToken, tokenAtBoot.c_str());
}

void testEmptyOtaPasswordLocksOutLanSetupAndSave() {
  resetAll();
  bootNormal("HomeNet", "wifipass", "");

  EXPECT_EQ(httpUpdater.setups, 0);
  EXPECT_EQ(ArduinoOTA.begins, 0);

  const std::string before = eepromSnapshot();
  FakeHttpResponse r = performRequest(HTTP_GET, "/setup");
  EXPECT_EQ(r.code, 403);
  EXPECT_CONTAINS("403 hint", r.body, "ESP-Shifter-Setup");

  r = performRequest(HTTP_POST, "/save", "admin", "whatever",
                     makeSaveArgs("NewNet", "p", "o"));
  EXPECT_EQ(r.code, 403);
  EXPECT_EQ(fake::state().restarts, 0);
  EXPECT_TRUE(eepromSnapshot() == before);  // nothing persisted
}

void testSetupPageBasicAuth() {
  resetAll();
  bootNormal();

  FakeHttpResponse r = performRequest(HTTP_GET, "/setup");
  EXPECT_EQ(r.code, 401);
  EXPECT_TRUE(r.authRequested);

  r = performRequest(HTTP_GET, "/setup", "admin", "wrong");
  EXPECT_EQ(r.code, 401);
  EXPECT_TRUE(r.authRequested);

  r = performRequest(HTTP_GET, "/setup", "root", "ota-secret-9");
  EXPECT_EQ(r.code, 401);

  r = performRequest(HTTP_GET, "/setup", "admin", "ota-secret-9");
  EXPECT_EQ(r.code, 200);
  EXPECT_STR("type", r.contentType, "text/html");
  EXPECT_CONTAINS("form", r.body, "<form");
  EXPECT_CONTAINS("ssid field", r.body, "name='ssid'");
  EXPECT_CONTAINS("ota field", r.body, "name='ota'");
  EXPECT_CONTAINS("token field", r.body, "name='token'");
  EXPECT_CONTAINS("token value", r.body,
                  std::string("value='") + setupToken + "'");
  // Saved secrets are never echoed back into the form.
  EXPECT_NOT_CONTAINS("wifi pass leak", r.body, "wifipass");
  EXPECT_NOT_CONTAINS("ota pass leak", r.body, "ota-secret-9");
}

void testSaveRequiresValidFormToken() {
  resetAll();
  bootNormal();
  const std::string before = eepromSnapshot();
  const int commits0 = EEPROM.commits;

  // Authorized but no token at all.
  FakeHttpResponse r =
      performRequest(HTTP_POST, "/save", "admin", "ota-secret-9",
                     makeSaveArgs("NewNet", "p", "o", false));
  EXPECT_EQ(r.code, 403);
  EXPECT_CONTAINS("hint", r.body, "Invalid form token");
  EXPECT_TRUE(eepromSnapshot() == before);
  EXPECT_EQ(fake::state().restarts, 0);
  EXPECT_EQ(EEPROM.commits, commits0);

  // Authorized but a foreign token.
  FakeArgs args = makeSaveArgs("NewNet", "p", "o", false);
  args.push_back(std::make_pair(std::string("token"),
                                std::string("deadbeefdeadbeefdeadbeefdeadbeef")));
  r = performRequest(HTTP_POST, "/save", "admin", "ota-secret-9", args);
  EXPECT_EQ(r.code, 403);
  EXPECT_TRUE(eepromSnapshot() == before);
  EXPECT_EQ(fake::state().restarts, 0);
  EXPECT_EQ(EEPROM.commits, commits0);

  // The live token from the form saves and reboots.
  r = performRequest(HTTP_POST, "/save", "admin", "ota-secret-9",
                     makeSaveArgs("NewNet", "p", "o", true));
  EXPECT_EQ(r.code, 200);
  EXPECT_EQ(fake::state().restarts, 1);
  EXPECT_EQ(EEPROM.commits, commits0 + 1);
}

void testSaveAuthPersistsAndReboots() {
  resetAll();
  bootNormal();
  const std::string before = eepromSnapshot();

  FakeHttpResponse r = performRequest(HTTP_POST, "/save", 0, 0,
                                      makeSaveArgs("NewNet", "p", "o"));
  EXPECT_EQ(r.code, 401);
  EXPECT_EQ(fake::state().restarts, 0);
  EXPECT_TRUE(eepromSnapshot() == before);

  r = performRequest(HTTP_POST, "/save", "admin", "wrong",
                     makeSaveArgs("NewNet", "p", "o"));
  EXPECT_EQ(r.code, 401);
  EXPECT_EQ(fake::state().restarts, 0);
  EXPECT_TRUE(eepromSnapshot() == before);

  const int commits0 = EEPROM.commits;
  const int puts0 = EEPROM.puts;
  r = performRequest(HTTP_POST, "/save", "admin", "ota-secret-9",
                     makeSaveArgs("NewNet", "new-pass", "new-ota", true));
  EXPECT_EQ(r.code, 200);
  EXPECT_CONTAINS("saved", r.body, "Saved");
  EXPECT_EQ(fake::state().restarts, 1);  // reboot after save
  EXPECT_EQ(EEPROM.commits, commits0 + 1);
  EXPECT_EQ(EEPROM.puts, puts0 + 1);

  const Config saved = peekConfig();
  EXPECT_TRUE(saved.magic == CFG_MAGIC);
  EXPECT_STR("ssid", saved.ssid, "NewNet");
  EXPECT_STR("pass", saved.pass, "new-pass");
  EXPECT_STR("ota", saved.otaPass, "new-ota");
  EXPECT_TRUE(saved.crc == cfgChecksum(saved));
  EXPECT_CONTAINS("log", fake::state().serialOutput,
                  "[SETUP] saved SSID 'NewNet'");
}

void testSaveRejectsInvalidInput() {
  resetAll();
  bootNormal();
  const std::string before = eepromSnapshot();

  // Valid token, but empty SSID.
  FakeHttpResponse r = performRequest(HTTP_POST, "/save", "admin",
                                      "ota-secret-9",
                                      makeSaveArgs("", "", "", true));
  EXPECT_EQ(r.code, 400);
  EXPECT_EQ(fake::state().restarts, 0);
  EXPECT_TRUE(eepromSnapshot() == before);

  // Valid token, but a 33-character SSID.
  r = performRequest(HTTP_POST, "/save", "admin", "ota-secret-9",
                     makeSaveArgs(std::string(33, 'x').c_str(), "", "",
                                  true));
  EXPECT_EQ(r.code, 400);
  EXPECT_EQ(fake::state().restarts, 0);
  EXPECT_TRUE(eepromSnapshot() == before);
}

void testSaveCommitFailureReturns500() {
  resetAll();
  bootNormal();
  EEPROM.commitResult = false;

  const FakeHttpResponse r =
      performRequest(HTTP_POST, "/save", "admin", "ota-secret-9",
                     makeSaveArgs("NewNet", "p", "o", true));
  EXPECT_EQ(r.code, 500);
  EXPECT_EQ(fake::state().restarts, 0);  // no reboot when the save failed
  // Like the real SDK, put() has already touched the RAM image, but the
  // in-memory config must not switch to the unsaved values.
  EXPECT_STR("cfg unchanged", cfg.ssid, "HomeNet");
  EXPECT_STR("cfg ota unchanged", cfg.otaPass, "ota-secret-9");
}

// ------------------------------------------------------ boot variations ---

void testBootWithoutConfigOpensPortal() {
  resetAll();  // EEPROM all zeroes -> loadConfig() fails
  WiFi.wlStatus = WL_DISCONNECTED;
  setup();

  EXPECT_TRUE(setupMode);
  EXPECT_EQ(WiFi.softApCalls, 1);
  EXPECT_STR("ap ssid", WiFi.softApSsid, "ESP-Shifter-Setup");
  EXPECT_STR("ap pass", WiFi.softApPass, "esp-shifter-2026");
  EXPECT_EQ(dnsServer.starts, 1);
  EXPECT_TRUE(dnsServer.port == 53);
  EXPECT_STR("dns domain", dnsServer.domain, "*");
  EXPECT_EQ(httpServer.begins, 1);
  EXPECT_EQ(httpServer.routes.size(), 5);  // routes exist even in AP mode
  EXPECT_EQ(WiFi.beginCalls, 0);           // never even tried STA

  const FakeHttpResponse r = performRequest(HTTP_GET, "/");
  EXPECT_EQ(r.code, 200);
  EXPECT_CONTAINS("form", r.body, "<form");
  EXPECT_CONTAINS("form token", r.body,
                  std::string("value='") + setupToken + "'");
}

void testBootBothButtonsHeldForcesSetup() {
  resetAll();
  storeConfig("HomeNet", "wifipass", "ota-secret-9");
  pressPin(PIN_UP);
  pressPin(PIN_DOWN);
  WiFi.wlStatus = WL_CONNECTED;
  setup();

  EXPECT_TRUE(setupMode);
  EXPECT_EQ(WiFi.softApCalls, 1);
  EXPECT_EQ(WiFi.beginCalls, 0);  // returned before any STA attempt
}

void testBootWifiUnreachableEntersSetupAfterTimeout() {
  resetAll();
  storeConfig("HomeNet", "wifipass", "ota-secret-9");
  WiFi.wlStatus = WL_DISCONNECTED;
  setup();

  EXPECT_TRUE(setupMode);
  EXPECT_EQ(WiFi.softApCalls, 1);
  // The connect wait is delay-driven: the virtual clock ran the full 20 s
  // timeout in 500 ms chunks.
  EXPECT_EQ(fake::state().delayCalls, kSpecWifiTimeoutMs / 500);
  EXPECT_EQ(fake::state().nowMs, kSpecWifiTimeoutMs);
}

void testNormalModeRootShowsStatus() {
  resetAll();
  bootNormal();

  FakeHttpResponse r = performRequest(HTTP_GET, "/");
  EXPECT_EQ(r.code, 200);
  EXPECT_STR("type", r.contentType, "text/plain");
  EXPECT_CONTAINS("version", r.body, FW_VERSION);
  EXPECT_CONTAINS("status", r.body, "OBC clients");
  EXPECT_CONTAINS("ip", r.body, "192.168.1.50");
  EXPECT_NOT_CONTAINS("form leak", r.body, "<form");

  r = performRequest(HTTP_GET, "/generate_204");
  EXPECT_EQ(r.code, 200);
  EXPECT_CONTAINS("status", r.body, "OBC clients");

  r = performRequest(HTTP_GET, "/nope");
  EXPECT_EQ(r.code, 404);
}

// ---------------------------------------------------- WiFi loss / loop ---

void testWifiLossNoDelaySingleReconnectApTimeout() {
  resetAll();
  bootNormal();

  WiFi.wlStatus = WL_DISCONNECTED;
  const unsigned long delaysBefore = fake::state().delayCalls;
  loop();  // first loop after the loss
  EXPECT_EQ(WiFi.reconnectCalls, 1);
  EXPECT_EQ(fake::state().delayCalls, delaysBefore);  // never blocks
  EXPECT_TRUE(!setupMode);
  EXPECT_EQ(WiFi.softApCalls, 0);

  // 19.9 s of polling: still reconnecting, still no delay, no second call.
  const unsigned long lossAt = fake::state().nowMs;
  while (fake::state().nowMs - lossAt < kSpecWifiTimeoutMs - 100) {
    fake::state().nowMs += 100;
    loop();
  }
  EXPECT_EQ(WiFi.reconnectCalls, 1);
  EXPECT_EQ(fake::state().delayCalls, delaysBefore);
  EXPECT_TRUE(!setupMode);
  EXPECT_EQ(WiFi.softApCalls, 0);

  // Cross the 20 s mark: the setup AP opens.
  fake::state().nowMs += 200;
  loop();
  EXPECT_TRUE(setupMode);
  EXPECT_EQ(WiFi.softApCalls, 1);
  EXPECT_STR("ap ssid", WiFi.softApSsid, "ESP-Shifter-Setup");
  EXPECT_EQ(WiFi.reconnectCalls, 1);  // never hammered
  EXPECT_EQ(fake::state().delayCalls, delaysBefore);
  EXPECT_TRUE(WiFi.currentMode == WIFI_AP);
}

void testWifiRecoveryThenSecondLossReconnectsAgain() {
  resetAll();
  bootNormal();
  EXPECT_EQ(MDNS.begins, 1);

  WiFi.wlStatus = WL_DISCONNECTED;
  loop();
  EXPECT_EQ(WiFi.reconnectCalls, 1);

  WiFi.wlStatus = WL_CONNECTED;
  loop();
  EXPECT_TRUE(!wifiRecovering);
  EXPECT_EQ(MDNS.begins, 2);  // mDNS re-announced on the current IP
  EXPECT_EQ(WiFi.reconnectCalls, 1);

  // The station is fully working again after recovery.
  const FakeHttpResponse r = performRequest(HTTP_GET, "/");
  EXPECT_EQ(r.code, 200);

  WiFi.wlStatus = WL_DISCONNECTED;
  loop();
  EXPECT_EQ(WiFi.reconnectCalls, 2);  // new episode -> one new reconnect
  EXPECT_EQ(WiFi.softApCalls, 0);
}

void enterSetupFromNormalViaWifiTimeout() {
  WiFi.wlStatus = WL_DISCONNECTED;
  loop();  // loss detected
  advance(kSpecWifiTimeoutMs + 1);
  loop();  // timeout -> startSetupMode()
  EXPECT_TRUE(setupMode);
}

void testSetupModePortalServing() {
  resetAll();
  bootNormal();
  const std::size_t routesBefore = httpServer.routes.size();
  const FakeHttpResponse normal = performRequest(HTTP_GET, "/");
  EXPECT_CONTAINS("status before", normal.body, "OBC clients");

  enterSetupFromNormalViaWifiTimeout();

  // Routes are registered once: switching modes must not duplicate/shadow.
  EXPECT_EQ(httpServer.routes.size(), routesBefore);

  // Root now shows the form, not the previous status text.
  FakeHttpResponse r = performRequest(HTTP_GET, "/");
  EXPECT_EQ(r.code, 200);
  EXPECT_STR("type", r.contentType, "text/html");
  EXPECT_CONTAINS("form", r.body, "<form");
  EXPECT_NOT_CONTAINS("old status", r.body, "OBC clients");

  r = performRequest(HTTP_GET, "/generate_204");
  EXPECT_EQ(r.code, 200);
  EXPECT_CONTAINS("captive", r.body, "<form");

  // GET /save matches no route (POST-only) -> captive redirect; the first
  // match runs and must not touch EEPROM.
  const std::string before = eepromSnapshot();
  r = performRequest(HTTP_GET, "/save");
  EXPECT_EQ(r.code, 302);
  std::string location;
  for (std::size_t i = 0; i < r.headers.size(); ++i) {
    if (r.headers[i].first == "Location") location = r.headers[i].second;
  }
  EXPECT_STR("location", location, "http://192.168.4.1/");
  EXPECT_TRUE(eepromSnapshot() == before);

  r = performRequest(HTTP_GET, "/unknown-page");
  EXPECT_EQ(r.code, 302);

  // In AP mode POST /save needs no Basic auth but a valid token.
  const int commits0 = EEPROM.commits;
  r = performRequest(HTTP_POST, "/save", 0, 0,
                     makeSaveArgs("PortalNet", "pp", "oo", false));
  EXPECT_EQ(r.code, 403);
  EXPECT_TRUE(eepromSnapshot() == before);
  EXPECT_EQ(fake::state().restarts, 0);

  r = performRequest(HTTP_POST, "/save", 0, 0,
                     makeSaveArgs("PortalNet", "pp", "oo", true));
  EXPECT_EQ(r.code, 200);
  EXPECT_EQ(fake::state().restarts, 1);
  EXPECT_EQ(EEPROM.commits, commits0 + 1);
  const Config saved = peekConfig();
  EXPECT_STR("ssid", saved.ssid, "PortalNet");
  EXPECT_STR("ota", saved.otaPass, "oo");
}

void testButtonHoldEntersSetupWhileOffline() {
  resetAll();
  bootNormal();
  WiFi.wlStatus = WL_DISCONNECTED;
  loop();  // loss detected, recovery window opens
  EXPECT_EQ(WiFi.reconnectCalls, 1);

  pressPin(PIN_UP);
  pressPin(PIN_DOWN);
  loop();  // raw level change noticed
  advance(DEBOUNCE_MS + 1);
  loop();  // both stable -> hold timer starts
  EXPECT_TRUE(!setupMode);

  const unsigned long delaysBefore = fake::state().delayCalls;
  advance(kSpecSetupHoldMs - 1);
  loop();
  EXPECT_TRUE(!setupMode);  // just under 5 s
  advance(2);
  loop();
  EXPECT_TRUE(setupMode);  // 5 s reached
  EXPECT_EQ(WiFi.softApCalls, 1);
  EXPECT_EQ(WiFi.reconnectCalls, 1);  // hold does not spam reconnect
  EXPECT_EQ(fake::state().delayCalls, delaysBefore);  // offline, never blocks
}

// ------------------------------------------------------ OBC / buttons ---

void testObcAcceptStatusReplacePeriodic() {
  resetAll();
  bootNormal();

  const FakeTcpConnectionPtr a = connectObcPeer(7);
  EXPECT_TRUE(obcClient.connected());
  EXPECT_BYTES_PREFIX("accept status", a->tx, kStatusFrame, 3);
  EXPECT_STR("peer ip", obcClient.remoteIP().toString(), "10.0.0.7");

  const FakeTcpConnectionPtr b = connectObcPeer(8);
  EXPECT_TRUE(!a->connected);  // replaced by the newest client
  EXPECT_TRUE(obcClient.connected());
  EXPECT_STR("peer ip", obcClient.remoteIP().toString(), "10.0.0.8");
  EXPECT_BYTES_PREFIX("accept status b", b->tx, kStatusFrame, 3);
  EXPECT_CONTAINS("log", fake::state().serialOutput,
                  "replacing existing client");

  // Periodic DeviceStatus every 30 s while connected.
  advance(STATUS_PERIOD_MS + 1);
  loop();
  EXPECT_EQ(b->tx.size(), 6);  // two 3-byte status frames
}

void testButtonEventsDeliveredToPeer() {
  resetAll();
  bootNormal();
  const FakeTcpConnectionPtr conn = connectObcPeer();
  conn->tx.clear();

  clickButton(btnUp);
  const unsigned char upPress[] = {0x01, 0x01, 0x01};
  const unsigned char upRelease[] = {0x01, 0x01, 0x00};
  EXPECT_BYTES_PREFIX("up press", conn->tx, upPress, 3);
  EXPECT_EQ(conn->tx.size(), 6);
  EXPECT_BYTES_PREFIX("up release", conn->tx.substr(3), upRelease, 3);

  conn->tx.clear();
  clickButton(btnDown);
  const unsigned char downPress[] = {0x01, 0x02, 0x01};
  const unsigned char downRelease[] = {0x01, 0x02, 0x00};
  EXPECT_BYTES_PREFIX("down press", conn->tx, downPress, 3);
  EXPECT_BYTES_PREFIX("down release", conn->tx.substr(3), downRelease, 3);
}

void testShortClickSurvivesTcpBacklog() {
  resetAll();
  bootNormal();
  const FakeTcpConnectionPtr conn = connectObcPeer();

  // 300 pending bytes (75 valid Haptic frames) — more than one loop() eats.
  const unsigned char haptic[] = {0x03, 0x01, 0x02, 0x03};
  for (int i = 0; i < 75; ++i)
    conn->rx.append(reinterpret_cast<const char*>(haptic), sizeof haptic);
  EXPECT_TRUE(obcClient.available() == 300);

  pressPin(PIN_UP);
  loop();
  advance(DEBOUNCE_MS + 1);
  loop();
  // The press is delivered even though >200 bytes are still pending.
  EXPECT_CONTAINS("press despite backlog", conn->tx,
                  std::string("\x01\x01\x01", 3));
  releasePin(PIN_UP);
  loop();
  advance(DEBOUNCE_MS + 1);
  loop();

  while (obcClient.available() > 0) loop();  // drain the rest

  EXPECT_CONTAINS("press", conn->tx, std::string("\x01\x01\x01", 3));
  EXPECT_CONTAINS("release", conn->tx, std::string("\x01\x01\x00", 3));
}

void testFeedbackNonBlockingPatternZeroStopsFlash() {
  resetAll();
  bootNormal();
  const FakeTcpConnectionPtr conn = connectObcPeer();
  EXPECT_EQ(fake::state().pinOutputs[LED_BUILTIN], 1);  // idle: HIGH (LED off)

  // Non-zero pattern: flash. Processing must not advance time or block.
  const unsigned char flash1[] = {0x03, 0x07, 0x32, 0xC8};
  peerSend(conn, flash1, sizeof flash1);
  const unsigned long tBefore = fake::state().nowMs;
  const unsigned long delaysBefore = fake::state().delayCalls;
  loop();
  EXPECT_EQ(fake::state().nowMs, tBefore);            // no time consumed
  EXPECT_EQ(fake::state().delayCalls, delaysBefore);  // non-blocking
  EXPECT_EQ(fake::state().pinOutputs[LED_BUILTIN], 0);  // LED on (LOW)

  // The 10 ms flash window expires by itself.
  advance(11);
  loop();
  EXPECT_EQ(fake::state().pinOutputs[LED_BUILTIN], 1);

  // Another flash, then pattern 0 must stop it immediately.
  const unsigned char flash2[] = {0x03, 0x05, 0x0A, 0x0A};
  peerSend(conn, flash2, sizeof flash2);
  loop();
  EXPECT_EQ(fake::state().pinOutputs[LED_BUILTIN], 0);
  advance(5);  // still inside the flash window
  const unsigned char stop[] = {0x03, 0x00, 0x00, 0x00};
  peerSend(conn, stop, sizeof stop);
  loop();
  loop();  // updateLed() runs on the next loop pass
  EXPECT_EQ(fake::state().pinOutputs[LED_BUILTIN], 1);
  advance(50);
  loop();
  EXPECT_EQ(fake::state().pinOutputs[LED_BUILTIN], 1);  // and stays off
}

void testUnknownRecordKeepsPeerAndResyncs() {
  resetAll();
  bootNormal();
  const FakeTcpConnectionPtr conn = connectObcPeer();

  const unsigned char bad[] = {0x07};  // unknown message type
  peerSend(conn, bad, sizeof bad);
  loop();
  EXPECT_TRUE(conn->connected);  // unknown data must not kill the session
  EXPECT_TRUE(obcClient.connected());
  EXPECT_CONTAINS("log", fake::state().serialOutput, "unknown byte 0x07, resync");

  // After the resync the parser accepts a well-formed record again.
  const unsigned char haptic[] = {0x03, 0x01, 0x00, 0x00};
  peerSend(conn, haptic, sizeof haptic);
  loop();
  loop();  // updateLed() runs on the next loop pass
  EXPECT_TRUE(conn->connected);
  EXPECT_EQ(fake::state().pinOutputs[LED_BUILTIN], 0);  // completed -> flash

  // The firmware keeps serving a fresh peer afterwards.
  const FakeTcpConnectionPtr next = connectObcPeer(9);
  EXPECT_TRUE(obcClient.connected());
  EXPECT_TRUE(next->tx.size() >= 3);
}

void testPartialMessageHeldAcrossLoops() {
  resetAll();
  bootNormal();
  const FakeTcpConnectionPtr conn = connectObcPeer();

  const unsigned char part1[] = {0x03, 0x07};
  const unsigned char part2[] = {0x08, 0x09};
  peerSend(conn, part1, sizeof part1);
  loop();
  EXPECT_TRUE(conn->connected);  // incomplete is not an error
  EXPECT_EQ(fake::state().pinOutputs[LED_BUILTIN], 1);  // nothing executed

  peerSend(conn, part2, sizeof part2);
  loop();
  EXPECT_TRUE(conn->connected);
  EXPECT_EQ(fake::state().pinOutputs[LED_BUILTIN], 0);  // completed -> flash
}

void testClientReplacementResetsParser() {
  resetAll();
  bootNormal();
  const FakeTcpConnectionPtr a = connectObcPeer();

  // A partial AppInfo: 3 of the 5 announced id bytes received.
  const unsigned char partial[] = {0x04, 0x01, 0x05, 'a', 'p', 'p'};
  peerSend(a, partial, sizeof partial);
  loop();
  EXPECT_TRUE(a->connected);  // still parsing, peer kept

  const FakeTcpConnectionPtr b = connectObcPeer(8);  // replaces a
  EXPECT_TRUE(!a->connected);

  // If the parser still held A's partial record, the valid haptic below would
  // be swallowed as id payload (Incomplete, no LED flash). With the reset it
  // parses completely after skipping the unknown probe byte.
  const unsigned char probe[] = {0x05};  // unknown type -> resync, peer kept
  peerSend(b, probe, sizeof probe);
  loop();
  EXPECT_TRUE(b->connected);  // unknown data must not kill the session
  const unsigned char haptic[] = {0x03, 0x01, 0x00, 0x00};
  peerSend(b, haptic, sizeof haptic);
  loop();
  loop();  // updateLed() runs on the next loop pass
  EXPECT_TRUE(b->connected);
  EXPECT_EQ(fake::state().pinOutputs[LED_BUILTIN], 0);  // parsed -> flash
}

}  // namespace

int main() {
  testSetupArmsOtaUpdaterWithStoredPassword();
  testSetupTokenIsBootScoped();
  testEmptyOtaPasswordLocksOutLanSetupAndSave();
  testSetupPageBasicAuth();
  testSaveRequiresValidFormToken();
  testSaveAuthPersistsAndReboots();
  testSaveRejectsInvalidInput();
  testSaveCommitFailureReturns500();
  testBootWithoutConfigOpensPortal();
  testBootBothButtonsHeldForcesSetup();
  testBootWifiUnreachableEntersSetupAfterTimeout();
  testNormalModeRootShowsStatus();
  testWifiLossNoDelaySingleReconnectApTimeout();
  testWifiRecoveryThenSecondLossReconnectsAgain();
  testSetupModePortalServing();
  testButtonHoldEntersSetupWhileOffline();
  testObcAcceptStatusReplacePeriodic();
  testButtonEventsDeliveredToPeer();
  testShortClickSurvivesTcpBacklog();
  testFeedbackNonBlockingPatternZeroStopsFlash();
  testUnknownRecordKeepsPeerAndResyncs();
  testPartialMessageHeldAcrossLoops();
  testClientReplacementResetsParser();

  std::printf("%s: %d checks, %d failures\n",
              g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
