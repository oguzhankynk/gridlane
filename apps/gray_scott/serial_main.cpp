// The Gray-Scott demonstrator on one thread, no communication: the C++ side
// of the comparison with the Python reference (python/compare.py).
//
//   gridlane_gray_scott_serial --out DIR [--width W] [--height H] [--steps N]
//                              [--every K] [--seed S]

#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "kernel.hpp"

int main(int argc, char** argv) {
  namespace gs = gridlane::grayscott;
  gs::Params p;
  std::string out;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string key = argv[i];
    const char* val = argv[i + 1];
    const auto u32 = [&] {
      return static_cast<std::uint32_t>(std::strtoul(val, nullptr, 10));
    };
    if (key == "--out") {
      out = val;
    } else if (key == "--width") {
      p.width = u32();
    } else if (key == "--height") {
      p.height = u32();
    } else if (key == "--steps") {
      p.steps = u32();
    } else if (key == "--every") {
      p.snapshot_every = u32();
    } else if (key == "--seed") {
      p.seed = std::strtoull(val, nullptr, 10);
    } else {
      std::cerr << "unknown option " << key << "\n";
      return 2;
    }
  }
  if (out.empty() || p.width < 8 || p.height < 8 || p.steps == 0 || p.snapshot_every == 0) {
    std::cerr << "usage: " << argv[0]
              << " --out DIR [--width W] [--height H] [--steps N] [--every K] [--seed S]\n";
    return 2;
  }
  try {
    std::vector<double> u;
    std::vector<double> v;
    const auto snaps = gs::simulate_serial(p, u, v);
    gs::write_results(out, p, u, v, snaps);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
