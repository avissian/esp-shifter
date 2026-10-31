// Minimal host fake of the ESP8266 EEPROM library: a flat byte container
// with get/put/commit plus call counters. Bytes written via put() are
// immediately visible (like the real RAM image) and survive until the test
// resets the world. commit() can be forced to fail to test error paths.
#ifndef TESTS_FAKES_EEPROM_H
#define TESTS_FAKES_EEPROM_H

#include <cstddef>
#include <cstring>

#include "Arduino.h"

class EEPROMClass {
 public:
  enum { kSize = 4096 };

  EEPROMClass()
      : begun(false), commitResult(true), begins(0), gets(0), puts(0),
        commits(0), size_(0) {
    std::memset(data_, 0, sizeof data_);
  }

  void begin(std::size_t size) {
    begun = true;
    ++begins;
    size_ = (size <= static_cast<std::size_t>(kSize)) ? size
                                                      : static_cast<std::size_t>(kSize);
  }

  template <typename T>
  T& get(int address, T& value) {
    ++gets;
    readBytes(static_cast<std::size_t>(address), &value, sizeof(T));
    return value;
  }

  template <typename T>
  const T& put(int address, const T& value) {
    ++puts;
    writeBytes(static_cast<std::size_t>(address), &value, sizeof(T));
    return value;
  }

  bool commit() {
    ++commits;
    return commitResult;
  }

  void reset() {
    std::memset(data_, 0, sizeof data_);
    size_ = 0;
    begun = false;
    commitResult = true;
    begins = gets = puts = commits = 0;
  }

  // Test access to the raw image (for preloading and snapshot comparisons).
  unsigned char* rawData() { return data_; }
  const unsigned char* rawData() const { return data_; }

  bool begun;
  bool commitResult;  // test can force commit() to fail
  int begins;
  int gets;
  int puts;
  int commits;

 private:
  void readBytes(std::size_t address, void* out, std::size_t count) const {
    if (address >= static_cast<std::size_t>(kSize)) {
      std::memset(out, 0, count);
      return;
    }
    if (count > static_cast<std::size_t>(kSize) - address)
      count = static_cast<std::size_t>(kSize) - address;
    std::memcpy(out, data_ + address, count);
  }

  void writeBytes(std::size_t address, const void* in, std::size_t count) {
    if (address >= static_cast<std::size_t>(kSize)) return;
    if (count > static_cast<std::size_t>(kSize) - address)
      count = static_cast<std::size_t>(kSize) - address;
    std::memcpy(data_ + address, in, count);
  }

  std::size_t size_;
  unsigned char data_[kSize];
};

EEPROMClass EEPROM;

#endif  // TESTS_FAKES_EEPROM_H
