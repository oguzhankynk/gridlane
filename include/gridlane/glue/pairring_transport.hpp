#ifndef GRIDLANE_GLUE_PAIRRING_TRANSPORT_HPP
#define GRIDLANE_GLUE_PAIRRING_TRANSPORT_HPP

/// @file
/// @brief Feeds wireforge's generated codecs from a pairring channel.

#include <cstddef>
#include <cstdint>
#include <pairring/channel.hpp>
#include <wireforge/span.hpp>

namespace gridlane {
namespace glue {

/// @brief A wireforge transport (non-blocking `send` / `receive`) over a pairring channel.
///
/// The channel carries whole messages, so the wireforge interface uses
/// `framing: none`: every receive() returns exactly one frame. send() is all
/// or nothing, as the channel is: 0 means "no room now, try again".
///
/// A frame longer than the receive buffer is left in the channel (receive()
/// returns 0). The control interface's largest frame always fits the link's
/// buffer; a longer one would come from a peer with another protocol, which
/// the version query rejects first.
class PairringTransport {
public:
  /// @brief Wraps a channel; the channel must outlive this object.
  /// @param channel The channel.
  explicit PairringTransport(pairring::Channel& channel) noexcept : channel_(channel) {}

  /// @brief Sends one frame, all or nothing.
  /// @param bytes The frame.
  /// @return bytes.size() if sent, 0 if the channel has no room or is not established.
  std::size_t send(wireforge::Span<const std::uint8_t> bytes) noexcept {
    return channel_.send(bytes.data(), bytes.size()) ? bytes.size() : 0;
  }

  /// @brief Receives one frame.
  /// @param buffer Destination.
  /// @return Frame length, or 0 if there is none.
  std::size_t receive(wireforge::Span<std::uint8_t> buffer) noexcept {
    return channel_.receive(buffer.data(), buffer.size());
  }

private:
  pairring::Channel& channel_;
};

}  // namespace glue
}  // namespace gridlane

#endif  // GRIDLANE_GLUE_PAIRRING_TRANSPORT_HPP
