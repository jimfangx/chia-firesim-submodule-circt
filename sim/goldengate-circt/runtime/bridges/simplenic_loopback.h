// See LICENSE for license details.
#ifndef GOLDENGATE_SIMPLENIC_LOOPBACK_H
#define GOLDENGATE_SIMPLENIC_LOOPBACK_H

#include "core/bridge_driver.h"
#include <string>
#include <vector>

// Compile-time LoopbackNIC seeds and forwards tokens entirely on the FPGA.
// The two allocated CPU streams are disabled and the only MMIO word is done.
// The ordinary simplenic_t driver cannot service this ABI: it writes five
// absent configuration words and pushes data into a disabled incoming stream.
// Keep a distinct kind, and inherit the bridge's no-op lifecycle methods.
// Runtime +nic-loopbackN still uses simplenic_t with the normal hardware ABI.
class simplenic_loopback_t final : public bridge_driver_t {
public:
  inline static char KIND;
  simplenic_loopback_t(simif_t &sim, unsigned widget_index,
                      const std::vector<std::string> &)
      : bridge_driver_t(sim, &KIND), widget_index(widget_index) {}
  const unsigned widget_index;
};

#endif
