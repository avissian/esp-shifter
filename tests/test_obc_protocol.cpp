// Deterministic host-side unit tests for ObcProtocol.h (namespace obc).
//
// Protocol under test (byte stream; partial records are retained by the
// parser, so messages may arrive split at any byte boundary):
//   Haptic:  [3, pattern, duration, intensity]
//   AppInfo: [4, 1, idLen, appId..., verLen, appVersion..., buttonCount,
//             buttonIds...]   with idLen/verLen <= 32, buttonCount <= 255
//
// Covered scenarios:
//   - whole-message and byte-by-byte feeding across every split point
//   - several commands glued into one stream
//   - capability bytes equal to Haptic(3)/AppInfo(4) are consumed as data
//     and never produce a phantom message
//   - incomplete Haptic is not executed
//   - empty strings and buttonCount == 0
//   - maximum sizes: idLen 32 + verLen 32 + buttonCount 255
//   - invalid type, version and string lengths
//   - reset() mid-message and between clients
//
// Build/run: scripts/test.sh (${CXX:-c++} -std=c++11 -Wall -Wextra -Werror
// -pedantic). No ESP SDK and no third-party libraries are required.

#include <cstdio>
#include <cstring>

#include "../ObcProtocol.h"

namespace {

const size_t kMaxAppInfoSize = 324;  // 1 + 1 + 1 + 32 + 1 + 32 + 1 + 255.

int g_checks = 0;
int g_failures = 0;

const char* resultName(obc::ParseResult result) {
  switch (result) {
    case obc::ParseResult::Incomplete: return "Incomplete";
    case obc::ParseResult::Complete: return "Complete";
    case obc::ParseResult::Invalid: return "Invalid";
  }
  return "unknown";
}

void expectResult(obc::ParseResult actual, obc::ParseResult expected,
                  int line) {
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    std::fprintf(stderr, "line %d: expected %s, got %s\n", line,
                 resultName(expected), resultName(actual));
  }
}

#define EXPECT_RESULT(actual, expected) \
  expectResult((actual), (expected), __LINE__)

void expectTrue(bool ok, int line, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "line %d: failed: %s\n", line, what);
  }
}

#define EXPECT_TRUE(expr) expectTrue((expr), __LINE__, #expr)

void expectStr(const char* label, const char* actual, const char* expected,
               int line) {
  ++g_checks;
  if (std::strcmp(actual, expected) != 0) {
    ++g_failures;
    std::fprintf(stderr, "line %d: %s: expected \"%s\", got \"%s\"\n", line,
                 label, expected, actual);
  }
}

#define EXPECT_STR(label, actual, expected) \
  expectStr((label), (actual), (expected), __LINE__)

// Feeds every byte and returns the result of the last feed() call.
obc::ParseResult feedAll(obc::Parser& parser, const uint8_t* bytes,
                         size_t count) {
  obc::ParseResult result = obc::ParseResult::Incomplete;
  for (size_t i = 0; i < count; ++i) result = parser.feed(bytes[i]);
  return result;
}

// Tallies results while feeding a stream byte-by-byte.
struct Tally {
  int incomplete;
  int complete;
  int invalid;
};

Tally feedStream(obc::Parser& parser, const uint8_t* bytes, size_t count) {
  Tally tally = {0, 0, 0};
  for (size_t i = 0; i < count; ++i) {
    switch (parser.feed(bytes[i])) {
      case obc::ParseResult::Incomplete: ++tally.incomplete; break;
      case obc::ParseResult::Complete: ++tally.complete; break;
      case obc::ParseResult::Invalid: ++tally.invalid; break;
    }
  }
  return tally;
}

