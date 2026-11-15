// ESP-Shifter — virtual shifting for MyWhoosh over WiFi (OpenBikeControl)
// Board: NodeMCU Amica (ESP8266EX, CP2102)
// Protocol: https://github.com/OpenBikeControl/openbikecontrol-protocol (mDNS + TCP)
//
// v1.0.1
//
// Pins:
//   D1 (GPIO5)  — Shift Up button (to GND)
//   D2 (GPIO4)  — Shift Down button (to GND)
//   Both buttons held 5s while running — enter setup mode
//
// First boot / no credentials / WiFi unreachable:
//   AP "ESP-Shifter-Setup" (password in README), captive portal on
//   http://192.168.4.1 — empty credential fields, save, reboot.
//   Credentials live in EEPROM on the board, never in the binary.
//
// v2 (planned): 8x8 LED matrix on D5/D6/D7 (74HC595 x2 or MAX7219)
// v3 (planned): MH-Z19B CO2 on D0 (PWM), buzzer on D8

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include "ObcProtocol.h"

#define FW_VERSION "1.0.2"
#define OTA_USER "admin"

// ---------- OpenBikeControl ----------
#define OBC_PORT         8080
#define OBC_NAME         "esp-shifter"
#define OBC_NAME_TXT     "ESP-Shifter"
#define OBC_MANUFACTURER "avissian"
#define OBC_MODEL        "ESP-Shifter v1"
#define OBC_SERVICE_UUID "d273f680-d548-419d-b9d1-fa0472345229"

#define BTN_SHIFT_UP     0x01
#define BTN_SHIFT_DOWN   0x02

// ---------- Setup AP (documented in README) ----------
#define AP_SSID "ESP-Shifter-Setup"
#define AP_PASS "esp-shifter-2026"

// ---------- Buttons ----------
#define PIN_UP           D1   // GPIO5
#define PIN_DOWN         D2   // GPIO4
#define DEBOUNCE_MS      20
#define SETUP_HOLD_MS    5000UL

#define STATUS_PERIOD_MS 30000UL
#define WIFI_TIMEOUT_MS  20000UL

// ---------- EEPROM config ----------
#define EEPROM_SIZE      4096
#define CFG_MAGIC        0x45534831UL  // "ESH1"
struct Config {
  uint32_t magic;
  char ssid[33];
  char pass[65];
  char otaPass[33];
  uint32_t crc;
};

// ---------- Globals ----------
WiFiServer obcServer(OBC_PORT);
WiFiClient obcClient;
obc::Parser obcParser;
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;
DNSServer dnsServer;

Config cfg;
bool haveConfig = false;
bool setupMode = false;

unsigned long lastStatusMs = 0;
unsigned long setupHoldStart = 0;
bool setupHoldActive = false;
bool ledFlashing = false;
unsigned long ledFlashStart = 0;
bool wifiRecovering = false;
unsigned long wifiRecoveryStart = 0;
char setupToken[33];

struct Button {
  uint8_t pin;
  uint8_t obcId;
  bool stableState;      // true = pressed (pin LOW)
  bool lastReading;
  unsigned long lastChangeMs;
};
Button btnUp   = { PIN_UP,   BTN_SHIFT_UP,   false, false, 0 };
Button btnDown = { PIN_DOWN, BTN_SHIFT_DOWN, false, false, 0 };

// forward declarations
void sendButtonState(uint8_t obcId, bool pressed);
void sendStatus();
void flashLed();
void updateLed();
void handleButton(struct Button& b);
void handleObcClient();
bool loadConfig();
bool saveConfig(const String& ssid, const String& pass, const String& otaPass);
void clearConfig();
void startSetupMode();
void handleRoot();
void handleSave();
void serveSetupForm();
void setupHttpRoutes();
bool authorizeSetup();
bool checkWiFi();

// ---------- Config storage ----------
uint32_t cfgChecksum(const Config& c) {
  uint32_t sum = 0;
  const uint8_t* p = (const uint8_t*)&c;
  for (size_t i = 0; i < sizeof(Config) - sizeof(uint32_t); i++)
    sum = (sum << 1 | sum >> 31) + p[i];
  return sum;
}

bool loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, cfg);
  if (cfg.magic != CFG_MAGIC || cfg.crc != cfgChecksum(cfg)) {
    memset(&cfg, 0, sizeof(cfg));
    return false;
  }
  cfg.ssid[sizeof(cfg.ssid)-1] = 0;
  cfg.pass[sizeof(cfg.pass)-1] = 0;
  cfg.otaPass[sizeof(cfg.otaPass)-1] = 0;
  return strlen(cfg.ssid) > 0;
}

