// SPDX-License-Identifier: Apache-2.0
//
// Latency and throughput characterisation for SpscRing.
//
// Deliberately dependency-free rather than built on Google Benchmark. That
// library reports means and standard deviations, which are the wrong summary
// for a real-time path: a control loop does not care about the average handoff,
// it cares about the worst one that still has to fit inside the cycle. So this
// harness keeps every sample and reports percentiles, including p99.9.
//
// It also refuses to report a single number where a single number would lie:
//   * every throughput figure is the median of N repetitions, printed with its
//     min and max so the spread is visible;
//   * the cost of the clock itself is reported, not silently subtracted;
//   * whether CPU pinning actually succeeded is reported, because on a
//     virtualised host it usually has not;
//   * a virtualised platform is detected and called out, because measurements
//     taken there characterise the hypervisor at least as much as the code.
//
// The point of a benchmark is to support a claim. A benchmark that hides its
// own error bars cannot support anything.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "safeedge/concurrent/spsc_ring.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;

constexpr std::size_t kRepetitions = 7;
constexpr std::size_t kThroughputItems = 3'000'000;
constexpr std::size_t kLatencyItems = 300'000;
constexpr std::size_t kProducerCpu = 4;
constexpr std::size_t kConsumerCpu = 6;

// ---------------------------------------------------------------------------
// A deliberately naive ring, used only as a control in the false-sharing
// comparison. Same algorithm, same memory ordering -- the ONLY differences are
// that the two positions share a cache line and that neither side caches the
// other position. This isolates the cost of the layout from the cost of the
// algorithm.
// ---------------------------------------------------------------------------
template <typename T, std::size_t Capacity>
class NaiveRing {
 public:
  [[nodiscard]] bool tryPush(const T& value) noexcept {
    const std::uint64_t write = write_pos_.load(std::memory_order_relaxed);
    if (write - read_pos_.load(std::memory_order_acquire) >= Capacity) {
      return false;
    }
    slots_[write & (Capacity - 1)] = value;
    write_pos_.store(write + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool tryPop(T& out) noexcept {
    const std::uint64_t read = read_pos_.load(std::memory_order_relaxed);
    if (read == write_pos_.load(std::memory_order_acquire)) {
      return false;
    }
    out = slots_[read & (Capacity - 1)];
    read_pos_.store(read + 1, std::memory_order_release);
    return true;
  }

 private:
  std::atomic<std::uint64_t> write_pos_{0};
  std::atomic<std::uint64_t> read_pos_{0};
  std::array<T, Capacity> slots_{};
};

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------
struct Percentiles {
  std::int64_t p50{};
  std::int64_t p90{};
  std::int64_t p99{};
  std::int64_t p999{};
  std::int64_t max{};
};

Percentiles computePercentiles(std::vector<std::int64_t>& samples) {
  if (samples.empty()) {
    return {};
  }
  std::sort(samples.begin(), samples.end());
  const auto pick = [&samples](double q) -> std::int64_t {
    const auto n = static_cast<double>(samples.size() - 1);
    return samples[static_cast<std::size_t>(q * n)];
  };
  return {pick(0.50), pick(0.90), pick(0.99), pick(0.999), samples.back()};
}

struct Spread {
  double median{};
  double min{};
  double max{};
};

Spread summarize(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return {values[values.size() / 2], values.front(), values.back()};
}

// ---------------------------------------------------------------------------
// Platform interrogation
// ---------------------------------------------------------------------------

/// Returns true only if the affinity call actually succeeded AND the thread is
/// verifiably running where we asked. A benchmark that assumes pinning worked
/// is a benchmark reporting someone else's scheduling decisions as its own.
bool pinToCpu(std::size_t cpu) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
    return false;
  }
  cpu_set_t check;
  CPU_ZERO(&check);
  if (pthread_getaffinity_np(pthread_self(), sizeof(check), &check) != 0) {
    return false;
  }
  return CPU_ISSET(cpu, &check) && CPU_COUNT(&check) == 1;
#else
  (void)cpu;
  return false;
#endif
}

