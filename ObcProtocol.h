#ifndef ESP_SHIFTER_OBC_PROTOCOL_H
#define ESP_SHIFTER_OBC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace obc {

enum MessageType : uint8_t {
  ButtonState = 0x01,
  DeviceStatus = 0x02,
  Haptic = 0x03,
  AppInfo = 0x04
};

enum class ParseResult { Incomplete, Complete, Invalid };

struct Message {
  uint8_t type;
  uint8_t pattern;
  uint8_t duration;
  uint8_t intensity;
  char appId[33];
  char appVersion[33];
  uint8_t buttonCount;
};

// TCP is a byte stream: retain partial records and consume the complete App Info
// capability list before interpreting another message type. No heap allocation.
class Parser {
 public:
  Parser() : length_(0), message_{} {}

  void reset() {
    length_ = 0;
    message_ = Message{};
  }

  const Message& message() const { return message_; }

  ParseResult feed(uint8_t byte) {
    if (length_ == 0 && byte != Haptic && byte != AppInfo) return invalid();
    if (length_ >= sizeof(buffer_)) return invalid();
    buffer_[length_++] = byte;

    if (buffer_[0] == Haptic) {
      if (length_ < 4) return ParseResult::Incomplete;
      message_ = Message{};
      message_.type = Haptic;
      message_.pattern = buffer_[1];
      message_.duration = buffer_[2];
      message_.intensity = buffer_[3];
      length_ = 0;
      return ParseResult::Complete;
    }

    if (length_ >= 2 && buffer_[1] != 1) return invalid();
    if (length_ < 3) return ParseResult::Incomplete;
    const size_t idLength = buffer_[2];
    if (idLength > 32) return invalid();
    const size_t versionLengthOffset = 3 + idLength;
    if (length_ <= versionLengthOffset) return ParseResult::Incomplete;
    const size_t versionLength = buffer_[versionLengthOffset];
    if (versionLength > 32) return invalid();
    const size_t countOffset = versionLengthOffset + 1 + versionLength;
    if (length_ <= countOffset) return ParseResult::Incomplete;
    const size_t buttonCount = buffer_[countOffset];
    if (length_ < countOffset + 1 + buttonCount) return ParseResult::Incomplete;

    message_ = Message{};
    message_.type = AppInfo;
    memcpy(message_.appId, buffer_ + 3, idLength);
    memcpy(message_.appVersion, buffer_ + versionLengthOffset + 1, versionLength);
    message_.buttonCount = static_cast<uint8_t>(buttonCount);
    length_ = 0;
    return ParseResult::Complete;
  }

 private:
  ParseResult invalid() {
    reset();
    return ParseResult::Invalid;
  }

  // type + version + two string lengths + button count + strings + capabilities
  uint8_t buffer_[5 + 32 + 32 + 255];
  size_t length_;
  Message message_;
};

}  // namespace obc

#endif
