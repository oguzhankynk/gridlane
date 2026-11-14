#ifndef GRIDLANE_GLUE_PUSHSEG_REGION_HPP
#define GRIDLANE_GLUE_PUSHSEG_REGION_HPP

/// @file
/// @brief Runs a pairring channel over pushseg: the channel's lower layer
/// contract mapped onto a pushseg connection and a local segment.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <pairring/region.hpp>
#include <pushseg/pushseg.hpp>

/// @brief gridlane: composition of the layers.
namespace gridlane {

/// @brief Adapters between layers that do not know each other.
namespace glue {

/// @brief pairring's RemoteRegion over a pushseg Connection.
///
/// The contracts line up one to one: pushseg's write is a bulk copy, its
/// 8-byte store is atomic and an ordering point (stronger than the release
/// store pairring needs), and its notify is queued at the owner, so it is
/// sticky as pairring requires.
class PushsegRegion final : public pairring::RemoteRegion {
public:
  /// @brief Wraps a connection to the peer's segment; the connection must outlive this object.
  /// @param conn Connection to the segment the peer receives on.
  explicit PushsegRegion(pushseg::Connection& conn) noexcept : conn_(conn) {}

  /// @brief Bulk copy into the peer's segment.
  /// @param off Offset.
  /// @param src Source.
  /// @param len Byte count.
  /// @return False if pushseg reports an error.
  bool write(std::uint64_t off, const void* src, std::size_t len) noexcept override {
    return conn_.write(off, src, len) == pushseg::Status::kOk;
  }

  /// @brief Atomic 8-byte store, an ordering point.
  /// @param off 8-aligned offset.
  /// @param value Value.
  /// @return False if pushseg reports an error.
  bool store64(std::uint64_t off, std::uint64_t value) noexcept override {
    return conn_.store<std::uint64_t>(off, value) == pushseg::Status::kOk;
  }

  /// @brief Notifies the segment's owner.
  /// @return False if pushseg reports an error.
  bool notify() noexcept override {
    return conn_.notify(0) == pushseg::Status::kOk;
  }

  /// @brief Size of the peer's segment.
  /// @return Bytes.
  std::size_t size() const noexcept override {
    return conn_.size();
  }

private:
  pushseg::Connection& conn_;
};

/// @brief The local region of a channel: a pushseg segment of this process.
/// @param seg Segment; at least pairring::region_size() bytes.
/// @return Region for the channel.
inline pairring::LocalRegion local_region(const pushseg::LocalSegment& seg) noexcept {
  return {seg.data(), seg.size()};
}

/// @brief Waits for the segment's next event (a doorbell, or a peer connecting
/// or leaving) and takes every event already queued behind it.
/// @param seg Segment.
/// @param timeout Maximum wait.
/// @return True if at least one event arrived.
inline bool wait_doorbell(pushseg::LocalSegment& seg, std::chrono::milliseconds timeout) {
  pushseg::Event ev;
  if (seg.wait_event(ev, timeout) != pushseg::Status::kOk) {
    return false;
  }
  while (seg.poll_event(ev)) {
  }
  return true;
}

}  // namespace glue
}  // namespace gridlane

#endif  // GRIDLANE_GLUE_PUSHSEG_REGION_HPP
