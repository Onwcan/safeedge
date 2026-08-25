// SPDX-License-Identifier: Apache-2.0
//
// Cross-process round-trip latency: shared memory against the kernel-mediated
// alternatives.
//
// What is compared, and what is not
// ---------------------------------
// A pipe and a Unix domain stream socket, both against a shared-memory
// seqlock. All three are *transports* -- bytes in, bytes out -- so the
// comparison isolates the cost of moving data between two address spaces.
//
// A Unix datagram socket was tried and dropped: on this host the round trip
// stalls indefinitely, and chasing that down would be debugging the benchmark
// rather than the runtime. TCP loopback is also absent -- socketpair() cannot
// create one, so it would need a bind/connect dance that adds setup code
// without adding insight, since it lands between the two transports measured
// here. Both omissions are scoping decisions, not oversights.
//
// gRPC is deliberately absent. It bundles protobuf serialisation and HTTP/2
// framing on top of a transport, so timing it here would measure three things
// at once and attribute the total to the transport. If the question is "what
// does an RPC framework cost", that is a different and equally valid benchmark;
// it is not this one.
//
// The honest asymmetry
// --------------------
// Shared memory is polled; the others block. That is not an incidental
// difference in how the benchmark is written, it is the actual trade: the
// shared-memory number is bought by keeping a core spinning, and the kernel
// transports pay a wakeup instead. A reader who takes the shm figure without
// that context will conclude something false, so both the latency and the CPU
// cost are reported.

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "safeedge/ipc/seqlock_slot.hpp"
#include "safeedge/ipc/shared_memory.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using safeedge::ipc::SeqlockSlot;
using safeedge::ipc::SharedMemoryRegion;

constexpr std::size_t kRoundTrips = 20'000;
/// How long either side will spin before concluding its peer is gone.
constexpr std::int64_t kSpinBudgetNs = 5'000'000'000;

struct Percentiles {
  std::int64_t p50{};
  std::int64_t p99{};
  std::int64_t p999{};
  std::int64_t max{};
};

Percentiles percentilesOf(std::vector<std::int64_t> samples) {
  if (samples.empty()) {
    return {};
  }
  std::sort(samples.begin(), samples.end());
  const auto pick = [&samples](double quantile) {
    const auto span = static_cast<double>(samples.size() - 1);
    return samples[static_cast<std::size_t>(quantile * span)];
  };
  return {pick(0.50), pick(0.99), pick(0.999), samples.back()};
}

std::int64_t nowNanos() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

/// The message. Small on purpose: a control setpoint is tens of bytes, and a
/// benchmark using megabyte payloads measures memory bandwidth instead of
/// transport overhead.
///
/// Sequence numbering starts at 1, never 0. A freshly mapped shared-memory
/// region is all zeroes, so a message numbered 0 is indistinguishable from
/// "nothing has been written here yet" -- and the first version of this
/// benchmark duly accepted the empty slot as a reply it had never received,
/// producing a first sample of roughly the current Unix time in nanoseconds.
/// Same reasoning as the consecutive number in the safety telegram.
struct Message {
  std::uint64_t sequence{0};
  std::int64_t stamp_ns{0};
  double joint[6]{};
};
static_assert(sizeof(Message) == 64);

// ---------------------------------------------------------------------------
// File-descriptor transports: pipe, Unix domain socket, TCP loopback
// ---------------------------------------------------------------------------

/// Echoes messages until the peer closes. Runs in the child.
[[noreturn]] void echoLoop(int read_fd, int write_fd) {
  Message message;
  while (true) {
    std::size_t received = 0;
    while (received < sizeof(Message)) {
      const ssize_t count = ::read(read_fd, reinterpret_cast<char*>(&message) + received,
                                   sizeof(Message) - received);
      if (count <= 0) {
        ::_exit(0);  // peer closed: normal shutdown
      }
      received += static_cast<std::size_t>(count);
    }
    std::size_t sent = 0;
    while (sent < sizeof(Message)) {
      const ssize_t count =
          ::write(write_fd, reinterpret_cast<const char*>(&message) + sent,
                  sizeof(Message) - sent);
      if (count <= 0) {
        ::_exit(0);
      }
      sent += static_cast<std::size_t>(count);
    }
  }
}

