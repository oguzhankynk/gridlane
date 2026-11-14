#include "kernel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace gridlane::grayscott {

Rows rows_of(std::uint32_t height, std::uint32_t parts, std::uint32_t i) noexcept {
  const auto at = [&](std::uint32_t k) {
    return static_cast<std::uint32_t>(std::uint64_t{k} * height / parts);
  };
  return Rows{at(i), at(i + 1)};
}

double noise(std::uint64_t seed, std::uint64_t j) noexcept {
  std::uint64_t z = seed + (j + 1) * 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  z ^= z >> 31U;
  return static_cast<double>(z >> 11U) * 0x1.0p-53;
}

void initial_rows(const Params& p, Rows rows, double* u, double* v) {
  const std::uint32_t side = std::max<std::uint32_t>(1, std::min(p.width, p.height) / 8);
  const std::uint32_t x0 = (p.width - side) / 2;
  const std::uint32_t y0 = (p.height - side) / 2;
  std::size_t at = 0;
  for (std::uint32_t y = rows.begin; y < rows.end; ++y) {
    for (std::uint32_t x = 0; x < p.width; ++x, ++at) {
      const bool square = x >= x0 && x < x0 + side && y >= y0 && y < y0 + side;
      double uu = square ? 0.5 : 1.0;
      double vv = square ? 0.25 : 0.0;
      const std::uint64_t i = std::uint64_t{y} * p.width + x;
      uu += 0.02 * (noise(p.seed, 2 * i) - 0.5);
      vv += 0.02 * (noise(p.seed, 2 * i + 1) - 0.5);
      u[at] = std::clamp(uu, 0.0, 1.0);
      v[at] = std::clamp(vv, 0.0, 1.0);
    }
  }
}

void step(const Params& p,
          std::uint32_t rows,
          const double* u,
          const double* v,
          double* u_out,
          double* v_out) noexcept {
  const std::size_t w = p.width;
  for (std::size_t y = 1; y <= rows; ++y) {
    const std::size_t row = y * w;
    const std::size_t north = row - w;
    const std::size_t south = row + w;
    for (std::size_t x = 0; x < w; ++x) {
      const std::size_t west = x == 0 ? w - 1 : x - 1;
      const std::size_t east = x + 1 == w ? 0 : x + 1;
      const double uc = u[row + x];
      const double vc = v[row + x];
      const double lap_u =
          (((u[north + x] + u[south + x]) + u[row + west]) + u[row + east]) - 4.0 * uc;
      const double lap_v =
          (((v[north + x] + v[south + x]) + v[row + west]) + v[row + east]) - 4.0 * vc;
      const double uvv = (uc * vc) * vc;
      u_out[row + x] = uc + p.dt * ((p.Du * lap_u - uvv) + p.F * (1.0 - uc));
      v_out[row + x] = vc + p.dt * ((p.Dv * lap_v + uvv) - (p.F + p.k) * vc);
    }
  }
}

void add_to_histogram(const double* v, std::size_t n, Histogram& h) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    const double b = std::floor(v[i] * static_cast<double>(kBins));
    const auto bin = static_cast<std::size_t>(std::clamp(b, 0.0, static_cast<double>(kBins - 1)));
    ++h[bin];
  }
}

double sum(const double* x, std::size_t n) noexcept {
  double s = 0;
  for (std::size_t i = 0; i < n; ++i) {
    s += x[i];
  }
  return s;
}

bool is_snapshot_step(const Params& p, std::uint32_t step) noexcept {
  return step % p.snapshot_every == 0 || step == p.steps;
}

std::vector<Snapshot> simulate_serial(const Params& p,
                                      std::vector<double>& u,
                                      std::vector<double>& v) {
  const std::size_t w = p.width;
  const std::uint32_t h = p.height;
  // One band of all rows; its halos are its own last and first rows.
  std::vector<double> a_u((h + 2) * w);
  std::vector<double> a_v((h + 2) * w);
  std::vector<double> b_u((h + 2) * w);
  std::vector<double> b_v((h + 2) * w);
  initial_rows(p, Rows{0, h}, a_u.data() + w, a_v.data() + w);
  std::vector<Snapshot> out;
  for (std::uint32_t s = 1; s <= p.steps; ++s) {
    std::copy_n(a_u.data() + h * w, w, a_u.data());  // north halo = last row
    std::copy_n(a_v.data() + h * w, w, a_v.data());
    std::copy_n(a_u.data() + w, w, a_u.data() + (h + 1) * w);  // south halo = first row
    std::copy_n(a_v.data() + w, w, a_v.data() + (h + 1) * w);
    step(p, h, a_u.data(), a_v.data(), b_u.data(), b_v.data());
    std::swap(a_u, b_u);
    std::swap(a_v, b_v);
    if (is_snapshot_step(p, s)) {
      Snapshot snap;
      snap.step = s;
      snap.sum_u = sum(a_u.data() + w, h * w);
      snap.sum_v = sum(a_v.data() + w, h * w);
      add_to_histogram(a_v.data() + w, h * w, snap.histogram);
      out.push_back(snap);
    }
  }
  u.assign(a_u.begin() + static_cast<std::ptrdiff_t>(w),
           a_u.end() - static_cast<std::ptrdiff_t>(w));
  v.assign(a_v.begin() + static_cast<std::ptrdiff_t>(w),
           a_v.end() - static_cast<std::ptrdiff_t>(w));
  return out;
}

namespace {

void write_binary(const std::string& path, const std::vector<double>& x) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(x.data()),
          static_cast<std::streamsize>(x.size() * sizeof(double)));
  if (!f) {
    throw std::runtime_error("cannot write " + path);
  }
}

std::string num(double x) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.17g", x);
  return buf;
}

}  // namespace

void write_results(const std::string& dir,
                   const Params& p,
                   const std::vector<double>& u,
                   const std::vector<double>& v,
                   const std::vector<Snapshot>& snapshots,
                   const std::string& extra_json) {
  write_binary(dir + "/U.bin", u);
  write_binary(dir + "/V.bin", v);
  std::ostringstream j;
  j << "{\n  \"params\": {\"width\": " << p.width << ", \"height\": " << p.height
    << ", \"F\": " << num(p.F) << ", \"k\": " << num(p.k) << ", \"Du\": " << num(p.Du)
    << ", \"Dv\": " << num(p.Dv) << ", \"dt\": " << num(p.dt) << ", \"seed\": " << p.seed
    << ", \"steps\": " << p.steps << ", \"snapshot_every\": " << p.snapshot_every << "},\n"
    << "  \"fields\": {\"U\": \"U.bin\", \"V\": \"V.bin\"},\n  \"snapshots\": [";
  for (std::size_t i = 0; i < snapshots.size(); ++i) {
    const Snapshot& s = snapshots[i];
    j << (i == 0 ? "\n" : ",\n") << "    {\"step\": " << s.step << ", \"sum_u\": " << num(s.sum_u)
      << ", \"sum_v\": " << num(s.sum_v) << ", \"histogram\": [";
    for (std::size_t b = 0; b < kBins; ++b) {
      j << (b == 0 ? "" : ", ") << s.histogram[b];
    }
    j << "]}";
  }
  j << "\n  ]" << (extra_json.empty() ? "" : ",\n  " + extra_json) << "\n}\n";
  std::ofstream f(dir + "/result.json");
  f << j.str();
  if (!f) {
    throw std::runtime_error("cannot write " + dir + "/result.json");
  }
}

}  // namespace gridlane::grayscott
