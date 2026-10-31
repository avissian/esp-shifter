# ESP-Shifter

Virtual shifting for MyWhoosh over WiFi. Speaks
[OpenBikeControl](https://github.com/OpenBikeControl/openbikecontrol-protocol)
(mDNS + TCP), which MyWhoosh supports natively — no app, no subscription,
no USB cable after the first flash.

## Hardware

- NodeMCU Amica (ESP8266)
- 2 momentary buttons: D1 = Shift Up, D2 = Shift Down (other leg to GND)

## Build & flash

```
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 --output-dir ./build .
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp8266:esp8266:nodemcuv2 .
```

No credentials in the binary: WiFi SSID/password and the OTA password are
entered through the on-device setup portal and stored in EEPROM.

## First-time setup

On first boot (or if WiFi is unreachable, or both buttons held 5s) the
device opens an access point:

- SSID: `ESP-Shifter-Setup`
- Password: `esp-shifter-2026`
- Portal: `http://192.168.4.1` (opens automatically, captive portal)

Enter WiFi credentials and an OTA password (empty = network flashing
disabled). All form fields are always empty — saved values are never
shown back.

## Use

1. Power the board (any USB charger).
2. In MyWhoosh: device connection screen → handlebar icon (top right) →
   it finds "ESP-Shifter" automatically. Enable MyShift in settings
   (gear step size 32 is a good start).
3. Shift with the buttons.

## OTA updates

- Web: `http://esp-shifter.local/update` (the OTA password from setup)
- Arduino OTA: `arduino-cli upload -p esp-shifter.local --fqbn esp8266:esp8266:nodemcuv2 .`

## Roadmap

- v2: 8x8 LED matrix, two cascaded 74HC595 on D5/D6/D7 —
  CO2 ppm always on, gear overlay on shift
- v3: MH-Z19B CO2 sensor (D0, PWM) + buzzer on D8, alert at >1000 ppm

Note: a prebuilt MAX7219 matrix module (3 pins, no soldering zoo)
replaces both 595s if buying one turns out easier than finding a second
shift register in the parts box.

## Credits

100% AI-made by GLM 5.3 (z.ai) — code, protocol integration, build setup.
The idea, hardware archaeology and stubbornness are human.