// Serializes a valid AppInfo message; returns its length in bytes.
size_t buildAppInfo(uint8_t* out, const char* appId, const char* appVersion,
                    const uint8_t* buttonIds, size_t buttonCount) {
  const size_t idLen = std::strlen(appId);
  const size_t versionLen = std::strlen(appVersion);
  size_t n = 0;
  out[n++] = obc::AppInfo;
  out[n++] = 1;  // protocol version
  out[n++] = static_cast<uint8_t>(idLen);
  if (idLen != 0) std::memcpy(out + n, appId, idLen);
  n += idLen;
  out[n++] = static_cast<uint8_t>(versionLen);
  if (versionLen != 0) std::memcpy(out + n, appVersion, versionLen);
  n += versionLen;
  out[n++] = static_cast<uint8_t>(buttonCount);
  if (buttonCount != 0) std::memcpy(out + n, buttonIds, buttonCount);
  n += buttonCount;
  return n;
}

void expectHaptic(const obc::Message& m, uint8_t pattern, uint8_t duration,
                  uint8_t intensity) {
  EXPECT_TRUE(m.type == static_cast<uint8_t>(obc::Haptic));
  EXPECT_TRUE(m.pattern == pattern);
  EXPECT_TRUE(m.duration == duration);
  EXPECT_TRUE(m.intensity == intensity);
}

void expectAppInfo(const obc::Message& m, const char* appId,
                   const char* appVersion, uint8_t buttonCount) {
  EXPECT_TRUE(m.type == static_cast<uint8_t>(obc::AppInfo));
  EXPECT_STR("appId", m.appId, appId);
  EXPECT_STR("appVersion", m.appVersion, appVersion);
  EXPECT_TRUE(m.buttonCount == buttonCount);
}

// Feeds the message byte-by-byte: every prefix must report Incomplete and the
// final byte Complete. Because feed() takes one byte at a time and keeps
// state, this exercises the message split at every possible byte boundary.
void expectIncompleteUntilLastByte(const uint8_t* msg, size_t count) {
  obc::Parser parser;
  for (size_t i = 0; i < count; ++i) {
    const obc::ParseResult expected =
        (i + 1 == count) ? obc::ParseResult::Complete
                         : obc::ParseResult::Incomplete;
    expectResult(parser.feed(msg[i]), expected, __LINE__);
  }
}

void expectMessageIsCleared(const obc::Message& m) {
  EXPECT_TRUE(m.type == 0);
  EXPECT_TRUE(m.pattern == 0);
  EXPECT_TRUE(m.duration == 0);
  EXPECT_TRUE(m.intensity == 0);
  EXPECT_TRUE(m.buttonCount == 0);
  EXPECT_STR("appId", m.appId, "");
  EXPECT_STR("appVersion", m.appVersion, "");
}

// ---------------------------------------------------------------- Haptic ---

void testHapticWholeMessage() {
  const uint8_t msg[] = {obc::Haptic, 7, 200, 42};
  obc::Parser parser;
  EXPECT_RESULT(feedAll(parser, msg, sizeof msg), obc::ParseResult::Complete);
  expectHaptic(parser.message(), 7, 200, 42);
}

void testHapticBoundaryValues() {
  const uint8_t zeros[] = {obc::Haptic, 0, 0, 0};
  obc::Parser parser;
  EXPECT_RESULT(feedAll(parser, zeros, sizeof zeros),
                obc::ParseResult::Complete);
  expectHaptic(parser.message(), 0, 0, 0);

  const uint8_t maxes[] = {obc::Haptic, 255, 255, 255};
  EXPECT_RESULT(feedAll(parser, maxes, sizeof maxes),
                obc::ParseResult::Complete);
  expectHaptic(parser.message(), 255, 255, 255);
}

void testHapticByteByByteAllSplitPoints() {
  const uint8_t msg[] = {obc::Haptic, 9, 8, 3};
  obc::Parser parser;
  EXPECT_RESULT(parser.feed(msg[0]), obc::ParseResult::Incomplete);
  EXPECT_RESULT(parser.feed(msg[1]), obc::ParseResult::Incomplete);
  EXPECT_TRUE(parser.message().type == 0);  // partial haptic is not executed
  EXPECT_RESULT(parser.feed(msg[2]), obc::ParseResult::Incomplete);
  expectMessageIsCleared(parser.message());
  EXPECT_RESULT(parser.feed(msg[3]), obc::ParseResult::Complete);
  expectHaptic(parser.message(), 9, 8, 3);

  // Payload bytes that collide with message type values stay payload.
  const uint8_t tricky[] = {obc::Haptic, 4, 1, 4};
  expectIncompleteUntilLastByte(tricky, sizeof tricky);
}