bool saveConfig(const String& ssid, const String& pass, const String& otaPass) {
  Config next;
  memset(&next, 0, sizeof(next));
  next.magic = CFG_MAGIC;
  strncpy(next.ssid, ssid.c_str(), sizeof(next.ssid)-1);
  strncpy(next.pass, pass.c_str(), sizeof(next.pass)-1);
  strncpy(next.otaPass, otaPass.c_str(), sizeof(next.otaPass)-1);
  next.crc = cfgChecksum(next);
  EEPROM.put(0, next);
  if (!EEPROM.commit()) return false;
  cfg = next;
  return true;
}

void clearConfig() {
  memset(&cfg, 0, sizeof(cfg));
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// ---------- LED / buttons ----------
void flashLed() {
  ledFlashStart = millis();
  ledFlashing = true;
  digitalWrite(LED_BUILTIN, LOW);
}

void updateLed() {
  if (setupMode) {
    digitalWrite(LED_BUILTIN, (millis() / 500UL) % 2 ? HIGH : LOW);
    return;
  }
  if (ledFlashing && millis() - ledFlashStart >= 10) ledFlashing = false;
  digitalWrite(LED_BUILTIN, ledFlashing ? LOW : HIGH);
}

void sendButtonState(uint8_t obcId, bool pressed) {
  if (!obcClient.connected()) return;
  uint8_t msg[3] = { obc::ButtonState, obcId, (uint8_t)(pressed ? 0x01 : 0x00) };
  obcClient.write(msg, 3);
  Serial.printf("[OBC] %s %s\n", obcId == BTN_SHIFT_UP ? "UP" : "DOWN", pressed ? "press" : "release");
  flashLed();
}

void sendStatus() {
  if (!obcClient.connected()) return;
  uint8_t msg[3] = { obc::DeviceStatus, 0xFF, 0x01 };
  obcClient.write(msg, 3);
}

void handleButton(struct Button& b) {
  bool reading = (digitalRead(b.pin) == LOW);
  if (reading != b.lastReading) {
    b.lastChangeMs = millis();
    b.lastReading = reading;
  }
  if ((millis() - b.lastChangeMs) > DEBOUNCE_MS) {
    if (reading != b.stableState) {
      b.stableState = reading;
      if (setupMode) return;  // buttons do nothing in setup mode
      sendButtonState(b.obcId, b.stableState);
    }
  }
}

// ---------- OBC client traffic ----------
void handleObcClient() {
  // Bound work per loop so an active TCP peer cannot starve button polling.
  for (size_t count = 0; count < 64 && obcClient.available(); count++) {
    int byte = obcClient.read();
    if (byte < 0) break;
    obc::ParseResult result = obcParser.feed(static_cast<uint8_t>(byte));
    if (result == obc::ParseResult::Invalid) {
      // MyWhoosh sends an undocumented handshake record (type 0x0E and
      // possibly others) right after connecting. Unknown data must not kill
      // the session: resync the parser and keep the client connected.
      Serial.printf("[OBC] unknown byte 0x%02X, resync\n", byte);
      obcParser.reset();
      continue;
    }
    if (result != obc::ParseResult::Complete) continue;
    const obc::Message& msg = obcParser.message();
    if (msg.type == obc::AppInfo) {
      Serial.printf("[OBC] App connected: %s %s (%u actions)\n",
                    msg.appId, msg.appVersion, msg.buttonCount);
    } else if (msg.type == obc::Haptic) {
      // This board has no vibration motor: acknowledge with a nonblocking LED.
      if (msg.pattern == 0) {
        ledFlashing = false;
      } else {
        flashLed();
      }
    }
  }
}

// ---------- Setup mode (AP + captive portal) ----------
const char SETUP_FORM[] PROGMEM = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP-Shifter Setup</title>
<style>body{font-family:sans-serif;background:#111;color:#eee;max-width:420px;margin:40px auto;padding:0 16px}
h1{font-size:1.3em}input{width:100%;box-sizing:border-box;padding:10px;margin:6px 0 14px;font-size:1em}
button{width:100%;padding:12px;font-size:1em;background:#e74c3c;color:#fff;border:0;border-radius:6px}
small{color:#888}</style></head><body>
<h1>ESP-Shifter v)HTML";

void serveSetupForm() {
  String html = FPSTR(SETUP_FORM);
  html += FW_VERSION;
  html += F("</h1><form method='POST' action='/save'><input type='hidden' name='token' value='");
  html += setupToken;
  html += F("'>WiFi SSID<input name='ssid' maxlength='32' required>"
            "WiFi password<input name='pass' type='password' maxlength='64'>"
            "OTA password<input name='ota' type='password' maxlength='32'>"
            "<small>Empty OTA password disables network flashing.</small>"
            "<button type='submit'>Save &amp; reboot</button></form>"
            "<small>All fields empty on purpose — saved values are never shown.</small></body></html>");
  httpServer.send(200, "text/html", html);
}

void startSetupMode() {
  setupMode = true;
  setupHoldActive = false;
  obcClient.stop();
  obcParser.reset();
  Serial.println("[SETUP] starting AP " AP_SSID);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  dnsServer.start(53, "*", WiFi.softAPIP());

  // Routes are registered once; switching modes must not shadow the root form.
  httpServer.begin();
  Serial.printf("[SETUP] portal: http://%s\n", WiFi.softAPIP().toString().c_str());
}

bool authorizeSetup() {
  if (setupMode) return true;
  if (cfg.otaPass[0] == '\0') {
    httpServer.send(403, "text/plain",
                    "Hold both buttons for 5 seconds and join " AP_SSID " to configure.\n");
    return false;
  }
  if (!httpServer.authenticate(OTA_USER, cfg.otaPass)) {
    httpServer.requestAuthentication();
    return false;
  }
  return true;
}

void handleRoot() {
  if (setupMode) {
    serveSetupForm();
    return;
  }
  char buf[192];
  snprintf(buf, sizeof(buf),
           "ESP-Shifter v%s\nIP: %s\nOBC clients: %d\nSetup: /setup\nUpdate: /update\n",
           FW_VERSION, WiFi.localIP().toString().c_str(),
           obcClient.connected() ? 1 : 0);
  httpServer.send(200, "text/plain", buf);
}

void handleSave() {
  if (!authorizeSetup()) return;
  if (setupToken[0] == '\0' || httpServer.arg("token") != setupToken) {
    httpServer.send(403, "text/plain", "Invalid form token. Reload the setup page.\n");
    return;
  }
  String ssid = httpServer.arg("ssid");
  String pass = httpServer.arg("pass");
  String ota = httpServer.arg("ota");
  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64 || ota.length() > 32) {
    httpServer.send(400, "text/plain", "Invalid SSID or password length.\n");
    return;
  }
  if (!saveConfig(ssid, pass, ota)) {
    httpServer.send(500, "text/plain", "Could not save configuration.\n");
    return;
  }
  Serial.printf("[SETUP] saved SSID '%s', rebooting\n", ssid.c_str());
  httpServer.send(200, "text/html", "<html><body><h2>Saved. Rebooting...</h2></body></html>");
  delay(500);
  ESP.restart();
}

void setupHttpRoutes() {
  // Boot-scoped CSRF protection for authenticated LAN configuration changes.
  snprintf(setupToken, sizeof(setupToken), "%08x%08x%08x%08x",
           ESP.random(), ESP.random(), ESP.random(), ESP.random());
  httpServer.on("/", HTTP_GET, handleRoot);
  httpServer.on("/setup", HTTP_GET, []() {
    if (authorizeSetup()) serveSetupForm();
  });
  httpServer.on("/save", HTTP_POST, handleSave);
  httpServer.on("/generate_204", HTTP_GET, handleRoot);  // Android captive check
  httpServer.on("/fwlink", HTTP_GET, handleRoot);
  httpServer.onNotFound([]() {
    if (setupMode) {
      httpServer.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
      httpServer.send(302, "text/plain", "");
    } else {
      httpServer.send(404, "text/plain", "Not found\n");
    }
  });
}

bool checkWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiRecovering) {
      Serial.println("[WiFi] reconnected");
      wifiRecovering = false;
      MDNS.begin(OBC_NAME);  // Re-probe preserved services on the current IP.
    }
    return true;
  }
  if (!wifiRecovering) {
    wifiRecovering = true;
    wifiRecoveryStart = millis();
    obcClient.stop();
    obcParser.reset();
    Serial.println("[WiFi] lost, reconnecting...");
    WiFi.reconnect();  // Once: repeated disconnect/connect cancels association.
  } else if (millis() - wifiRecoveryStart >= WIFI_TIMEOUT_MS) {
    Serial.println("[WiFi] reconnect timed out, entering setup mode");
    startSetupMode();
  }
  return false;
}

