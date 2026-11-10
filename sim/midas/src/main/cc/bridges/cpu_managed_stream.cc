#include "cpu_managed_stream.h"
#include "core/simif.h"

#include <cassert>
#include <algorithm>
#include <atomic>
#include <cstdio>

namespace {
// F2 BAR4 may be write-combining memory. Payload reads are non-destructive in
// the explicit-ACK protocol; complete all requested loads and buffer stores
// before the control-bus dequeue, and order the next head after ACK readback.
void stream_io_fence() {
#if defined(__x86_64__) || defined(__i386__)
  asm volatile("mfence" ::: "memory");
#else
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}
}

using namespace CPUManagedStreams;

/**
 * @brief Enqueues as much as num_bytes of data into the associated stream
 *
 * @param src Source from which to copy data to enqueue
 * @param num_bytes Desired number of bytes to enqueue
 * @param required_bytes Minimum number of bytes to enqueue. If fewer bytes
 *        would be enqueued, this method enqueues none and returns 0.
 * @return size_t
 */
size_t CPUManagedStreams::CPUToFPGADriver::push(void *src,
                                                size_t num_bytes,
                                                size_t required_bytes) {

  assert(num_bytes >= required_bytes);

  // Similarly to above, the legacy implementation of DMA does not correctly
  // implement non-multiples of 512b. The FPGA-side queue will take on the
  // high-order bytes of the final beat in the transaction, and the strobe is
  // not respected. So put the assertion here and discuss what to do next.
  assert((num_bytes % fpga_buffer_width_bytes()) == 0);

  auto num_beats = num_bytes / fpga_buffer_width_bytes();
  auto threshold_beats = required_bytes / fpga_buffer_width_bytes();

  assert(threshold_beats <= fpga_buffer_size());
  const auto occupied = mmio_read(count_addr());
  if (occupied > fpga_buffer_size()) {
    throw std::runtime_error("CPU-to-FPGA stream count exceeds capacity at address " +
                             std::to_string(count_addr()) + ": " +
                             std::to_string(occupied) + " > " +
                             std::to_string(fpga_buffer_size()));
  }
  const auto space_available = fpga_buffer_size() - occupied;

  if ((space_available == 0) || (space_available < threshold_beats)) {
    ++blocked_pushes;
    if (!reported_backpressure) {
      std::fprintf(stderr,
                   "CPU_STREAM_BACKPRESSURE count_addr=%llu occupied=%zu "
                   "capacity=%u request_beats=%zu minimum_beats=%zu "
                   "accepted_beats=%llu\n",
                   static_cast<unsigned long long>(count_addr()), occupied,
                   fpga_buffer_size(), num_beats, threshold_beats,
                   static_cast<unsigned long long>(accepted_beats));
      reported_backpressure = true;
    }
    return 0;
  }

  auto push_beats = std::min(space_available, num_beats);
  auto push_bytes = push_beats * fpga_buffer_width_bytes();
  auto bytes_written =
      cpu_managed_axi4_write(dma_addr(), (char *)src, push_bytes);
  assert(bytes_written == push_bytes);
  accepted_beats += bytes_written / fpga_buffer_width_bytes();
  if (reported_backpressure && !reported_recovery) {
    std::fprintf(stderr,
                 "CPU_STREAM_RECOVERED count_addr=%llu occupied=%zu capacity=%u "
                 "blocked_pushes=%llu accepted_beats=%llu\n",
                 static_cast<unsigned long long>(count_addr()), occupied,
                 fpga_buffer_size(), static_cast<unsigned long long>(blocked_pushes),
                 static_cast<unsigned long long>(accepted_beats));
    reported_recovery = true;
  }

  return bytes_written;
}

/**
 * @brief Dequeues as much as num_bytes of data from the associated bridge
 * stream.
 *
 * @param dest  Buffer into which to copy dequeued stream data
 * @param num_bytes  Bytes of data to dequeue
 * @param required_bytes  Minimum number of bytes to dequeue. If fewer bytes
 * would be dequeued, dequeue none and return 0.
 * @return size_t Number of bytes successfully dequeued
 */
