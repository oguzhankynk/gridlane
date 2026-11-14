#include "pipeline.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <pairring/crc32c.hpp>
#include <stdexcept>

namespace gridlane::grayscott {
namespace {

using Clock = std::chrono::steady_clock;
using Sums = std::array<double, 2>;

std::uint64_t ns_since(Clock::time_point t0) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
}

std::size_t up64(std::size_t n) {
  return (n + 63) / 64 * 64;
}

std::uint32_t snapshot_count(const Params& p) {
  return p.steps / p.snapshot_every + (p.steps % p.snapshot_every != 0 ? 1U : 0U);
}

std::uint32_t max_rows(std::uint32_t height, std::uint32_t parts) {
  std::uint32_t m = 0;
  for (std::uint32_t i = 0; i < parts; ++i) {
    m = std::max(m, rows_of(height, parts, i).count());
  }
  return m;
}

std::uint32_t tree_levels(std::uint32_t parts) {
  std::uint32_t levels = 0;
  while ((1U << levels) < parts) {
    ++levels;
  }
  return levels;
}

// What a Collective of `members` with `max_bytes` allocates.
std::size_t collective_bytes(std::size_t members, std::size_t max_bytes) {
  return up64(members * 8) + up64(8) + up64(members * max_bytes) + up64(max_bytes);
}

}  // namespace

std::size_t Pipeline::bytes_per_rank(const Params& p, std::uint32_t sim, std::uint32_t analysis) {
  const std::size_t w = p.width;
  const std::size_t levels = std::max<std::uint32_t>(tree_levels(analysis), 1);
  return up64(2 * 2 * (max_rows(p.height, sim) + 2) * w * 8) + up64(8) + up64(2 * 8) +
         up64(max_rows(p.height, analysis) * w * 8) + up64(sim * 8) + up64(analysis * 8) +
         up64(levels * kBins * 8) + up64(levels * 8) + up64(2 * p.height * w * 8) + up64(sim * 8) +
         up64(snapshot_count(p) * kBins * 8) + up64(8) + collective_bytes(sim, sizeof(Sums)) +
         collective_bytes(sim + analysis, 8);
}

Pipeline::Pipeline(Fabric& fabric, const Params& p, std::uint32_t sim, std::uint32_t analysis)
    : p_(p),
      sim_(sim),
      analysis_(analysis),
      snapshots_(snapshot_count(p)),
      max_sim_rows_(max_rows(p.height, sim)),
      max_ana_rows_(max_rows(p.height, analysis)),
      levels_(tree_levels(analysis)) {
  if (sim == 0 || analysis == 0 || sim > p.height || analysis > p.height) {
    throw std::invalid_argument("pipeline: 1..height workers in each group");
  }
  if (fabric.layout().size() != sim + analysis) {
    throw std::invalid_argument("pipeline: the layout must have sim + analysis workers");
  }
  const std::size_t w = p.width;
  const std::size_t levels = std::max<std::uint32_t>(levels_, 1);
  field_ = fabric.alloc(2 * 2 * (max_sim_rows_ + 2) * w * 8);
  dist_flag_ = fabric.alloc(8);
  halo_flags_ = fabric.alloc(2 * 8);
  snap_ = fabric.alloc(max_ana_rows_ * w * 8);
  snap_flags_ = fabric.alloc(sim * 8);
  ack_flags_ = fabric.alloc(analysis * 8);
  tree_ = fabric.alloc(levels * kBins * 8);
  tree_flags_ = fabric.alloc(levels * 8);
  gather_ = fabric.alloc(2 * p.height * w * 8);
  gather_flags_ = fabric.alloc(sim * 8);
  hists_ = fabric.alloc(snapshots_ * kBins * 8);
  hists_flag_ = fabric.alloc(8);
  sim_group_ = std::make_unique<Collective>(fabric, Group::range(0, sim), sizeof(Sums));
  everyone_ = std::make_unique<Collective>(fabric, Group::range(0, sim + analysis), 8);
}

std::size_t Pipeline::field_offset(std::uint32_t parity, std::uint32_t f, std::uint32_t row) const {
  return ((std::size_t{parity} * 2 + f) * (max_sim_rows_ + 2) + row) * p_.width * 8;
}

double* Pipeline::field(const World& w,
                        std::uint32_t parity,
                        std::uint32_t f,
                        std::uint32_t row) const {
  return w.local<double>(field_) + field_offset(parity, f, row) / 8;
}

RankResult Pipeline::run(World& w, const std::string& out_dir, Observer* observer) {
  RankResult res;
  res.rank = w.rank();
  if (w.rank() < sim_) {
    const auto t0 = Clock::now();
    distribute(w);
    if (observer != nullptr) {
      observer->on_phase_done(w.rank(), kDistribute, 0, ns_since(t0));
    }
    simulate(w, observer, res);
  } else {
    analyse(w, observer);
  }
  collect(w, out_dir, res);
  return res;
}