// --------------------------------------------------------------- AppInfo ---

void testAppInfoWholeMessage() {
  const uint8_t caps[] = {1, 2, 3, 4, 5};
  uint8_t msg[kMaxAppInfoSize];
  const size_t n =
      buildAppInfo(msg, "com.example.shifter", "1.2.3", caps, sizeof caps);
  obc::Parser parser;
  EXPECT_RESULT(feedAll(parser, msg, n), obc::ParseResult::Complete);
  expectAppInfo(parser.message(), "com.example.shifter", "1.2.3", 5);
  // Haptic-only fields must not keep stale values from earlier messages.
  EXPECT_TRUE(parser.message().pattern == 0);
  EXPECT_TRUE(parser.message().duration == 0);
  EXPECT_TRUE(parser.message().intensity == 0);
}

void testAppInfoByteByByteAllSplitPoints() {
  const uint8_t caps[] = {0, 3, 4, 255, 3, 4};
  uint8_t msg[kMaxAppInfoSize];
  const size_t n = buildAppInfo(msg, "app.id", "v1", caps, sizeof caps);
  expectIncompleteUntilLastByte(msg, n);

  obc::Parser parser;
  EXPECT_RESULT(feedAll(parser, msg, n), obc::ParseResult::Complete);
  expectAppInfo(parser.message(), "app.id", "v1", 6);
}

void testEmptyStringsAndZeroCapabilities() {
  uint8_t msg[kMaxAppInfoSize];
  const size_t n = buildAppInfo(msg, "", "", 0, 0);
  EXPECT_TRUE(n == 5);
  expectIncompleteUntilLastByte(msg, n);

  obc::Parser parser;
  EXPECT_RESULT(feedAll(parser, msg, n), obc::ParseResult::Complete);
  expectAppInfo(parser.message(), "", "", 0);
}

void testBoundaryStringLengths() {
  char id[33];
  char version[33];
  for (int i = 0; i < 32; ++i) {
    id[i] = static_cast<char>('a' + (i % 26));
    version[i] = static_cast<char>('0' + (i % 10));
  }
  id[32] = '\0';
  version[32] = '\0';

  uint8_t msg[kMaxAppInfoSize];
  // Full 32-byte id, empty version.
  size_t n = buildAppInfo(msg, id, "", 0, 0);
  EXPECT_TRUE(n == 37);
  expectIncompleteUntilLastByte(msg, n);
  obc::Parser parser;
  EXPECT_RESULT(feedAll(parser, msg, n), obc::ParseResult::Complete);
  expectAppInfo(parser.message(), id, "", 0);

  // Empty id, full 32-byte version.
  n = buildAppInfo(msg, "", version, 0, 0);
  EXPECT_TRUE(n == 37);
  expectIncompleteUntilLastByte(msg, n);
  EXPECT_RESULT(feedAll(parser, msg, n), obc::ParseResult::Complete);
  expectAppInfo(parser.message(), "", version, 0);
}

void testMaxLengths32Plus32Plus255() {
  char id[33];
  char version[33];
  for (int i = 0; i < 32; ++i) {
    id[i] = static_cast<char>('A' + (i % 26));
    version[i] = static_cast<char>('0' + (i % 10));
  }
  id[32] = '\0';
  version[32] = '\0';

  // Capability bytes deliberately consist only of Haptic(3)/AppInfo(4).
  uint8_t caps[255];
  for (size_t i = 0; i < sizeof caps; ++i) {
    caps[i] = static_cast<uint8_t>((i % 2) != 0 ? obc::Haptic : obc::AppInfo);
  }

  uint8_t msg[kMaxAppInfoSize];
  const size_t n = buildAppInfo(msg, id, version, caps, sizeof caps);
  EXPECT_TRUE(n == kMaxAppInfoSize);

  obc::Parser parser;
  EXPECT_RESULT(feedAll(parser, msg, n), obc::ParseResult::Complete);
  expectAppInfo(parser.message(), id, version, 255);
  EXPECT_TRUE(parser.message().appId[32] == '\0');
  EXPECT_TRUE(parser.message().appVersion[32] == '\0');

  // Same maximum message split at every byte boundary.
  expectIncompleteUntilLastByte(msg, n);

  // The stream stays aligned afterwards: a following Haptic parses normally.
  const uint8_t haptic[] = {obc::Haptic, 1, 2, 3};
  EXPECT_RESULT(feedAll(parser, haptic, sizeof haptic),
                obc::ParseResult::Complete);
  expectHaptic(parser.message(), 1, 2, 3);
}

