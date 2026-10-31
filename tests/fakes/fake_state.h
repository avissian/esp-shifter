// Shared host-side fake state for the Arduino/ESP8266 SDK fakes and the test:
// virtual clock, GPIO levels, serial log, restart counter and the
// deterministic ESP.random() sequence. Kept separate so every fake header
// stays self-contained while the test stays in control of all inputs.
#ifndef TESTS_FAKES_FAKE_STATE_H
#define TESTS_FAKES_FAKE_STATE_H

#include <string>
#include <vector>

namespace fake {

struct State {
  enum { kPinCount = 64 };

  State()
      : nowMs(0), delayCalls(0), delayMsTotal(0), restarts(0),
        randomState(0x24FCF93Bu) {
    for (int i = 0; i < kPinCount; ++i) {
      pinModes[i] = 0;
      pinInputs[i] = 1;  // INPUT_PULLUP idle level (HIGH; buttons short to GND)
      pinOutputs[i] = 0;
    }
  }

  // Virtual clock. delay() advances it so firmware wait loops terminate
  // without ever blocking the host test process.
  unsigned long nowMs;
  unsigned long delayCalls;
  unsigned long delayMsTotal;

  // GPIO: modes and output levels are recorded; input levels are test-driven.
  unsigned char pinModes[kPinCount];
  int pinInputs[kPinCount];
  int pinOutputs[kPinCount];

  std::string serialOutput;
  int restarts;

  // Deterministic ESP.random(): each draw is recorded so the test can rebuild
  // values the firmware derived from the sequence (e.g. the boot-scoped setup
  // token); the generator state resets together with the rest of the world.
  unsigned int randomState;
  std::vector<unsigned int> randomHistory;
};

inline State& state() {
  static State instance;
  return instance;
}

}  // namespace fake

#endif  // TESTS_FAKES_FAKE_STATE_H