// The first simulation rank makes the initial field and hands every
// simulation rank its band.
void Pipeline::distribute(World& w) {
  const std::size_t width = p_.width;
  if (w.rank() == 0) {
    double* u = w.local<double>(gather_);  // scratch until the final collection
    double* v = u + std::size_t{p_.height} * width;
    initial_rows(p_, Rows{0, p_.height}, u, v);
    for (Rank r = 0; r < sim_; ++r) {
      const Rows rows = rows_of(p_.height, sim_, r);
      const std::size_t bytes = std::size_t{rows.count()} * width * 8;
      w.put(r, field_, field_offset(0, 0, 1), u + rows.begin * width, bytes);
      w.put(r, field_, field_offset(0, 1, 1), v + rows.begin * width, bytes);
      w.signal(r, dist_flag_, 0, 1);
    }
  }
  w.wait(dist_flag_, 0, 1);
  exchange_halos(w, 0);  // halos of the initial field
}

// Sends this band's edge rows of `step`'s field to the neighbors' halos and
// waits for its own. A neighbor cannot be more than one step ahead: it needs
// these rows first. So two parities of buffers suffice.
void Pipeline::exchange_halos(World& w, std::uint32_t step) {
  const std::uint32_t i = w.rank();
  const std::uint32_t parity = step % 2;
  const std::uint32_t n = rows_of(p_.height, sim_, i).count();
  const Rank north = (i + sim_ - 1) % sim_;
  const Rank south = (i + 1) % sim_;
  const std::uint32_t north_rows = rows_of(p_.height, sim_, north).count();
  const std::size_t bytes = std::size_t{p_.width} * 8;
  for (std::uint32_t f = 0; f < 2; ++f) {  // my first row is the south halo of the band above
    w.put(north, field_, field_offset(parity, f, north_rows + 1), field(w, parity, f, 1), bytes);
  }
  w.signal(north, halo_flags_, 1, step + 1);
  for (std::uint32_t f = 0; f < 2; ++f) {  // my last row is the north halo of the band below
    w.put(south, field_, field_offset(parity, f, 0), field(w, parity, f, n), bytes);
  }
  w.signal(south, halo_flags_, 0, step + 1);
  w.wait(halo_flags_, 0, step + 1);
  w.wait(halo_flags_, 1, step + 1);
}

void Pipeline::simulate(World& w, Observer* obs, RankResult& res) {
  const std::uint32_t i = w.rank();
  const Rows rows = rows_of(p_.height, sim_, i);
  const std::size_t width = p_.width;
  const auto sim_start = Clock::now();
  std::uint32_t snapshot = 0;
  for (std::uint32_t s = 1; s <= p_.steps; ++s) {
    const std::uint32_t cur = (s - 1) % 2;
    const std::uint32_t next = s % 2;
    auto t = Clock::now();
    step(p_,
         rows.count(),
         field(w, cur, 0, 0),
         field(w, cur, 1, 0),
         field(w, next, 0, 0),
         field(w, next, 1, 0));
    const std::uint64_t step_ns = ns_since(t);
    res.sim_ns += step_ns;
    t = Clock::now();
    exchange_halos(w, s);
    res.exchange_ns += ns_since(t);
    if (obs != nullptr) {
      obs->on_progress(i, s, step_ns);
    }
    if (!is_snapshot_step(p_, s)) {
      continue;
    }
    ++snapshot;
    const double* u = field(w, next, 0, 1);
    const double* v = field(w, next, 1, 1);
    const std::size_t cells = std::size_t{rows.count()} * width;

    // Global sums, combined in member order.
    t = Clock::now();
    const Sums total =
        sim_group_->allreduce(w, Sums{sum(u, cells), sum(v, cells)}, [](Sums a, Sums b) {
          return Sums{a[0] + b[0], a[1] + b[1]};
        });
    if (i == 0) {
      Snapshot snap;
      snap.step = s;
      snap.sum_u = total[0];
      snap.sum_v = total[1];
      snapshots_out_.push_back(snap);
    }
    if (obs != nullptr) {
      obs->on_phase_done(i, kMeasure, s, ns_since(t));
    }

    // In transit: V rows to the analysis ranks, once they are done with the previous snapshot.
    t = Clock::now();
    for (std::uint32_t j = 0; j < analysis_; ++j) {
      w.wait(ack_flags_, j, snapshot - 1);
    }
    for (std::uint32_t j = 0; j < analysis_; ++j) {
      const Rows a = rows_of(p_.height, analysis_, j);
      const std::uint32_t first = std::max(rows.begin, a.begin);
      const std::uint32_t last = std::min(rows.end, a.end);
      if (first < last) {
        w.put(sim_ + j,
              snap_,
              std::size_t{first - a.begin} * width * 8,
              v + std::size_t{first - rows.begin} * width,
              std::size_t{last - first} * width * 8);
      }
      w.signal(sim_ + j, snap_flags_, i, snapshot);
    }
    if (obs != nullptr) {
      obs->on_phase_done(i, kTransfer, s, ns_since(t));
    }
  }
  const std::uint32_t fin = p_.steps % 2;
  const std::size_t cells = std::size_t{rows.count()} * width;
  pairring::Crc32c crc;
  crc.update(field(w, fin, 0, 1), cells * 8);
  crc.update(field(w, fin, 1, 1), cells * 8);
  res.tile_crc = crc.value();
  res.sum_u = sum(field(w, fin, 0, 1), cells);
  res.sum_v = sum(field(w, fin, 1, 1), cells);
  if (obs != nullptr) {
    obs->on_phase_done(i, kSimulate, p_.steps, ns_since(sim_start));
  }
}