// --------------------------------------------------------- stream framing ---

void testGluedHaptics() {
  const uint8_t stream[] = {obc::Haptic, 1, 2, 3, obc::Haptic, 4, 5, 6};
  obc::Parser parser;
  const Tally tally = feedStream(parser, stream, sizeof stream);
  EXPECT_TRUE(tally.complete == 2);
  EXPECT_TRUE(tally.incomplete == 6);
  EXPECT_TRUE(tally.invalid == 0);
  expectHaptic(parser.message(), 4, 5, 6);
}

void testGluedMixedStream() {
  const uint8_t capsA[] = {3, 4};
  const uint8_t capsB[] = {255};
  uint8_t stream[2 * kMaxAppInfoSize + 8];
  size_t n = 0;
  n += buildAppInfo(stream + n, "first.app", "0.1", capsA, sizeof capsA);
  stream[n++] = obc::Haptic;
  stream[n++] = 10;
  stream[n++] = 20;
  stream[n++] = 30;
  n += buildAppInfo(stream + n, "second.app", "2.0", capsB, sizeof capsB);
  stream[n++] = obc::Haptic;
  stream[n++] = 40;
  stream[n++] = 50;
  stream[n++] = 60;

  obc::Parser parser;
  const Tally tally = feedStream(parser, stream, n);
  EXPECT_TRUE(tally.complete == 4);
  EXPECT_TRUE(tally.invalid == 0);
  expectHaptic(parser.message(), 40, 50, 60);
}

void testCapabilitiesContainingTypeBytes() {
  // Capability bytes equal to Haptic(3)/AppInfo(4) must be consumed as data.
  const uint8_t caps[] = {3, 4, 3, 3, 4, 4, 3, 0, 255};
  uint8_t msg[kMaxAppInfoSize];
  const size_t n = buildAppInfo(msg, "caps.app", "9.9", caps, sizeof caps);

  obc::Parser parser;
  for (size_t i = 0; i + 1 < n; ++i) {
    expectResult(parser.feed(msg[i]), obc::ParseResult::Incomplete, __LINE__);
  }
  EXPECT_RESULT(parser.feed(msg[n - 1]), obc::ParseResult::Complete);
  expectAppInfo(parser.message(), "caps.app", "9.9", 9);

  // Had the trailing capability bytes been left unconsumed, the following
  // bytes would have paired with them into a phantom Haptic/AppInfo.
  const uint8_t tail[] = {5, 6, 7, 8};
  for (size_t i = 0; i < sizeof tail; ++i) {
    EXPECT_RESULT(parser.feed(tail[i]), obc::ParseResult::Invalid);
  }

  // The parser is still usable after the invalid tail.
  const uint8_t haptic[] = {obc::Haptic, 1, 2, 3};
  EXPECT_RESULT(feedAll(parser, haptic, sizeof haptic),
                obc::ParseResult::Complete);
  expectHaptic(parser.message(), 1, 2, 3);
}

