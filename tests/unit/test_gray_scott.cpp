// The distributed Gray-Scott pipeline against the single-threaded C++ run:
// the fields must be identical (every cell is computed the same way, only the
// halos travel), the histograms equal, the sums equal within rounding (they
// are combined in another order).

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "cluster.hpp"
#include "pipeline.hpp"

namespace gridlane {
namespace {

using namespace gridlane::test;  // NOLINT(google-build-using-namespace)
namespace gs = gridlane::grayscott;

std::vector<double> read_doubles(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  const auto bytes = static_cast<std::size_t>(f.tellg());
  std::vector<double> out(bytes / sizeof(double));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(bytes));
  return out;
}

struct Scenario {
  const char* name;
  const char* backend;
  const char* layout;
  std::uint32_t sim;
  std::uint32_t analysis;
};

class GrayScottTest : public ::testing::TestWithParam<Scenario> {};

TEST_P(GrayScottTest, DistributedRunEqualsSerialRun) {
  const Scenario sc = GetParam();
  gs::Params p;
  p.width = 61;  // not divisible by the worker counts
  p.height = 47;
  p.steps = 230;
  p.snapshot_every = 50;
  p.seed = 11;

  Cluster c(sc.backend, sc.layout, gs::Pipeline::bytes_per_rank(p, sc.sim, sc.analysis));
  std::vector<std::unique_ptr<gs::Pipeline>> pipe;
  for (std::size_t i = 0; i < c.layout().processes(); ++i) {
    pipe.push_back(std::make_unique<gs::Pipeline>(c.fabric(i), p, sc.sim, sc.analysis));
  }
  c.connect();

  const std::string dir = ::testing::TempDir() + "gs_" + sc.name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  c.run([&](World& w) { (void)pipe[c.layout().process_of(w.rank())]->run(w, dir); });

  std::vector<double> su;
  std::vector<double> sv;
  const auto serial = gs::simulate_serial(p, su, sv);
  EXPECT_EQ(read_doubles(dir + "/U.bin"), su);
  EXPECT_EQ(read_doubles(dir + "/V.bin"), sv);

  std::ifstream f(dir + "/result.json");
  const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  for (const auto& s : serial) {
    std::string hist = "\"histogram\": [";
    for (std::size_t b = 0; b < gs::kBins; ++b) {
      hist += (b == 0 ? "" : ", ") + std::to_string(s.histogram[b]);
    }
    EXPECT_NE(json.find("{\"step\": " + std::to_string(s.step) + ","), std::string::npos);
    EXPECT_NE(json.find(hist + "]"), std::string::npos) << "histogram of step " << s.step;
  }
}

INSTANTIATE_TEST_SUITE_P(
    Scenarios,
    GrayScottTest,
    ::testing::Values(Scenario{"one_process", "shm", "0:0:2", 1, 1},
                      Scenario{"tcp_3_processes", "tcp", "0:0:3,1:0:2,1:1:2", 5, 2},
                      Scenario{"shm_2_processes", "shm", "0:0:4,0:1:3", 4, 3},
                      Scenario{"tcp_analysis_tree", "tcp", "0:0:2,1:0:5", 2, 5}),
    [](const ::testing::TestParamInfo<Scenario>& i) { return std::string(i.param.name); });

}  // namespace
}  // namespace gridlane