void Pipeline::analyse(World& w, Observer* obs) {
  const std::uint32_t j = w.rank() - sim_;
  const Rows a = rows_of(p_.height, analysis_, j);
  const std::uint64_t* parts = w.local<std::uint64_t>(tree_);
  for (std::uint32_t k = 1; k <= snapshots_; ++k) {
    const auto t = Clock::now();
    for (std::uint32_t i = 0; i < sim_; ++i) {
      w.wait(snap_flags_, i, k);
    }
    Histogram h{};
    add_to_histogram(w.local<double>(snap_), std::size_t{a.count()} * p_.width, h);
    // Tree of pairs: at level l, j receives from j + 2^l when j is a multiple of 2^(l+1).
    bool sent = false;
    for (std::uint32_t l = 0; l < levels_ && !sent; ++l) {
      const std::uint32_t d = 1U << l;
      if (j % (2 * d) == 0) {
        if (j + d < analysis_) {
          w.wait(tree_flags_, l, k);
          for (std::size_t b = 0; b < kBins; ++b) {
            h[b] += parts[l * kBins + b];
          }
        }
      } else {
        w.put(sim_ + (j - d), tree_, std::size_t{l} * kBins * 8, h.data(), kBins * 8);
        w.signal(sim_ + (j - d), tree_flags_, l, k);
        sent = true;
      }
    }
    if (j == 0) {  // the total goes to the first simulation rank
      w.put(0, hists_, std::size_t{k - 1} * kBins * 8, h.data(), kBins * 8);
      w.signal(0, hists_flag_, 0, k);
    }
    for (std::uint32_t i = 0; i < sim_; ++i) {  // done with this snapshot
      w.signal(i, ack_flags_, j, k);
    }
    if (obs != nullptr) {
      obs->on_phase_done(w.rank(), kAnalyze, k, ns_since(t));
    }
  }
}

void Pipeline::collect(World& w, const std::string& out_dir, RankResult& res) {
  (void)res;
  const auto t = Clock::now();
  const std::size_t width = p_.width;
  const std::size_t grid = std::size_t{p_.height} * width;
  if (w.rank() < sim_) {
    const Rows rows = rows_of(p_.height, sim_, w.rank());
    const std::uint32_t fin = p_.steps % 2;
    const std::size_t bytes = std::size_t{rows.count()} * width * 8;
    w.put(0, gather_, std::size_t{rows.begin} * width * 8, field(w, fin, 0, 1), bytes);
    w.put(0, gather_, (grid + std::size_t{rows.begin} * width) * 8, field(w, fin, 1, 1), bytes);
    w.signal(0, gather_flags_, w.rank(), 1);
  }
  if (w.rank() == 0) {
    for (std::uint32_t i = 0; i < sim_; ++i) {
      w.wait(gather_flags_, i, 1);
    }
    w.wait(hists_flag_, 0, snapshots_);
    const std::uint64_t* hists = w.local<std::uint64_t>(hists_);
    for (std::size_t k = 0; k < snapshots_out_.size(); ++k) {
      std::copy_n(hists + k * kBins, kBins, snapshots_out_[k].histogram.begin());
    }
    if (!out_dir.empty()) {
      const double* all = w.local<double>(gather_);
      const std::vector<double> u(all, all + grid);
      const std::vector<double> v(all + grid, all + 2 * grid);
      write_results(out_dir,
                    p_,
                    u,
                    v,
                    snapshots_out_,
                    "\"workers\": {\"simulation\": " + std::to_string(sim_) +
                        ", \"analysis\": " + std::to_string(analysis_) + "}");
    }
  }
  // Nobody leaves while others may still write into its memory.
  everyone_->barrier(w);
  (void)t;
}

}  // namespace gridlane::grayscott