void testSingleCapabilityEqualToTypeByte() {
  uint8_t msg[kMaxAppInfoSize];

  // The last capability byte is 3 (Haptic): it must end the AppInfo, not
  // start a Haptic.
  const uint8_t caps3[] = {3};
  size_t n = buildAppInfo(msg, "a", "b", caps3, 1);
  obc::Parser p3;
  EXPECT_RESULT(feedAll(p3, msg, n), obc::ParseResult::Complete);
  expectAppInfo(p3.message(), "a", "b", 1);
  // 5 is an unknown type: proves no leftover 3 is pending as a Haptic prefix.
  EXPECT_RESULT(p3.feed(5), obc::ParseResult::Invalid);
  EXPECT_RESULT(p3.feed(6), obc::ParseResult::Invalid);
  EXPECT_RESULT(p3.feed(7), obc::ParseResult::Invalid);

  // The last capability byte is 4 (AppInfo): same idea, AppInfo-shaped tail.
  const uint8_t caps4[] = {4};
  n = buildAppInfo(msg, "a", "b", caps4, 1);
  obc::Parser p4;
  EXPECT_RESULT(feedAll(p4, msg, n), obc::ParseResult::Complete);
  expectAppInfo(p4.message(), "a", "b", 1);
  // If the 4 were left pending, [4,1,0,0,0] would complete a phantom AppInfo.
  const uint8_t phantom[] = {1, 0, 0, 0, 0};
  const Tally tally = feedStream(p4, phantom, sizeof phantom);
  EXPECT_TRUE(tally.complete == 0);
  EXPECT_TRUE(tally.invalid == 5);
}

// ------------------------------------------------- partial message safety ---

void testIncompleteHapticNotExecuted() {
  obc::Parser parser;
  EXPECT_RESULT(parser.feed(obc::Haptic), obc::ParseResult::Incomplete);
  EXPECT_RESULT(parser.feed(1), obc::ParseResult::Incomplete);
  EXPECT_RESULT(parser.feed(2), obc::ParseResult::Incomplete);
  expectMessageIsCleared(parser.message());
  EXPECT_RESULT(parser.feed(3), obc::ParseResult::Complete);
  expectHaptic(parser.message(), 1, 2, 3);

  // A partial next Haptic must not execute: no Complete, and the previously
  // completed message stays untouched.
  EXPECT_RESULT(parser.feed(obc::Haptic), obc::ParseResult::Incomplete);
  EXPECT_RESULT(parser.feed(9), obc::ParseResult::Incomplete);
  EXPECT_RESULT(parser.feed(9), obc::ParseResult::Incomplete);
  expectHaptic(parser.message(), 1, 2, 3);
  EXPECT_RESULT(parser.feed(9), obc::ParseResult::Complete);
  expectHaptic(parser.message(), 9, 9, 9);
}

void testIncompleteAppInfoNotExecuted() {
  obc::Parser parser;
  EXPECT_RESULT(parser.feed(obc::AppInfo), obc::ParseResult::Incomplete);
  EXPECT_RESULT(parser.feed(1), obc::ParseResult::Incomplete);
  EXPECT_RESULT(parser.feed(3), obc::ParseResult::Incomplete);
  expectMessageIsCleared(parser.message());
}

// ------------------------------------------------------------ invalid input ---

void testInvalidTypeRejected() {
  const uint8_t types[] = {0, 1, 2, 5, 0x7f, 0xff};
  for (size_t i = 0; i < sizeof types; ++i) {
    obc::Parser parser;
    EXPECT_RESULT(parser.feed(types[i]), obc::ParseResult::Invalid);
  }
  // ButtonState/DeviceStatus are device-to-app types, not accepted inbound.
  obc::Parser buttonState;
  EXPECT_RESULT(buttonState.feed(static_cast<uint8_t>(obc::ButtonState)),
                obc::ParseResult::Invalid);
  obc::Parser deviceStatus;
  EXPECT_RESULT(deviceStatus.feed(static_cast<uint8_t>(obc::DeviceStatus)),
                obc::ParseResult::Invalid);
}

void testInvalidVersionRejected() {
  obc::Parser zero;
  EXPECT_RESULT(zero.feed(obc::AppInfo), obc::ParseResult::Incomplete);
  EXPECT_RESULT(zero.feed(0), obc::ParseResult::Invalid);

  obc::Parser two;
  EXPECT_RESULT(two.feed(obc::AppInfo), obc::ParseResult::Incomplete);
  EXPECT_RESULT(two.feed(2), obc::ParseResult::Invalid);

  obc::Parser big;
  EXPECT_RESULT(big.feed(obc::AppInfo), obc::ParseResult::Incomplete);
  EXPECT_RESULT(big.feed(255), obc::ParseResult::Invalid);
}

