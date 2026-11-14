#ifndef GRIDLANE_LAYOUT_HPP
#define GRIDLANE_LAYOUT_HPP

/// @file
/// @brief Where the workers run: targets, applications, threads, and global ranks.

#include <cstddef>
#include <cstdint>
#include <pushseg/types.hpp>
#include <string>
#include <vector>

namespace gridlane {

/// @brief Global index of a worker (a thread), 0 .. Layout::size() - 1.
using Rank = std::uint32_t;

/// @brief One application (a process): the target it runs on, its instance
/// number there, and its worker threads.
struct ProcessSpec {
  pushseg::NodeId node = 0;   ///< Target (pushseg node).
  pushseg::InstId inst = 0;   ///< Instance of the application on the target.
  std::uint32_t threads = 1;  ///< Worker threads.
};

/// @brief The workers of a run. Ranks are numbered process by process, in
/// the order given, and thread by thread within a process.
///
/// Every process of a run must use the same layout.
class Layout {
public:
  /// @brief Builds a layout. Throws std::invalid_argument for an empty list, a
  /// process without threads, or two processes with the same (node, inst).
  /// @param processes Processes, in rank order.
  explicit Layout(std::vector<ProcessSpec> processes);

  /// @brief Parses "node:inst:threads,..." (e.g. "0:0:4,1:0:4").
  /// Throws std::invalid_argument on malformed input.
  /// @param text Layout string.
  /// @return The layout.
  static Layout parse(const std::string& text);

  /// @brief Number of workers.
  /// @return Total threads of all processes.
  Rank size() const noexcept {
    return size_;
  }
  /// @brief Number of processes.
  /// @return Process count.
  std::size_t processes() const noexcept {
    return processes_.size();
  }
  /// @brief A process.
  /// @param p Process index.
  /// @return Its spec.
  const ProcessSpec& process(std::size_t p) const {
    return processes_.at(p);
  }
  /// @brief First rank of a process.
  /// @param p Process index.
  /// @return Rank of its thread 0.
  Rank first_rank(std::size_t p) const {
    return first_.at(p);
  }
  /// @brief Process that runs a rank.
  /// @param r Rank.
  /// @return Process index.
  std::size_t process_of(Rank r) const;
  /// @brief Thread index of a rank within its process.
  /// @param r Rank.
  /// @return Local index.
  std::uint32_t local_index(Rank r) const {
    return r - first_rank(process_of(r));
  }
  /// @brief Index of the process (node, inst). Throws std::invalid_argument if absent.
  /// @param node Target.
  /// @param inst Instance.
  /// @return Process index.
  std::size_t find(pushseg::NodeId node, pushseg::InstId inst) const;

private:
  std::vector<ProcessSpec> processes_;
  std::vector<Rank> first_;
  Rank size_ = 0;
};

}  // namespace gridlane

#endif  // GRIDLANE_LAYOUT_HPP
