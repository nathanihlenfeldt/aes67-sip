#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace aes67sip {

/**
 * Single producer / single consumer lock free float ring buffer.
 *
 * Used to hand audio between the AES67 routing thread and the PJSIP media
 * thread.  Reads and writes are non blocking: callers get back the number of
 * samples actually transferred and are expected to pad with silence (or drop)
 * the remainder.
 */
class SpscRing {
 public:
  explicit SpscRing(size_t capacity = 16384)
      : buffer_(capacity == 0 ? 1024 : capacity) {}

  size_t capacity() const { return buffer_.size(); }

  size_t available() const {
    const size_t write = write_index_.load(std::memory_order_acquire);
    const size_t read = read_index_.load(std::memory_order_acquire);
    return write - read;
  }

  size_t space() const { return capacity() - available(); }

  /** Writes up to `count` samples, returns the number written. */
  size_t write(const float* source, size_t count) {
    const size_t write = write_index_.load(std::memory_order_relaxed);
    const size_t read = read_index_.load(std::memory_order_acquire);
    const size_t free_space = capacity() - (write - read);
    const size_t to_write = std::min(count, free_space);
    for (size_t i = 0; i < to_write; ++i) {
      buffer_[(write + i) % capacity()] = source[i];
    }
    write_index_.store(write + to_write, std::memory_order_release);
    return to_write;
  }

  /** Reads up to `count` samples, returns the number read. */
  size_t read(float* destination, size_t count) {
    const size_t read = read_index_.load(std::memory_order_relaxed);
    const size_t write = write_index_.load(std::memory_order_acquire);
    const size_t to_read = std::min(count, write - read);
    for (size_t i = 0; i < to_read; ++i) {
      destination[i] = buffer_[(read + i) % capacity()];
    }
    read_index_.store(read + to_read, std::memory_order_release);
    return to_read;
  }

  /** Discards the oldest `count` samples. */
  void discard(size_t count) {
    const size_t read = read_index_.load(std::memory_order_relaxed);
    const size_t write = write_index_.load(std::memory_order_acquire);
    const size_t to_drop = std::min(count, write - read);
    read_index_.store(read + to_drop, std::memory_order_release);
  }

  void reset() {
    read_index_.store(0, std::memory_order_release);
    write_index_.store(0, std::memory_order_release);
  }

 private:
  std::vector<float> buffer_;
  std::atomic<size_t> write_index_{0};
  std::atomic<size_t> read_index_{0};
};

}  // namespace aes67sip
