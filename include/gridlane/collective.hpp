#ifndef GRIDLANE_COLLECTIVE_HPP
#define GRIDLANE_COLLECTIVE_HPP

/// @file
/// @brief Groups of workers and the operations they perform together.

#include "gridlane/fabric.hpp"
#include "gridlane/layout.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <type_traits>
#include <vector>

namespace gridlane {

/// @brief An ordered set of ranks. The first member is the group's root.
class Group {
public:
  /// @brief Builds a group. Throws std::invalid_argument if empty or with duplicates.
  /// @param members Ranks; the first one is the root.
  explicit Group(std::vector<Rank> members);
  /// @brief Ranks first .. first + count - 1.
  /// @param first First rank (the root).
  /// @param count Number of members.
  /// @return The group.
  static Group range(Rank first, Rank count);

  /// @brief Number of members.
  /// @return Size.
  std::size_t size() const noexcept {
    return members_.size();
  }
  /// @brief A member.
  /// @param i Member index.
  /// @return Its rank.
  Rank member(std::size_t i) const {
    return members_.at(i);
  }
  /// @brief The root (member 0).
  /// @return Its rank.
  Rank root() const noexcept {
    return members_.front();
  }
  /// @brief Member index of a rank.
  /// @param r Rank.
  /// @return Index, or size() if `r` is not a member.
  std::size_t index_of(Rank r) const noexcept;
  /// @brief Whether a rank is a member.
  /// @param r Rank.
  /// @return True if a member.
  bool contains(Rank r) const noexcept {
    return index_of(r) != size();
  }

private:
  std::vector<Rank> members_;
};

/// @brief Collective operations of one group: barrier, broadcast, scatter,
/// gather, allreduce.
///
/// Every operation is an arrival (each member writes to the root, with data
/// if the operation carries any) followed by a release (the root writes to each
/// member, with data if any). A member therefore returns only after the root
/// has used what it sent, and buffers can be reused at once. The root combines
/// values in member order, so a reduction gives the same result whatever the
/// order in which the members arrive.
///
/// Setup: construct it in every process, in the same order relative to other
/// allocations of the fabric (its buffers are symmetric). Every member must
/// make the same sequence of calls. Calls are blocking. A process's members may
/// call from their own threads at the same time.
class Collective {
public:
  /// @brief Allocates the group's buffers in `fabric`.
  /// @param fabric The process's fabric.
  /// @param group The members.
  /// @param max_bytes Largest payload of one member in one operation.
  Collective(Fabric& fabric, Group group, std::size_t max_bytes);

  /// @brief The group.
  /// @return Group.
  const Group& group() const noexcept {
    return group_;
  }

  /// @brief Returns when every member has entered.
  /// @param w The calling member.
  void barrier(World& w);

  /// @brief The root's `data` is copied into every member's `data`.
  /// @param w The calling member.
  /// @param data Root: source; others: destination.
  /// @param len Bytes, at most max_bytes.
  void broadcast(World& w, void* data, std::size_t len);

  /// @brief Member i receives part i of the root's `parts`.
  /// @param w The calling member.
  /// @param parts Root: size() parts of `len` bytes; ignored elsewhere.
  /// @param mine Destination of this member's part.
  /// @param len Bytes per part, at most max_bytes.
  void scatter(World& w, const void* parts, void* mine, std::size_t len);

  /// @brief The root receives every member's `mine`, in member order.
  /// @param w The calling member.
  /// @param mine This member's part.
  /// @param all Root: destination of size() parts of `len` bytes; ignored elsewhere.
  /// @param len Bytes per part, at most max_bytes.
  void gather(World& w, const void* mine, void* all, std::size_t len);

  /// @brief Every member gets op applied to all members' values, in member order.
  /// @tparam T Trivially copyable value type.
  /// @tparam Op Callable as `T op(T, T)`.
  /// @param w The calling member.
  /// @param value This member's value.
  /// @param op Combining operation.
  /// @return The combined value.
  template <typename T, typename Op>
  T allreduce(World& w, T value, Op op) {
    static_assert(std::is_trivially_copyable_v<T>, "allreduce: T must be trivially copyable");
    T result = value;
    exchange(
        w,
        &value,
        sizeof(T),
        [&](const unsigned char* parts) {
          T acc;
          std::memcpy(&acc, parts, sizeof(T));
          for (std::size_t i = 1; i < group_.size(); ++i) {
            T v;
            std::memcpy(&v, parts + i * max_bytes_, sizeof(T));
            acc = op(acc, v);
          }
          result = acc;
          return &result;
        },
        &result,
        sizeof(T));
    return result;
  }

private:
  using Reduce = std::function<const void*(const unsigned char*)>;

  std::size_t index(const World& w) const;
  void check_len(std::size_t len) const;
  // Arrival with `up` (len bytes, may be 0), then release: the root calls
  // `combine` with every member's part and sends what it returns (down_len
  // bytes) to every member, written to `down`.
  void exchange(World& w,
                const void* up,
                std::size_t len,
                const Reduce& combine,
                void* down,
                std::size_t down_len);

  Group group_;
  std::size_t max_bytes_;
  Buffer arrive_;                   // at the root: one flag per member
  Buffer release_;                  // at every member: one flag
  Buffer up_;                       // at the root: one part per member
  Buffer down_;                     // at every member: one part
  std::vector<std::uint64_t> seq_;  // per member; only that member's thread touches it
};

}  // namespace gridlane

#endif  // GRIDLANE_COLLECTIVE_HPP
