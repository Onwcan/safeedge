// SPDX-License-Identifier: Apache-2.0
#include "safeedge/ipc/shared_memory.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <type_traits>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace safeedge::ipc {
namespace {

/// POSIX requires a shared-memory name to be "/" followed by up to NAME_MAX
/// characters, none of them "/". Checking here turns a confusing EINVAL from
/// deep inside libc into a message naming the actual rule.
// @satisfies REQ-IPC-003
bool nameIsWellFormed(const char* name) noexcept {
  if (name == nullptr || name[0] != '/' || name[1] == '\0') {
    return false;
  }
  return std::strchr(name + 1, '/') == nullptr;
}

#if defined(__linux__)
template <typename Result>
const char* resolvedErrorText(Result result, const char* buffer) noexcept {
  if constexpr (std::is_pointer_v<Result>) {
    return result != nullptr ? result : "unknown error";
  } else {
    return result == 0 ? buffer : "unknown error";
  }
}

const char* threadSafeErrorText(int error) noexcept {
  thread_local std::array<char, 256> buffer{};
  return resolvedErrorText(::strerror_r(error, buffer.data(), buffer.size()),
                           buffer.data());
}

/// Maps `bytes` of `descriptor`, or returns nullptr and fills `error`.
///
/// Returns the raw address rather than a SharedMemoryRegion so that it can live
/// in an anonymous namespace: a free function is not a friend and cannot touch
/// the private members, and making it a member purely to satisfy access rules
/// would put an implementation detail in the public header.
void* mapDescriptor(int descriptor, std::size_t bytes,
                    SharedMemoryError& error) noexcept {
  void* address =
      ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
  if (address == MAP_FAILED) {
    error = {"mmap", errno};
    return nullptr;
  }
  // The caller closes the descriptor immediately after this returns. The
  // mapping keeps the region alive on its own -- worth stating, because holding
  // the descriptor open "just in case" is a common and unnecessary source of fd
  // leaks in long-running processes.
  error = {};
  return address;
}
#endif

}  // namespace

const char* SharedMemoryError::what() const noexcept {
  if (ok()) {
    return "ok";
  }
#if defined(__linux__)
  return threadSafeErrorText(error_number);
#else
  return std::strerror(error_number);  // NOLINT(concurrency-mt-unsafe)
#endif
}

SharedMemoryRegion::~SharedMemoryRegion() { close(); }

SharedMemoryRegion::SharedMemoryRegion(SharedMemoryRegion&& other) noexcept
    : address_(std::exchange(other.address_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      error_(other.error_) {}

// @satisfies REQ-IPC-005
SharedMemoryRegion& SharedMemoryRegion::operator=(SharedMemoryRegion&& other) noexcept {
  if (this != &other) {
    close();
    address_ = std::exchange(other.address_, nullptr);
    size_ = std::exchange(other.size_, 0);
    error_ = other.error_;
  }
  return *this;
}

void SharedMemoryRegion::close() noexcept {
#if defined(__linux__)
  if (address_ != nullptr) {
    ::munmap(address_, size_);
  }
#endif
  address_ = nullptr;
  size_ = 0;
}

// @satisfies REQ-IPC-004
void SharedMemoryRegion::unlinkName(const char* name) noexcept {
#if defined(__linux__)
  if (nameIsWellFormed(name)) {
    // ENOENT is the expected outcome when it was already removed, so the
    // result is deliberately ignored -- this is idempotent by contract.
    (void)::shm_unlink(name);
  }
#else
  (void)name;
#endif
}

#if defined(__linux__)

SharedMemoryRegion SharedMemoryRegion::create(const char* name, std::size_t bytes) {
  SharedMemoryRegion region;
  if (!nameIsWellFormed(name)) {
    region.error_ = {"name", EINVAL};
    return region;
  }
  if (bytes == 0) {
    region.error_ = {"size", EINVAL};
    return region;
  }

  // O_EXCL: never adopt an existing region. Sharing state with the remains of
  // a previous instance is worse than failing to start.
  // @satisfies REQ-IPC-001
  const int descriptor = ::shm_open(name, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    region.error_ = {"shm_open", errno};
    return region;
  }

  if (::ftruncate(descriptor, static_cast<off_t>(bytes)) != 0) {
    region.error_ = {"ftruncate", errno};
    ::close(descriptor);
    // The name exists but the region is unusable; remove it rather than leave a
    // zero-length trap for the next process to open successfully and then read
    // past the end of.
    (void)::shm_unlink(name);
    return region;
  }

  region.address_ = mapDescriptor(descriptor, bytes, region.error_);
  region.size_ = region.address_ != nullptr ? bytes : 0;
  ::close(descriptor);
  if (!region.valid()) {
    (void)::shm_unlink(name);
  }
  return region;
}

SharedMemoryRegion SharedMemoryRegion::openExisting(const char* name, std::size_t bytes) {
  SharedMemoryRegion region;
  if (!nameIsWellFormed(name)) {
    region.error_ = {"name", EINVAL};
    return region;
  }

  const int descriptor = ::shm_open(name, O_RDWR, 0);
  if (descriptor < 0) {
    region.error_ = {"shm_open", errno};
    return region;
  }

  // Verify the size before mapping. Mapping more than the region holds is
  // permitted by mmap and produces SIGBUS on first touch of the excess -- a
  // fault that surfaces later, somewhere else, as a mysterious crash rather
  // than as the configuration mismatch it actually is.
  struct stat status {};
  if (::fstat(descriptor, &status) != 0) {
    region.error_ = {"fstat", errno};
    ::close(descriptor);
    return region;
  }
  // @satisfies REQ-IPC-002
  if (static_cast<std::size_t>(status.st_size) < bytes) {
    region.error_ = {"size", EINVAL};
    ::close(descriptor);
    return region;
  }

  region.address_ = mapDescriptor(descriptor, bytes, region.error_);
  region.size_ = region.address_ != nullptr ? bytes : 0;
  ::close(descriptor);
  return region;
}

#else  // !__linux__

SharedMemoryRegion SharedMemoryRegion::create(const char* name, std::size_t bytes) {
  (void)name;
  (void)bytes;
  SharedMemoryRegion region;
  region.error_ = {"platform", ENOSYS};
  return region;
}

SharedMemoryRegion SharedMemoryRegion::openExisting(const char* name, std::size_t bytes) {
  (void)name;
  (void)bytes;
  SharedMemoryRegion region;
  region.error_ = {"platform", ENOSYS};
  return region;
}

#endif

}  // namespace safeedge::ipc