// ---------- Main setup ----------
void setup() {
  Serial.begin(115200);
  Serial.println("\n[BOOT] ESP-Shifter v" FW_VERSION);

  pinMode(PIN_UP, INPUT_PULLUP);
  pinMode(PIN_DOWN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  haveConfig = loadConfig();
  WiFi.persistent(false);
  // ESP8266's hardware RNG needs RF entropy before generating the form token.
  WiFi.mode(WIFI_STA);
  setupHttpRoutes();

  // Both buttons held at boot -> force setup mode
  if (digitalRead(PIN_UP) == LOW && digitalRead(PIN_DOWN) == LOW) {
    Serial.println("[BOOT] both buttons held -> setup mode");
    startSetupMode();
    return;
  }

  if (!haveConfig) {
    startSetupMode();
    return;
  }

  // Try to connect with stored credentials
  Serial.printf("[WiFi] connecting to '%s' ", cfg.ssid);
  WiFi.hostname(OBC_NAME);
  WiFi.begin(cfg.ssid, cfg.pass);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[WiFi] failed, entering setup mode");
    startSetupMode();
    return;
  }
  Serial.printf("\n[WiFi] connected, IP: %s\n", WiFi.localIP().toString().c_str());

  // mDNS + OpenBikeControl service advertisement
  if (MDNS.begin(OBC_NAME)) {
    MDNS.addService("openbikecontrol", "tcp", OBC_PORT);
    MDNS.addServiceTxt("openbikecontrol", "tcp", "version", "1");
    MDNS.addServiceTxt("openbikecontrol", "tcp", "id", WiFi.macAddress());
    MDNS.addServiceTxt("openbikecontrol", "tcp", "name", OBC_NAME_TXT);
    MDNS.addServiceTxt("openbikecontrol", "tcp", "service-uuids", OBC_SERVICE_UUID);
    MDNS.addServiceTxt("openbikecontrol", "tcp", "manufacturer", OBC_MANUFACTURER);
    MDNS.addServiceTxt("openbikecontrol", "tcp", "model", OBC_MODEL);
    Serial.println("[mDNS] openbikecontrol._tcp advertised as " OBC_NAME);
  } else {
    Serial.println("[mDNS] FAILED to start!");
  }

  obcServer.begin();
  obcServer.setNoDelay(true);
  Serial.printf("[OBC] TCP server on port %d\n", OBC_PORT);

  // Web OTA updater at http://esp-shifter.local/update
  if (strlen(cfg.otaPass) > 0) {
    httpUpdater.setup(&httpServer, "/update", OTA_USER, cfg.otaPass);
  }
  httpServer.begin();

  // Arduino OTA (network upload, port 8266)
  if (strlen(cfg.otaPass) > 0) {
    ArduinoOTA.setHostname(OBC_NAME);
    ArduinoOTA.setPassword(cfg.otaPass);
    ArduinoOTA.begin();
  }

  Serial.println("[BOOT] ready");
}