void testInvalidStringLengthsRejected() {
  {  // idLen > 32 is rejected on the third byte.
    obc::Parser parser;
    EXPECT_RESULT(parser.feed(obc::AppInfo), obc::ParseResult::Incomplete);
    EXPECT_RESULT(parser.feed(1), obc::ParseResult::Incomplete);
    EXPECT_RESULT(parser.feed(33), obc::ParseResult::Invalid);
  }
  {  // idLen = 255, same position.
    obc::Parser parser;
    EXPECT_RESULT(parser.feed(obc::AppInfo), obc::ParseResult::Incomplete);
    EXPECT_RESULT(parser.feed(1), obc::ParseResult::Incomplete);
    EXPECT_RESULT(parser.feed(255), obc::ParseResult::Invalid);
  }
  {  // versionLen > 32 with an empty id is rejected on the fourth byte.
    obc::Parser parser;
    EXPECT_RESULT(parser.feed(obc::AppInfo), obc::ParseResult::Incomplete);
    EXPECT_RESULT(parser.feed(1), obc::ParseResult::Incomplete);
    EXPECT_RESULT(parser.feed(0), obc::ParseResult::Incomplete);
    EXPECT_RESULT(parser.feed(33), obc::ParseResult::Invalid);
  }
  {  // versionLen = 255, same position.
    obc::Parser parser;
    EXPECT_RESULT(parser.feed(obc::AppInfo), obc::ParseResult::Incomplete);
    EXPECT_RESULT(parser.feed(1), obc::ParseResult::Incomplete);
    EXPECT_RESULT(parser.feed(0), obc::ParseResult::Incomplete);
    EXPECT_RESULT(parser.feed(255), obc::ParseResult::Invalid);
  }
  {  // versionLen > 32 after a full 32-byte id: rejected exactly on the
     // versionLen byte.
    uint8_t msg[kMaxAppInfoSize];
    size_t n = 0;
    msg[n++] = obc::AppInfo;
    msg[n++] = 1;
    msg[n++] = 32;
    for (int i = 0; i < 32; ++i) {
      msg[n++] = static_cast<uint8_t>('A' + (i % 26));
    }
    msg[n++] = 33;
    obc::Parser parser;
    for (size_t i = 0; i + 1 < n; ++i) {
      expectResult(parser.feed(msg[i]), obc::ParseResult::Incomplete,
                   __LINE__);
    }
    EXPECT_RESULT(parser.feed(msg[n - 1]), obc::ParseResult::Invalid);
  }
}

void testParserUsableAfterInvalid() {
  obc::Parser parser;
  EXPECT_RESULT(parser.feed(0x55), obc::ParseResult::Invalid);
  expectMessageIsCleared(parser.message());

  const uint8_t haptic[] = {obc::Haptic, 1, 2, 3};
  EXPECT_RESULT(feedAll(parser, haptic, sizeof haptic),
                obc::ParseResult::Complete);
  expectHaptic(parser.message(), 1, 2, 3);

  // Invalid struck in the middle of an AppInfo; parsing recovers afterwards.
  EXPECT_RESULT(parser.feed(obc::AppInfo), obc::ParseResult::Incomplete);
  EXPECT_RESULT(parser.feed(1), obc::ParseResult::Incomplete);
  EXPECT_RESULT(parser.feed(33), obc::ParseResult::Invalid);

  const uint8_t caps[] = {1};
  uint8_t msg[kMaxAppInfoSize];
  const size_t n = buildAppInfo(msg, "ok.app", "2.2", caps, sizeof caps);
  EXPECT_RESULT(feedAll(parser, msg, n), obc::ParseResult::Complete);
  expectAppInfo(parser.message(), "ok.app", "2.2", 1);
}

// ----------------------------------------------------------------- reset() ---

void testResetClearsMessage() {
  obc::Parser parser;
  const uint8_t haptic[] = {obc::Haptic, 4, 5, 6};
  EXPECT_RESULT(feedAll(parser, haptic, sizeof haptic),
                obc::ParseResult::Complete);
  expectHaptic(parser.message(), 4, 5, 6);

  parser.reset();
  expectMessageIsCleared(parser.message());
}