size_t CPUManagedStreams::FPGAToCPUDriver::pull(void *dest,
                                                size_t num_bytes,
                                                size_t required_bytes) {
  if (explicit_ack() && !explicit_ack_supported()) {
    throw std::runtime_error("Explicit-ACK CPU stream requires a non-destructive payload-read backend");
  }
  if (explicit_ack() && (fpga_buffer_width_bytes() != 64 || dma_addr() % 64 ||
      num_bytes % 64 || required_bytes % 64 || required_bytes > num_bytes)) {
    throw std::invalid_argument("Explicit-ACK CPU stream reads require aligned whole 64-byte beats");
  }
  assert(num_bytes >= required_bytes);

  // The legacy code is clearly broken for requests that aren't a
  // multiple of 512b since CPU_MANAGED_AXI4_SIZE is fixed to the full width of
  // the AXI4 IF. The high-order bytes of the final word will be copied into the
  // destination buffer (potentially an overflow, bug 1), and since reads are
  // destructive, will not be visible to future pulls (bug 2). So i've put this
  // assertion here for now...

  // Due to the destructive nature of reads, if we wish to support reads that
  // aren't a multiple of 512b, we'll need to keep a little buffer around for
  // the remainder, and prepend this to the destination buffer.
  assert((num_bytes % fpga_buffer_width_bytes()) == 0);

  auto num_beats = num_bytes / fpga_buffer_width_bytes();
  auto threshold_beats = required_bytes / fpga_buffer_width_bytes();

  assert(threshold_beats <= fpga_buffer_size());
  auto count = mmio_read(count_addr());
  if (explicit_ack() && count > fpga_buffer_size()) {
    throw std::runtime_error("Explicit-ACK CPU stream occupancy exceeds its declared capacity");
  }

  if ((count == 0) || (count < threshold_beats)) {
    return 0;
  }

  auto pull_beats = std::min(count, num_beats);
  auto pull_bytes = pull_beats * fpga_buffer_width_bytes();
  if (explicit_ack()) {
    auto *bytes = static_cast<char *>(dest);
    for (size_t offset = 0; offset < pull_bytes; offset += 64) {
      // Every PCI read observes the same stable head until this host commits
      // it. Neither transaction count nor the shell's low-address forwarding
      // determines when data is consumed.
      const auto received = cpu_managed_axi4_read_acked(dma_addr(), bytes + offset, 64);
      if (received != 64)
        throw std::runtime_error("Explicit-ACK CPU stream read was incomplete; head was not acknowledged");
      stream_io_fence();
      mmio_write(count_addr(), 1);
      // Flush the posted control write through the same register interface
      // before a later BAR4 request may observe the next queue head.
      (void)mmio_read(count_addr());
      stream_io_fence();
    }
    return pull_bytes;
  }
  auto bytes_read = cpu_managed_axi4_read(dma_addr(), (char *)dest, pull_bytes);
  assert(bytes_read == pull_bytes);
  return bytes_read;
}

CPUManagedStreamWidget::CPUManagedStreamWidget(
    simif_t &simif,
    unsigned index,
    const std::vector<std::string> &args,
    std::vector<CPUManagedStreams::StreamParameters> &&from_cpu,
    std::vector<CPUManagedStreams::StreamParameters> &&to_cpu) {
  assert(index == 0 && "only one managed stream engine is allowed");

  auto &io = simif.get_cpu_managed_stream_io();
  for (auto &&params : from_cpu) {
    cpu_to_fpga_streams.push_back(
        std::make_unique<CPUManagedStreams::CPUToFPGADriver>(std::move(params),
                                                             io));
  }

  for (auto &&params : to_cpu) {
    fpga_to_cpu_streams.push_back(
        std::make_unique<CPUManagedStreams::FPGAToCPUDriver>(std::move(params),
                                                             io));
  }
}
