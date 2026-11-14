#ifndef GRIDLANE_GLUE_PAIRRING_TRANSPORT_HPP
#define GRIDLANE_GLUE_PAIRRING_TRANSPORT_HPP

/// @file
/// @brief Feeds wireforge's generated codecs from a pairring channel.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <pairring/channel.hpp>
#include <wireforge/link.hpp>
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

/// @brief A wireforge link for a message transport such as PairringTransport.
///
/// The generated `Link` alias sizes the receive buffer to the largest frame.
/// An unframed link treats a read that fills the buffer as possibly cut short
/// and drains the next read as its excess: right for a byte stream, but on a
/// message transport the next read is the next message, and both are lost.
/// One more byte of buffer keeps every frame of the interface shorter than the
/// buffer.
/// @tparam Transport The transport.
/// @tparam Decoder The interface's Decoder.
/// @tparam Dispatcher The interface's Dispatcher.
/// @tparam MaxFrame The interface's kMaxFrameSize.
/// @tparam RxBytes The interface's kRxBufferBytes.
template <typename Transport,
          typename Decoder,
          typename Dispatcher,
          std::size_t MaxFrame,
          std::size_t RxBytes>
using MessageLink =
    wireforge::Link<Transport, Decoder, Dispatcher, MaxFrame + 1, std::max(RxBytes, MaxFrame + 1)>;

}  // namespace glue
}  // namespace gridlane

#endif  // GRIDLANE_GLUE_PAIRRING_TRANSPORT_HPP