bool isVirtualised() {
  std::ifstream version("/proc/version");
  if (!version) {
    return false;
  }
  std::string line;
  std::getline(version, line);
  std::transform(line.begin(), line.end(), line.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return line.find("microsoft") != std::string::npos ||
         line.find("wsl") != std::string::npos;
}

std::int64_t measureClockOverhead(std::size_t iterations = 200'000) {
  std::vector<std::int64_t> samples;
  samples.reserve(iterations);
  for (std::size_t i = 0; i < iterations; ++i) {
    const auto a = Clock::now();
    const auto b = Clock::now();
    samples.push_back(std::chrono::duration_cast<Nanos>(b - a).count());
  }
  return computePercentiles(samples).p50;
}

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------

double benchSingleThreadedThroughput(std::size_t items) {
  safeedge::concurrent::SpscRing<std::uint64_t, 1024> ring;
  std::uint64_t sink = 0;
  const auto start = Clock::now();
  for (std::uint64_t i = 0; i < items; ++i) {
    while (!ring.tryPush(i)) {
    }
    while (!ring.tryPop(sink)) {
    }
  }
  const auto elapsed = Clock::now() - start;
  if (sink == 0xFFFFFFFFFFFFFFFFULL) {
    std::fputs("", stderr);  // keep the optimiser honest
  }
  const auto ns = static_cast<double>(std::chrono::duration_cast<Nanos>(elapsed).count());
  return static_cast<double>(items) / (ns / 1e9);
}

template <typename Ring>
double benchCrossThreadThroughput(std::size_t items) {
  auto ring = std::make_unique<Ring>();
  std::atomic<bool> go{false};

  std::thread consumer([&] {
    (void)pinToCpu(kConsumerCpu);
    std::uint64_t out = 0;
    std::uint64_t seen = 0;
    while (!go.load(std::memory_order_acquire)) {
    }
    while (seen < items) {
      if (ring->tryPop(out)) {
        ++seen;
      }
    }
  });

  (void)pinToCpu(kProducerCpu);
  const auto start = Clock::now();
  go.store(true, std::memory_order_release);
  for (std::uint64_t i = 0; i < items; ++i) {
    while (!ring->tryPush(i)) {
    }
  }
  consumer.join();
  const auto elapsed = Clock::now() - start;
  const auto ns = static_cast<double>(std::chrono::duration_cast<Nanos>(elapsed).count());
  return static_cast<double>(items) / (ns / 1e9);
}

Percentiles benchHandoffLatency(std::size_t items) {
  struct Stamped {
    std::int64_t sent_ns{0};
  };

  safeedge::concurrent::SpscRing<Stamped, 1024> ring;
  std::vector<std::int64_t> samples;
  samples.reserve(items);
  std::atomic<bool> go{false};

  std::thread consumer([&] {
    (void)pinToCpu(kConsumerCpu);
    Stamped out;
    while (!go.load(std::memory_order_acquire)) {
    }
    while (samples.size() < items) {
      if (ring.tryPop(out)) {
        const auto now =
            std::chrono::duration_cast<Nanos>(Clock::now().time_since_epoch()).count();
        samples.push_back(now - out.sent_ns);
      }
    }
  });

  (void)pinToCpu(kProducerCpu);
  go.store(true, std::memory_order_release);
  for (std::size_t i = 0; i < items; ++i) {
    Stamped in;
    in.sent_ns =
        std::chrono::duration_cast<Nanos>(Clock::now().time_since_epoch()).count();
    while (!ring.tryPush(in)) {
    }
    // Pace the producer so the ring is not permanently saturated. A saturated
    // ring measures backpressure, not handoff latency.
    for (int spin = 0; spin < 200; ++spin) {
      std::atomic_signal_fence(std::memory_order_seq_cst);
    }
  }
  consumer.join();
  return computePercentiles(samples);
}

void printSpread(const char* label, const Spread& s) {
  std::printf("| %-40s | %8.1f | %8.1f | %8.1f |\n", label, s.median, s.min, s.max);
}

}  // namespace

