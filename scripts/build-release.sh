#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(sed -n 's/^#define FW_VERSION "\([^"]*\)"/\1/p' "$root/main.cpp")"
fqbn="esp8266:esp8266:nodemcuv2"
core_version="3.1.2"
output="${1:-$root/build/release/v$version}"
mkdir -p "$output"
output="$(cd "$output" && pwd)"

cli=("${ARDUINO_CLI:-arduino-cli}")
if [[ -n "${ARDUINO_CLI_CONFIG:-}" ]]; then
  cli+=(--config-file "$ARDUINO_CLI_CONFIG")
fi
installed_core="$("${cli[@]}" core list | awk '$1 == "esp8266:esp8266" {print $2}')"
if [[ "$installed_core" != "$core_version" ]]; then
  echo "ESP8266 core $core_version is required; found: ${installed_core:-none}" >&2
  exit 1
fi
if [[ -z "$version" ]]; then
  echo "Could not read FW_VERSION" >&2
  exit 1
fi

# Arduino requires the sketch folder and its .ino file to have the same name.
# Staging also permits a checkout whose directory is not named esp-shifter.
sketch="$root/build/sketch/esp-shifter"
compiled="$root/build/compiled"
mkdir -p "$sketch" "$compiled"
cp "$root/esp-shifter.ino" "$root/main.cpp" "$root/ObcProtocol.h" "$sketch/"
"${cli[@]}" compile --fqbn "$fqbn" --warnings all \
  --build-path "$root/build/obj" --output-dir "$compiled" "$sketch"

firmware="esp-shifter-v$version-nodemcuv2.bin"
cp "$compiled/esp-shifter.ino.bin" "$output/$firmware"
awk -v heading="## $version" '
  $0 == heading {printing = 1; next}
  /^## / && printing {exit}
  printing {print}
' "$root/CHANGELOG.md" > "$output/RELEASE_NOTES.md"
test -s "$output/RELEASE_NOTES.md"

{
  printf 'version=%s\nboard=%s\nesp8266_core=%s\n' "$version" "$fqbn" "$core_version"
  printf 'source_commit=%s\n' "$(git -C "$root" rev-parse HEAD)"
  if [[ -n "$(git -C "$root" status --porcelain)" ]]; then
    printf 'source_tree=dirty\n'
  else
    printf 'source_tree=clean\n'
  fi
  "${cli[@]}" version
} > "$output/BUILD_INFO.txt"
(
  cd "$output"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$firmware" BUILD_INFO.txt RELEASE_NOTES.md > SHA256SUMS
  else
    shasum -a 256 "$firmware" BUILD_INFO.txt RELEASE_NOTES.md > SHA256SUMS
  fi
)
printf 'Release files: %s\n' "$output"
