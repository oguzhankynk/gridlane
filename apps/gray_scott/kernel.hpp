#ifndef GRIDLANE_APPS_GRAY_SCOTT_KERNEL_HPP
#define GRIDLANE_APPS_GRAY_SCOTT_KERNEL_HPP

// The arithmetic of the Gray-Scott demonstrator (docs/gray-scott.md), with no
// communication: initial field, one step on a band of rows, measures.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gridlane::grayscott {

struct Params {
  std::uint32_t width = 256;
  std::uint32_t height = 256;
  double F = 0.0367;
  double k = 0.0649;
  double Du = 0.16;
  double Dv = 0.08;
  double dt = 1.0;
  std::uint64_t seed = 0;
  std::uint32_t steps = 1000;
  std::uint32_t snapshot_every = 100;
};

inline constexpr std::size_t kBins = 64;
using Histogram = std::array<std::uint64_t, kBins>;

// Rows [begin, end) of part `i` of `parts`.
struct Rows {
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
  std::uint32_t count() const noexcept {
    return end - begin;
  }
};
Rows rows_of(std::uint32_t height, std::uint32_t parts, std::uint32_t i) noexcept;

// r(j) of the specification: a number in [0, 1) from the index alone.
double noise(std::uint64_t seed, std::uint64_t j) noexcept;

// Initial values of `rows`, written row by row (no halo).
void initial_rows(const Params& p, Rows rows, double* u, double* v);

// One step of a band. `u`, `v` hold rows + 2 rows of `width` cells: row 0 is
// the halo above (north), rows 1..rows the band, row rows + 1 the halo below.
// Writes rows 1..rows of `u_out`, `v_out`.
void step(const Params& p,
          std::uint32_t rows,
          const double* u,
          const double* v,
          double* u_out,
          double* v_out) noexcept;

// Adds n values of V to a histogram.
void add_to_histogram(const double* v, std::size_t n, Histogram& h) noexcept;

// Sums of n cells, added left to right.
double sum(const double* x, std::size_t n) noexcept;

// What a run records at every snapshot step.
struct Snapshot {
  std::uint32_t step = 0;
  double sum_u = 0;
  double sum_v = 0;
  Histogram histogram{};
};

// The whole run on one thread: the plain reference of the C++ side.
// Returns the snapshots; `u`, `v` receive the final fields.
std::vector<Snapshot> simulate_serial(const Params& p,
                                      std::vector<double>& u,
                                      std::vector<double>& v);

// Steps at which snapshots are taken: multiples of snapshot_every, and the last.
bool is_snapshot_step(const Params& p, std::uint32_t step) noexcept;

// Writes <dir>/U.bin, <dir>/V.bin (float64, row-major) and <dir>/result.json.
void write_results(const std::string& dir,
                   const Params& p,
                   const std::vector<double>& u,
                   const std::vector<double>& v,
                   const std::vector<Snapshot>& snapshots,
                   const std::string& extra_json = "");

}  // namespace gridlane::grayscott

#endif  // GRIDLANE_APPS_GRAY_SCOTT_KERNEL_HPP