void loop() {
  updateLed();
  handleButton(btnUp);
  handleButton(btnDown);

  // Physical recovery is available even when the station connection is down.
  if (!setupMode) {
    bool bothHeld = btnUp.stableState && btnDown.stableState;
    if (bothHeld) {
      if (!setupHoldActive) {
        setupHoldActive = true;
        setupHoldStart = millis();
      }
      if (millis() - setupHoldStart >= SETUP_HOLD_MS) {
        Serial.println("[BTN] both held 5s -> setup mode");
        startSetupMode();
      }
    } else {
      setupHoldActive = false;
    }
  }

  if (setupMode) {
    dnsServer.processNextRequest();
    httpServer.handleClient();
    return;
  }

  if (!checkWiFi()) return;

  ArduinoOTA.handle();
  httpServer.handleClient();
  MDNS.update();

  // Accept new OBC connections (keep only the latest client)
  if (obcServer.hasClient()) {
    WiFiClient newClient = obcServer.accept();
    if (obcClient.connected()) {
      Serial.println("[OBC] replacing existing client");
      obcClient.stop();
    }
    obcClient = newClient;
    obcParser.reset();
    lastStatusMs = millis();
    Serial.printf("[OBC] client connected from %s\n", obcClient.remoteIP().toString().c_str());
    sendStatus();
  }

  if (obcClient.connected()) {
    handleObcClient();
    if (millis() - lastStatusMs > STATUS_PERIOD_MS) {
      lastStatusMs = millis();
      sendStatus();
    }
  }
}