void testResetMidMessage() {
  // A nearly complete AppInfo is discarded by reset().
  const uint8_t caps[] = {7, 8};
  uint8_t msg[kMaxAppInfoSize];
  const size_t n = buildAppInfo(msg, "reset.mid", "3.1", caps, sizeof caps);
  obc::Parser parser;
  for (size_t i = 0; i + 1 < n; ++i) {
    expectResult(parser.feed(msg[i]), obc::ParseResult::Incomplete, __LINE__);
  }
  parser.reset();
  expectMessageIsCleared(parser.message());
  // Re-feeding the whole message completes normally.
  EXPECT_RESULT(feedAll(parser, msg, n), obc::ParseResult::Complete);
  expectAppInfo(parser.message(), "reset.mid", "3.1", 2);

  // A partial Haptic is discarded too: after reset() the next byte is judged
  // as a message start. Had {3,1} survived, 2 would be its duration byte and
  // the result would be Incomplete; with cleared state it is an unknown type.
  obc::Parser hapticParser;
  EXPECT_RESULT(hapticParser.feed(obc::Haptic), obc::ParseResult::Incomplete);
  EXPECT_RESULT(hapticParser.feed(1), obc::ParseResult::Incomplete);
  hapticParser.reset();
  EXPECT_RESULT(hapticParser.feed(2), obc::ParseResult::Invalid);
  const uint8_t haptic[] = {obc::Haptic, 7, 8, 9};
  EXPECT_RESULT(feedAll(hapticParser, haptic, sizeof haptic),
                obc::ParseResult::Complete);
  expectHaptic(hapticParser.message(), 7, 8, 9);
}

void testResetBetweenClients() {
  // One parser object reused across sequential client connections.
  obc::Parser parser;

  // Client A: one Haptic.
  const uint8_t haptic[] = {obc::Haptic, 1, 2, 3};
  EXPECT_RESULT(feedAll(parser, haptic, sizeof haptic),
                obc::ParseResult::Complete);
  expectHaptic(parser.message(), 1, 2, 3);

  parser.reset();

  // Client B: one AppInfo.
  const uint8_t caps[] = {4, 3};
  uint8_t msg[kMaxAppInfoSize];
  const size_t n = buildAppInfo(msg, "client.b", "7.7", caps, sizeof caps);
  EXPECT_RESULT(feedAll(parser, msg, n), obc::ParseResult::Complete);
  expectAppInfo(parser.message(), "client.b", "7.7", 2);

  parser.reset();

  // Client C: partial message, then disconnect.
  EXPECT_RESULT(parser.feed(obc::AppInfo), obc::ParseResult::Incomplete);
  EXPECT_RESULT(parser.feed(1), obc::ParseResult::Incomplete);
  parser.reset();
  expectMessageIsCleared(parser.message());

  // Client D: clean reuse.
  EXPECT_RESULT(feedAll(parser, haptic, sizeof haptic),
                obc::ParseResult::Complete);
  expectHaptic(parser.message(), 1, 2, 3);
}

}  // namespace

int main() {
  testHapticWholeMessage();
  testHapticBoundaryValues();
  testHapticByteByByteAllSplitPoints();
  testAppInfoWholeMessage();
  testAppInfoByteByByteAllSplitPoints();
  testEmptyStringsAndZeroCapabilities();
  testBoundaryStringLengths();
  testMaxLengths32Plus32Plus255();
  testGluedHaptics();
  testGluedMixedStream();
  testCapabilitiesContainingTypeBytes();
  testSingleCapabilityEqualToTypeByte();
  testIncompleteHapticNotExecuted();
  testIncompleteAppInfoNotExecuted();
  testInvalidTypeRejected();
  testInvalidVersionRejected();
  testInvalidStringLengthsRejected();
  testParserUsableAfterInvalid();
  testResetClearsMessage();
  testResetMidMessage();
  testResetBetweenClients();

  std::printf("%s: %d checks, %d failures\n",
              g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
