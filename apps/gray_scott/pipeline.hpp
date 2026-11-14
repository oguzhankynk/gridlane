#ifndef GRIDLANE_APPS_GRAY_SCOTT_PIPELINE_HPP
#define GRIDLANE_APPS_GRAY_SCOTT_PIPELINE_HPP

// The distributed Gray-Scott in situ pipeline (docs/gray-scott.md) on the
// gridlane data plane. Ranks [0, S) simulate, ranks [S, S + A) analyse.
//
// Per process: construct one Pipeline (it allocates symmetric buffers and
// collectives, so in the same order in every process, before connect()), then
// call run() from every local rank's thread.

#include <gridlane/collective.hpp>
#include <gridlane/fabric.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "kernel.hpp"

namespace gridlane::grayscott {

// Hooks for the control plane: called from worker threads.
struct Observer {
  virtual ~Observer() = default;
  // A simulation worker finished `step`; `step_ns` is its compute time.
  virtual void on_progress(Rank /*rank*/, std::uint32_t /*step*/, std::uint64_t /*step_ns*/) {}
  // A worker finished a phase (the values of control::Phase).
  virtual void on_phase_done(Rank /*rank*/,
                             std::uint8_t /*phase*/,
                             std::uint32_t /*step*/,
                             std::uint64_t /*ns*/) {}
};

// Phase numbers, as in model/project.yaml.
enum PhaseId : std::uint8_t {
  kDistribute = 1,
  kSimulate = 2,
  kMeasure = 3,
  kTransfer = 4,
  kAnalyze = 5,
  kCollect = 6,
};

// What one rank reports at the end (control message ResultSummary).
struct RankResult {
  Rank rank = 0;
  std::uint32_t tile_crc = 0;  // CRC-32C of the rank's final U and V rows (simulation ranks)
  double sum_u = 0;
  double sum_v = 0;
  std::uint64_t sim_ns = 0;       // computing steps
  std::uint64_t exchange_ns = 0;  // halo exchange and waits
};

class Pipeline {
public:
  // Bytes each rank's area needs for these parameters.
  static std::size_t bytes_per_rank(const Params& p, std::uint32_t sim, std::uint32_t analysis);

  Pipeline(Fabric& fabric, const Params& p, std::uint32_t sim, std::uint32_t analysis);

  // Runs the whole pipeline for rank `w`. The first simulation rank collects
  // the final fields and, if `out_dir` is not empty, writes the results there.
  RankResult run(World& w, const std::string& out_dir, Observer* observer = nullptr);

private:
  void distribute(World& w);
  void exchange_halos(World& w, std::uint32_t step);
  void simulate(World& w, Observer* obs, RankResult& res);
  void analyse(World& w, Observer* obs);
  void collect(World& w, const std::string& out_dir, RankResult& res);

  // Byte offset of row `row` (0 = north halo) of field f (0 = U, 1 = V) at `parity`.
  std::size_t field_offset(std::uint32_t parity, std::uint32_t f, std::uint32_t row) const;
  double* field(const World& w, std::uint32_t parity, std::uint32_t f, std::uint32_t row) const;

  Params p_;
  std::uint32_t sim_;
  std::uint32_t analysis_;
  std::uint32_t snapshots_;
  std::uint32_t max_sim_rows_;
  std::uint32_t max_ana_rows_;
  std::uint32_t levels_;  // of the histogram tree

  Buffer field_;         // [parity][U|V][(rows + 2) * width]
  Buffer dist_flag_;     // simulation: initial band arrived
  Buffer halo_flags_;    // [north, south]: step whose halo arrived
  Buffer snap_;          // analysis: V rows of the current snapshot
  Buffer snap_flags_;    // analysis: per simulation rank, snapshot number
  Buffer ack_flags_;     // simulation: per analysis rank, snapshot number done
  Buffer tree_;          // analysis: [level][kBins]
  Buffer tree_flags_;    // analysis: per level, snapshot number
  Buffer gather_;        // first simulation rank: U then V, whole grid
  Buffer gather_flags_;  // first simulation rank: per simulation rank
  Buffer hists_;         // first simulation rank: [snapshot][kBins] from the analysis root
  Buffer hists_flag_;
  std::unique_ptr<Collective> sim_group_;
  std::unique_ptr<Collective> everyone_;

  // Written by the first simulation rank, read when it writes the results.
  std::vector<Snapshot> snapshots_out_;
};

}  // namespace gridlane::grayscott

#endif  // GRIDLANE_APPS_GRAY_SCOTT_PIPELINE_HPP
