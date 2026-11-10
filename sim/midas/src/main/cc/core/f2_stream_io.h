// See LICENSE for license details.
#ifndef FIRESIM_F2_STREAM_IO_H
#define FIRESIM_F2_STREAM_IO_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#if defined(__x86_64__) || defined(__i386__)
#include <smmintrin.h>
#endif

namespace f2_stream_io {
// BAR4 is write-combining memory. A cache line may still split into several
// strobed PCIS writes (for example on an interrupt). The FPGA assembles those
// fragments. Finish each line before starting the next so only one incomplete
// line per stream can exist, even when WC buffers flush out of address order.
inline void fence() {
#if defined(__x86_64__) || defined(__i386__)
  asm volatile("mfence" ::: "memory");
#else
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

inline bool streaming_load_supported() {
#if defined(__x86_64__) || defined(__i386__)
  return __builtin_cpu_supports("sse4.1");
#else
  return false;
#endif
}

#if defined(__x86_64__) || defined(__i386__)
// This instruction may fetch a complete WC cache line speculatively. Use it
// only for an explicitly acknowledged stream whose payload reads retain the
// head. MFENCE orders these loads against the device's next-head update.
// SSE4.1 is local to this function; the rest of the driver keeps its baseline
// instruction set and selects this path only after checking CPUID.
__attribute__((target("sse4.1"), noinline))
inline void read_stable_line_sse41(const char *mapped, char *data) {
  fence();
  const auto *source = reinterpret_cast<const __m128i *>(mapped);
  const auto a = _mm_stream_load_si128(const_cast<__m128i *>(source));
  const auto b = _mm_stream_load_si128(const_cast<__m128i *>(source + 1));
  const auto c = _mm_stream_load_si128(const_cast<__m128i *>(source + 2));
  const auto d = _mm_stream_load_si128(const_cast<__m128i *>(source + 3));
  _mm_storeu_si128(reinterpret_cast<__m128i *>(data), a);
  _mm_storeu_si128(reinterpret_cast<__m128i *>(data + 16), b);
  _mm_storeu_si128(reinterpret_cast<__m128i *>(data + 32), c);
  _mm_storeu_si128(reinterpret_cast<__m128i *>(data + 48), d);
  fence();
}
#endif

// Read one stable 64-byte head without acknowledging it. The portable path
// uses eight SDK 64-bit reads; any incomplete read throws before the caller
// can acknowledge the head. Destination buffers need no SIMD alignment.
template <typename Read64>
size_t read_stable_line(const char *mapped, char *data, bool streaming,
                        Read64 read64) {
  if (!data)
    throw std::invalid_argument("F2 stream read requires a destination");
  if (streaming) {
    if (!mapped || reinterpret_cast<uintptr_t>(mapped) % 64)
      throw std::invalid_argument("F2 streaming reads require an aligned 64-byte head");
#if defined(__x86_64__) || defined(__i386__)
    if (!streaming_load_supported())
      throw std::runtime_error("F2 streaming read requires SSE4.1");
    read_stable_line_sse41(mapped, data);
    return 64;
#else
    throw std::runtime_error("F2 streaming read requires an x86 host");
#endif
  }
  for (size_t offset = 0; offset < 64; offset += sizeof(uint64_t)) {
    uint64_t word;
    if (read64(offset, &word) != sizeof(word))
      throw std::runtime_error("F2 stable-head read was incomplete");
    std::memcpy(data + offset, &word, sizeof(word));
  }
  fence();
  return 64;
}

template <typename Write>
size_t write_lines(size_t address, const char *data, size_t bytes, Write write) {
  constexpr size_t line_bytes = 64;
  if (address % line_bytes || bytes % line_bytes)
    throw std::invalid_argument("F2 stream writes require whole aligned 64-byte lines");
  for (size_t offset = 0; offset < bytes; offset += line_bytes) {
    if (write(address + offset, data + offset, line_bytes) != line_bytes)
      throw std::runtime_error("F2 stream line write was incomplete");
    fence();
  }
  return bytes;
}
} // namespace f2_stream_io
#endif