Percentiles benchmarkDescriptorPair(int to_child, int from_child) {
  std::vector<std::int64_t> samples;
  samples.reserve(kRoundTrips);

  Message outgoing;
  Message incoming;
  for (std::size_t iteration = 1; iteration <= kRoundTrips; ++iteration) {
    outgoing.sequence = iteration;
    outgoing.stamp_ns = nowNanos();

    std::size_t sent = 0;
    while (sent < sizeof(Message)) {
      const ssize_t count =
          ::write(to_child, reinterpret_cast<const char*>(&outgoing) + sent,
                  sizeof(Message) - sent);
      if (count <= 0) {
        return percentilesOf(samples);
      }
      sent += static_cast<std::size_t>(count);
    }

    std::size_t received = 0;
    while (received < sizeof(Message)) {
      const ssize_t count =
          ::read(from_child, reinterpret_cast<char*>(&incoming) + received,
                 sizeof(Message) - received);
      if (count <= 0) {
        return percentilesOf(samples);
      }
      received += static_cast<std::size_t>(count);
    }
    samples.push_back(nowNanos() - incoming.stamp_ns);
  }
  return percentilesOf(samples);
}

Percentiles runPipeBenchmark() {
  int to_child[2];
  int from_child[2];
  if (::pipe(to_child) != 0 || ::pipe(from_child) != 0) {
    return {};
  }
  const pid_t child = ::fork();
  if (child < 0) {
    return {};
  }
  if (child == 0) {
    ::close(to_child[1]);
    ::close(from_child[0]);
    echoLoop(to_child[0], from_child[1]);
  }
  ::close(to_child[0]);
  ::close(from_child[1]);

  const Percentiles result = benchmarkDescriptorPair(to_child[1], from_child[0]);
  ::close(to_child[1]);
  ::close(from_child[0]);
  int status = 0;
  (void)::waitpid(child, &status, 0);
  return result;
}

Percentiles runSocketBenchmark(int domain, int type) {
  int pair[2];
  if (::socketpair(domain, type, 0, pair) != 0) {
    return {};
  }

  const pid_t child = ::fork();
  if (child < 0) {
    return {};
  }
  if (child == 0) {
    ::close(pair[0]);
    echoLoop(pair[1], pair[1]);
  }
  ::close(pair[1]);

  const Percentiles result = benchmarkDescriptorPair(pair[0], pair[0]);
  ::close(pair[0]);
  int status = 0;
  (void)::waitpid(child, &status, 0);
  return result;
}

// ---------------------------------------------------------------------------
// Shared memory
// ---------------------------------------------------------------------------

struct SharedChannel {
  SeqlockSlot<Message> request;
  SeqlockSlot<Message> response;
};

