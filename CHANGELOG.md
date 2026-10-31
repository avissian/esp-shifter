# Changelog

## 1.0.1

- Require the `admin` username and the configured OTA password for web updates.
- Authenticate setup/save requests on the normal LAN; without an OTA password,
  use physical-button recovery and the setup access point instead. Protect saves
  with a boot-scoped random form token against cross-site requests.
- Register setup/save routes once and select the root page by the current mode,
  fixing missing saves and the status page shadowing the captive portal.
- Preserve fragmented TCP commands and consume the full OpenBikeControl App Info
  capabilities list; reset parsing on client replacement and reject invalid data.
- Replace blocking feedback flashes with a nonblocking LED acknowledgement.
- Keep buttons responsive during Wi-Fi recovery, attempt reconnection once, and
  open the setup access point after a 20-second recovery timeout.
- Add host regression tests, a pinned-core release build script, and GitHub
  Actions builds that publish firmware and SHA256 checksums for version tags.

## 1.0.0

- Initial ESP8266 OpenBikeControl shifter with two buttons, Wi-Fi provisioning
  and optional web/Arduino OTA updates.
