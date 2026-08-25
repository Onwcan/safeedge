// SPDX-License-Identifier: Apache-2.0
//
// libFuzzer target for the safety telegram decoder.
//
// The decoder is the one place in this runtime that parses bytes it did not
// produce, arriving over a transport that is assumed to be entirely
// untrustworthy. Everything else in safeedge consumes data that some other part
// of safeedge created. That asymmetry is why this function, and only this
// function, gets a fuzzer.
//
// What is being checked is not "does it reject bad input" -- the unit tests
// cover the fault model exhaustively. It is the weaker and more important
// property that **no input can make it misbehave**: no out-of-bounds read, no
// uninitialised value, no signed overflow, no unbounded loop. A safety layer
// that can be crashed by a malformed frame has replaced a detectable fault
// with an undetectable one.
//
// Build:  cmake --preset fuzz && cmake --build --preset fuzz
// Run:    ./build/fuzz/fuzz/fuzz_telegram_decode -max_total_time=60 fuzz/corpus

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "safeedge/safety/black_channel.hpp"

namespace {

using namespace safeedge::safety;

const SafetyAddress kAddress{/*source=*/0x0011, /*destination=*/0x2200,
                             /*parameter_signature=*/0xDEADBEEF};

}  // namespace

// @verifies REQ-SAF-017
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // A fresh receiver per input keeps each case independent and reproducible
  // from the input file alone -- a fuzzer finding that depends on hidden
  // carried-over state is very hard to act on.
  SafetyReceiver receiver(kAddress, /*watchdog_ns=*/10'000'000);

  std::array<std::uint8_t, kMaxPayloadBytes> payload{};
  std::size_t payload_size = 0;
  std::uint8_t status = 0;

  // The first byte, when present, steers the simulated arrival time so the
  // watchdog and the latch/acknowledge paths are reachable rather than dead
  // code from the fuzzer's point of view. The remainder is the frame.
  std::int64_t now_ns = 0;
  std::span<const std::uint8_t> frame{data, size};
  if (size > 0) {
    now_ns = static_cast<std::int64_t>(data[0]) * 1'000'000;
    frame = frame.subspan(1);
  }

  const ReceiveStatus first =
      receiver.receive(frame, now_ns, payload, payload_size, status);

  // Feed it a second time. Repetition, latching and the refusal to deliver
  // while latched all need at least two deliveries to exercise, and a
  // single-shot target would never reach them.
  (void)receiver.receive(frame, now_ns + 1'000'000, payload, payload_size, status);
  (void)receiver.poll(now_ns + 20'000'000);

  if (first == ReceiveStatus::kValid) {
    // If a frame was accepted, the reported payload size must be consistent
    // with the frame it came from. An inconsistency here would mean the
    // consumer is handed a length that does not match the data.
    if (payload_size + kOverheadBytes != frame.size()) {
      __builtin_trap();
    }
  } else if (payload_size != 0) {
    // A rejected frame must never leave a payload behind for the caller to
    // act on.
    __builtin_trap();
  }

  return 0;
}