int main() {
  const bool virtualised = isVirtualised();
  const bool pinned = pinToCpu(kProducerCpu);
  const std::int64_t clock_overhead = measureClockOverhead();

  std::printf("# SpscRing benchmark\n\n");
  std::printf("## Measurement conditions\n\n");
  std::printf("- Hardware concurrency: %u\n", std::thread::hardware_concurrency());
  std::printf("- Repetitions per figure: %zu (median reported, min/max shown)\n",
              kRepetitions);
  std::printf("- steady_clock::now() median cost: %lld ns\n",
              static_cast<long long>(clock_overhead));
  std::printf("- CPU pinning: %s\n", pinned ? "verified" : "FAILED or not honoured");
  std::printf("- Virtualised host detected: %s\n", virtualised ? "YES" : "no");
  if (!pinned) {
    std::printf(
        "\n> CPU pinning could not be verified, so the producer and consumer may\n");
    std::printf(
        "> have been migrated between cores mid-measurement. Cross-core figures\n");
    std::printf("> below are not comparable across runs.\n");
  }
  if (virtualised) {
    std::printf("\n> Taken on a virtualised host. Medians and low percentiles still\n");
    std::printf(
        "> characterise the queue, but everything above roughly p99 characterises\n");
    std::printf(
        "> the hypervisor scheduler, not this code, and must not be quoted as a\n");
    std::printf(
        "> property of it. A defensible tail figure needs bare-metal Linux with\n");
    std::printf("> isolated cores -- which is what rt-latency-lab exists to provide.\n");
  }
  std::printf("\n");

  // -- throughput -----------------------------------------------------------
  std::vector<double> single;
  std::vector<double> padded;
  std::vector<double> naive;
  single.reserve(kRepetitions);
  padded.reserve(kRepetitions);
  naive.reserve(kRepetitions);

  // One untimed warmup of each, to page in the slots and settle the frequency
  // governor before anything is recorded.
  (void)benchSingleThreadedThroughput(kThroughputItems / 4);
  (void)benchCrossThreadThroughput<safeedge::concurrent::SpscRing<std::uint64_t, 1024>>(
      kThroughputItems / 4);
  (void)benchCrossThreadThroughput<NaiveRing<std::uint64_t, 1024>>(kThroughputItems / 4);

  for (std::size_t rep = 0; rep < kRepetitions; ++rep) {
    single.push_back(benchSingleThreadedThroughput(kThroughputItems) / 1e6);
    padded.push_back(
        benchCrossThreadThroughput<safeedge::concurrent::SpscRing<std::uint64_t, 1024>>(
            kThroughputItems) /
        1e6);
    naive.push_back(
        benchCrossThreadThroughput<NaiveRing<std::uint64_t, 1024>>(kThroughputItems) /
        1e6);
  }

  const Spread s_single = summarize(single);
  const Spread s_padded = summarize(padded);
  const Spread s_naive = summarize(naive);

  std::printf("## Throughput, M items/s\n\n");
  std::printf(
      "| Configuration                            |   median |      min |      max |\n");
  std::printf(
      "|------------------------------------------|----------|----------|----------|\n");
  printSpread("Single thread, push+pop (cache hot)", s_single);
  printSpread("Cross-thread, padded + cached positions", s_padded);
  printSpread("Cross-thread, shared line, no caching", s_naive);
  std::printf("\nLayout advantage at the median: %.2fx",
              s_padded.median / s_naive.median);
  std::printf("  (worst rep %.2fx, best rep %.2fx)\n", s_padded.min / s_naive.max,
              s_padded.max / s_naive.min);
  std::printf("Both variants run the same algorithm and the same memory ordering;\n");
  std::printf("the only difference is cache-line layout and position caching.\n\n");

  // -- latency --------------------------------------------------------------
  const Percentiles handoff = benchHandoffLatency(kLatencyItems);
  std::printf("## Handoff latency, producer store to consumer load (ns)\n\n");
  std::printf("|    p50 |    p90 |    p99 |    p99.9 |      max |\n");
  std::printf("|--------|--------|--------|----------|----------|\n");
  std::printf("| %6lld | %6lld | %6lld | %8lld | %8lld |\n",
              static_cast<long long>(handoff.p50), static_cast<long long>(handoff.p90),
              static_cast<long long>(handoff.p99), static_cast<long long>(handoff.p999),
              static_cast<long long>(handoff.max));
  std::printf("\nSamples: %zu. Producer paced to keep the ring unsaturated.\n",
              kLatencyItems);

  return 0;
}
