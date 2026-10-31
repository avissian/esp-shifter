# ESP-Shifter

Virtual shifting for MyWhoosh over WiFi. Speaks
[OpenBikeControl](https://github.com/OpenBikeControl/openbikecontrol-protocol)
(mDNS + TCP), which MyWhoosh supports natively — no app, no subscription,
no USB cable after the first flash.

## Hardware

- NodeMCU Amica (ESP8266)
- 2 momentary buttons: D1 = Shift Up, D2 = Shift Down (other leg to GND)

ESP8266 requires **2.4 GHz WiFi**. Android may use another band if both devices
remain on the same local network.

## Build, test & flash

Use Arduino CLI **1.5.1**, ESP8266 core **3.1.2**, and a C++11 host compiler for
the regression tests. Linux, macOS and Windows can build the firmware; the helper
scripts require Bash (including the macOS system Bash 3.2).

```bash
arduino-cli core update-index --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli core install esp8266:esp8266@3.1.2 --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
bash scripts/test.sh
bash scripts/build-release.sh
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp8266:esp8266:nodemcuv2 \
  --input-file build/release/v1.0.1/esp-shifter-v1.0.1-nodemcuv2.bin
```

Use your board's serial port instead of `/dev/ttyUSB0` (for example,
`/dev/cu.SLAB_USBtoUART` on macOS). The build script stages an Arduino-compatible
sketch folder, so the checkout itself may have any directory name. Release files
are written to `build/release/v1.0.1/`. For an isolated local CLI installation,
set `ARDUINO_CLI` to its executable and `ARDUINO_CLI_CONFIG` to its config file.

No credentials in the binary: WiFi SSID/password and the OTA password are
entered through the on-device setup portal and stored in EEPROM.

## First-time setup

On first boot, after a 20-second WiFi connection/recovery timeout, or when both
buttons are held for 5 seconds, the device opens an access point. The button
combination also works while WiFi is disconnected. Holding both buttons during
power-on enters setup immediately.

- SSID: `ESP-Shifter-Setup`
- Password: `esp-shifter-2026`
- Portal: `http://192.168.4.1` (opens automatically, captive portal)

Enter WiFi credentials and an OTA password (empty = network flashing
disabled). All credential fields are always empty — saved values are never
shown back. The setup form includes a boot-scoped random token to protect saves
against cross-site requests. Reopen the form after rebooting the board.

The setup access point uses the shared password above: provision it in a trusted
place. In normal WiFi mode, `/setup` and saving settings require username
`admin` and the configured OTA password. If OTA is disabled, configure through
the physical-button setup access point instead. Entering empty WiFi/OTA password
fields saves empty passwords; it does not preserve the old ones.

## Use

1. Power the board (any USB charger).
2. In a current MyWhoosh version with OpenBikeControl support (5.7.0+): device
   connection screen → OpenBikeControl/handlebar icon (top right) → look for
   "ESP-Shifter". Enable MyShift in settings. The board and MyWhoosh must be on
   the same LAN, with multicast discovery and TCP port 8080 allowed.
3. Shift with the buttons.

## OTA updates

- Web: `http://esp-shifter.local/update`, username **`admin`**, password from
  setup. Upload the release `.bin` file. If `.local` is unavailable, use the
  board's IP address.
- Arduino OTA: select the board's network port in an OTA-capable IDE and supply
  the configured OTA password (port 8266).
- With an empty OTA password, both update mechanisms are disabled.
- Version 1.0.1 keeps the 1.0.0 EEPROM format; an update without erasing all flash
  retains the existing credentials.

The web interface uses unencrypted HTTP, including authentication. Use it only
on a trusted local network; do not forward its ports to the Internet.

## GitHub releases

Pushing a version tag such as `v1.0.1` runs `.github/workflows/release.yml`:
host regression tests → pinned-core firmware build → artifact upload → GitHub
Release publication. The tag must match `FW_VERSION` in `main.cpp`.

The release contains the NodeMCU `.bin`, `SHA256SUMS`, `BUILD_INFO.txt` (source
commit and tool versions), and release notes. A manual workflow run builds a
downloadable Actions artifact but does not publish a release. No WiFi credentials
or OTA secrets are needed by GitHub Actions.

Host tests cover TCP fragmentation/capabilities and firmware control flow using
SDK fakes; they are not hardware tests. Successful compilation does not prove
discovery, shifting or OTA with a physical ESP8266 and Android/MyWhoosh — verify
those before relying on the controller during a ride.

## Roadmap

- v2: 8x8 LED matrix, two cascaded 74HC595 on D5/D6/D7 —
  CO2 ppm always on, gear overlay on shift
- v3: MH-Z19B CO2 sensor (D0, PWM) + buzzer on D8, alert at >1000 ppm

Note: a prebuilt MAX7219 matrix module (3 pins, no soldering zoo)
replaces both 595s if buying one turns out easier than finding a second
shift register in the parts box.

## Credits

Initial implementation by GLM 5.3 (z.ai); v1.0.1 hardening and release automation
by GPT-6.1 Sol, with independent Z.AI test/review assistance.
The idea, hardware archaeology and stubbornness are human.
