#ifndef GRIDLANE_FABRIC_HPP
#define GRIDLANE_FABRIC_HPP

/// @file
/// @brief The data plane of one process: symmetric memory over pushseg.

#include "gridlane/layout.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <pushseg/pushseg.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace gridlane {

/// @brief A failure of the data plane (the layer below reported an error).
class Error : public std::runtime_error {
public:
  /// @brief Creates the error.
  /// @param what Description.
  explicit Error(const std::string& what) : std::runtime_error(what) {}
};

/// @brief A symmetric buffer: the same offset and size in every rank's area.
struct Buffer {
  std::size_t offset = 0;  ///< Offset inside a rank's area.
  std::size_t bytes = 0;   ///< Size per rank.
};

/// @brief The data plane of one process (application): its pushseg segment,
/// connections to the segments of every other process of the layout, and
/// symmetric buffers in them.
///
/// Every rank owns an area of `bytes_per_rank` bytes in the segment of its
/// process. alloc() carves buffers out of every area at once; called in the
/// same order in every process, it gives every buffer the same offset
/// everywhere, so a rank can address another rank's buffer without asking.
///
/// A rank reads only its own area (push only). put() and signal() write
/// another rank's area: a memory copy and an atomic store when the target rank
/// runs in this process, pushseg operations otherwise. Either way a signal
/// makes the puts before it visible.
///
/// Thread safety: put(), signal(), wait() and local() may be called from the
/// process's worker threads at the same time. alloc() and connect() belong to
/// setup, before the workers run.
class Fabric {
public:
  /// @brief pushseg segment number of the data segment.
  static constexpr pushseg::SegNo kDataSegment = 1;

  /// @brief Creates and publishes this process's segment.
  /// @param ctx pushseg context of this process; its (node, inst) must be in `layout`.
  /// @param layout The workers of the run; the same in every process.
  /// @param bytes_per_rank Size of each rank's area.
  Fabric(pushseg::Context& ctx, Layout layout, std::size_t bytes_per_rank);

  /// @brief Connects to the segment of every other process, waiting for each
  /// to be published. Throws pushseg::Error on timeout.
  /// @param timeout Maximum wait per process.
  void connect(std::chrono::milliseconds timeout);

  /// @brief Allocates a symmetric buffer (64-byte aligned). Throws
  /// std::length_error when the areas are full.
  /// @param bytes Size per rank.
  /// @return The buffer.
  Buffer alloc(std::size_t bytes);

  /// @brief The layout.
  /// @return Layout.
  const Layout& layout() const noexcept {
    return layout_;
  }
  /// @brief Index of this process in the layout.
  /// @return Process index.
  std::size_t self_process() const noexcept {
    return self_;
  }
  /// @brief Whether a rank runs in this process.
  /// @param r Rank.
  /// @return True if local.
  bool is_local(Rank r) const {
    return layout_.process_of(r) == self_;
  }

  /// @brief Writes into rank `dst`'s copy of `b`. No atomicity, no ordering
  /// until the next signal().
  /// @param dst Target rank.
  /// @param b Buffer.
  /// @param off Offset inside the buffer.
  /// @param src Source bytes.
  /// @param len Byte count.
  void put(Rank dst, const Buffer& b, std::size_t off, const void* src, std::size_t len);

  /// @brief Stores the 64-bit word `index` of rank `dst`'s copy of `b`
  /// atomically, after every earlier put() of this process to that rank.
  /// @param dst Target rank.
  /// @param b Buffer of 64-bit flags.
  /// @param index Word index.
  /// @param value Value.
  void signal(Rank dst, const Buffer& b, std::size_t index, std::uint64_t value);

  /// @brief Reads word `index` of rank `self`'s copy of `b` (acquire).
  /// @param self A rank of this process.
  /// @param b Buffer of 64-bit flags.
  /// @param index Word index.
  /// @return The value.
  std::uint64_t peek(Rank self, const Buffer& b, std::size_t index) const;

  /// @brief Waits until word `index` of rank `self`'s copy of `b` reaches `value`.
  /// @param self A rank of this process.
  /// @param b Buffer of 64-bit flags.
  /// @param index Word index.
  /// @param value Value to reach (the word only grows).
  void wait(Rank self, const Buffer& b, std::size_t index, std::uint64_t value) const;

  /// @brief Rank `self`'s copy of `b`, to read.
  /// @param self A rank of this process.
  /// @param b Buffer.
  /// @return Pointer to the first byte.
  void* local(Rank self, const Buffer& b) const;

private:
  std::size_t area_offset(Rank r) const;

  pushseg::Context& ctx_;
  Layout layout_;
  std::size_t self_;
  std::size_t per_rank_;
  std::size_t used_ = 0;
  pushseg::LocalSegment seg_;
  std::vector<pushseg::Connection> out_;  // by process index; empty for this process
};

/// @brief What a worker thread sees: its rank, the number of workers, and the
/// data plane bound to its rank.
class World {
public:
  /// @brief Binds a rank of this process to its fabric.
  /// @param fabric The process's fabric.
  /// @param rank A rank of this process.
  World(Fabric& fabric, Rank rank) : fabric_(&fabric), rank_(rank) {}

  /// @brief This worker's rank.
  /// @return Rank.
  Rank rank() const noexcept {
    return rank_;
  }
  /// @brief Number of workers.
  /// @return Size.
  Rank size() const noexcept {
    return fabric_->layout().size();
  }
  /// @brief See Fabric::put().
  /// @param dst Target rank.
  /// @param b Buffer.
  /// @param off Offset inside the buffer.
  /// @param src Source bytes.
  /// @param len Byte count.
  void put(Rank dst, const Buffer& b, std::size_t off, const void* src, std::size_t len) {
    fabric_->put(dst, b, off, src, len);
  }
  /// @brief See Fabric::signal().
  /// @param dst Target rank.
  /// @param b Buffer of flags.
  /// @param index Word index.
  /// @param value Value.
  void signal(Rank dst, const Buffer& b, std::size_t index, std::uint64_t value) {
    fabric_->signal(dst, b, index, value);
  }
  /// @brief Waits on this worker's own flag; see Fabric::wait().
  /// @param b Buffer of flags.
  /// @param index Word index.
  /// @param value Value to reach.
  void wait(const Buffer& b, std::size_t index, std::uint64_t value) const {
    fabric_->wait(rank_, b, index, value);
  }
  /// @brief This worker's copy of a buffer.
  /// @tparam T Element type.
  /// @param b Buffer.
  /// @return Typed pointer.
  template <typename T>
  T* local(const Buffer& b) const {
    return static_cast<T*>(fabric_->local(rank_, b));
  }
  /// @brief The process's fabric.
  /// @return Fabric.
  Fabric& fabric() const noexcept {
    return *fabric_;
  }

private:
  Fabric* fabric_;
  Rank rank_;
};

}  // namespace gridlane

#endif  // GRIDLANE_FABRIC_HPP