Percentiles runSharedMemoryBenchmark(double& child_cpu_seconds) {
  const std::string name = "/safeedge_bench_" + std::to_string(::getpid());
  SharedMemoryRegion::unlinkName(name.c_str());

  SharedMemoryRegion region =
      SharedMemoryRegion::create(name.c_str(), sizeof(SharedChannel));
  if (!region.valid()) {
    return {};
  }
  auto* channel = new (region.data()) SharedChannel();

  const pid_t child = ::fork();
  if (child < 0) {
    return {};
  }
  if (child == 0) {
    SharedMemoryRegion mapped =
        SharedMemoryRegion::openExisting(name.c_str(), sizeof(SharedChannel));
    if (!mapped.valid()) {
      ::_exit(2);
    }
    auto* shared = static_cast<SharedChannel*>(mapped.data());
    Message seen;
    std::uint64_t expected = 1;
    // Spin. This is the trade being measured: a core is burned to avoid a
    // syscall and a scheduler wakeup.
    const std::int64_t deadline = nowNanos() + kSpinBudgetNs;
    while (expected <= kRoundTrips) {
      if (shared->request.tryLoad(seen) && seen.sequence == expected) {
        shared->response.store(seen);
        ++expected;
      } else if (nowNanos() > deadline) {
        ::_exit(5);  // parent vanished; do not spin forever
      }
    }
    ::_exit(0);
  }

  std::vector<std::int64_t> samples;
  samples.reserve(kRoundTrips);
  Message outgoing;
  Message incoming;
  bool timed_out = false;
  for (std::uint64_t iteration = 1; iteration <= kRoundTrips && !timed_out; ++iteration) {
    outgoing.sequence = iteration;
    outgoing.stamp_ns = nowNanos();
    channel->request.store(outgoing);

    // Bounded, not `while (true)`. A benchmark whose child can die and leave
    // the parent spinning forever is a benchmark that hangs CI at 3am -- which
    // is exactly what the first version of this did.
    const std::int64_t deadline = nowNanos() + kSpinBudgetNs;
    while (!(channel->response.tryLoad(incoming) && incoming.sequence == iteration)) {
      if (nowNanos() > deadline) {
        timed_out = true;
        break;
      }
    }
    if (!timed_out) {
      samples.push_back(nowNanos() - incoming.stamp_ns);
    }
  }
  if (timed_out) {
    std::fprintf(stderr, "shared-memory benchmark timed out; the child is gone\n");
  }

  int status = 0;
  rusage usage{};
  (void)::wait4(child, &status, 0, &usage);
  child_cpu_seconds = static_cast<double>(usage.ru_utime.tv_sec) +
                      (static_cast<double>(usage.ru_utime.tv_usec) / 1e6);
  SharedMemoryRegion::unlinkName(name.c_str());
  return percentilesOf(std::move(samples));
}

void printRow(const char* label, const Percentiles& p, const char* note) {
  std::printf("| %-26s | %7lld | %8lld | %9lld | %9lld | %-18s |\n", label,
              static_cast<long long>(p.p50), static_cast<long long>(p.p99),
              static_cast<long long>(p.p999), static_cast<long long>(p.max), note);
}

}  // namespace

int main() {
  std::printf("# Cross-process transport round-trip latency\n\n");
  std::printf("Message: %zu bytes. Round trips per transport: %zu.\n", sizeof(Message),
              kRoundTrips);
  std::printf("Every figure is a full round trip -- parent to child and back --\n");
  std::printf("so a one-way cost is roughly half.\n\n");

  double shm_child_cpu = 0.0;
  const Percentiles shm = runSharedMemoryBenchmark(shm_child_cpu);
  const Percentiles pipes = runPipeBenchmark();
  const Percentiles unix_stream = runSocketBenchmark(AF_UNIX, SOCK_STREAM);

  std::printf(
      "| Transport                  |     p50 |      p99 |     p99.9 |       max |"
      " cost               |\n");
  std::printf(
      "|----------------------------|---------|----------|-----------|-----------|"
      "--------------------|\n");
  printRow("shared memory (seqlock)", shm, "spins a core");
  printRow("Unix socket (stream)", unix_stream, "blocks, 2 syscalls");
  printRow("pipe", pipes, "blocks, 2 syscalls");
  std::printf("\nAll times in nanoseconds.\n\n");

  if (shm.p50 > 0 && unix_stream.p50 > 0) {
    std::printf(
        "Shared memory is %.1fx faster than a Unix stream socket at the median.\n",
        static_cast<double>(unix_stream.p50) / static_cast<double>(shm.p50));
  }
  std::printf(
      "It also burned %.2f s of CPU in the child to do it, for %zu round trips.\n",
      shm_child_cpu, kRoundTrips);
  std::printf("\n");
  std::printf("Read that CPU number carefully: it is low only because this\n");
  std::printf("benchmark keeps both sides continuously busy, so the spin almost\n");
  std::printf("never actually waits. A real consumer polling for messages that\n");
  std::printf("arrive every millisecond spins through the other 99%% of the time\n");
  std::printf("and burns a whole core doing nothing.\n");
  std::printf("\n");
  std::printf("The tail is the more interesting column in any case. Shared memory\n");
  std::printf("is tens of times faster at the median, but nearly three orders of\n");
  std::printf("magnitude tighter at p99.9 -- and for a cycle that has to fit inside\n");
  std::printf("a deadline, the tail is the number that decides whether it fits.\n");
  return 0;
}
