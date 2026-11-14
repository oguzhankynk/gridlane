// The data plane: layouts, symmetric buffers, put / signal / wait between
// ranks of one process and of different processes. Every "process" is a
// pushseg context inside the test process, with its own worker threads.

#include "gridlane/fabric.hpp"
#include "gridlane/layout.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "cluster.hpp"
#include "test_util.hpp"

namespace gridlane {
namespace {

using namespace gridlane::test;  // NOLINT(google-build-using-namespace)

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

TEST(LayoutTest, RanksFollowProcessesThenThreads) {
  const Layout l = Layout::parse("0:0:2,1:0:3,1:1:1");
  EXPECT_EQ(l.size(), 6U);
  EXPECT_EQ(l.processes(), 3U);
  EXPECT_EQ(l.first_rank(1), 2U);
  EXPECT_EQ(l.process_of(0), 0U);
  EXPECT_EQ(l.process_of(4), 1U);
  EXPECT_EQ(l.process_of(5), 2U);
  EXPECT_EQ(l.local_index(4), 2U);
  EXPECT_EQ(l.find(1, 1), 2U);
  EXPECT_THROW((void)l.process_of(6), std::out_of_range);
  EXPECT_THROW((void)l.find(2, 0), std::invalid_argument);
}

TEST(LayoutTest, RejectsBadInput) {
  for (const char* bad : {"", "0:0", "0:0:0", "0:0:1,0:0:2", "a:0:1", "0:0:1:"}) {
    EXPECT_THROW((void)Layout::parse(bad), std::invalid_argument) << bad;
  }
}

class FabricTest : public ::testing::TestWithParam<std::string> {};

// Each rank passes a value to the next one, round after round; one slot per
// round, so no value is overwritten before it is read.
TEST_P(FabricTest, RingAcrossAndWithinProcesses) {
  constexpr std::uint64_t kRounds = 200;
  Cluster c(GetParam(), "0:0:2,1:0:3,1:1:1", 4096);
  const Buffer data = c.alloc(kRounds * sizeof(std::uint64_t));
  const Buffer flag = c.alloc(sizeof(std::uint64_t));
  c.connect();

  std::atomic<int> bad{0};
  c.run([&](World& w) {
    const Rank next = (w.rank() + 1) % w.size();
    const Rank prev = (w.rank() + w.size() - 1) % w.size();
    for (std::uint64_t round = 1; round <= kRounds; ++round) {
      const std::uint64_t v = w.rank() * 1000000ULL + round;
      w.put(next, data, (round - 1) * sizeof(v), &v, sizeof(v));
      w.signal(next, flag, 0, round);
      w.wait(flag, 0, round);
      if (w.local<std::uint64_t>(data)[round - 1] != prev * 1000000ULL + round) {
        ++bad;
      }
    }
  });
  EXPECT_EQ(bad.load(), 0);
}

// Every rank writes a slot at every rank and raises its own flag there.
TEST_P(FabricTest, AllToAll) {
  Cluster c(GetParam(), "0:0:3,1:0:2,1:1:2", 4096);
  const Rank n = c.layout().size();
  const Buffer slots = c.alloc(n * sizeof(std::uint64_t));
  const Buffer flags = c.alloc(n * sizeof(std::uint64_t));
  c.connect();

  std::atomic<int> bad{0};
  c.run([&](World& w) {
    for (Rank d = 0; d < w.size(); ++d) {
      const std::uint64_t v = (w.rank() + 1) * 100ULL + d;
      w.put(d, slots, w.rank() * sizeof(v), &v, sizeof(v));
      w.signal(d, flags, w.rank(), 1);
    }
    for (Rank s = 0; s < w.size(); ++s) {
      w.wait(flags, s, 1);
      if (w.local<std::uint64_t>(slots)[s] != (s + 1) * 100ULL + w.rank()) {
        ++bad;
      }
    }
  });
  EXPECT_EQ(bad.load(), 0);
}

TEST_P(FabricTest, RejectsOutOfBufferAccess) {
  Cluster c(GetParam(), "0:0:1,1:0:1", 256);
  const Buffer b = c.alloc(64);
  EXPECT_THROW((void)c.alloc(256), std::length_error);
  c.connect();
  c.run([&](World& w) {
    const char x[8] = {};
    EXPECT_THROW(w.put(1 - w.rank(), b, 60, x, 8), std::out_of_range);
    EXPECT_THROW(w.signal(1 - w.rank(), b, 8, 1), std::out_of_range);
  });
}

INSTANTIATE_TEST_SUITE_P(Backends,
                         FabricTest,
                         ::testing::Values("tcp", "shm"),
                         [](const ::testing::TestParamInfo<std::string>& i) { return i.param; });

// The same eight workers deployed two ways compute the same result:
// 2 targets x 1 application x 4 threads over TCP, and
// 1 target x 2 applications x 4 threads over shared memory.
std::vector<std::uint64_t> sum_of_all(const std::string& backend, const std::string& layout) {
  Cluster c(backend, layout, 4096);
  const Rank n = c.layout().size();
  const Buffer slots = c.alloc(n * sizeof(std::uint64_t));
  const Buffer flags = c.alloc(n * sizeof(std::uint64_t));
  c.connect();
  std::vector<std::uint64_t> sums(n, 0);
  c.run([&](World& w) {
    const std::uint64_t mine = (w.rank() + 7) * (w.rank() + 3);
    for (Rank d = 0; d < w.size(); ++d) {
      w.put(d, slots, w.rank() * sizeof(mine), &mine, sizeof(mine));
      w.signal(d, flags, w.rank(), 1);
    }
    std::uint64_t sum = 0;
    for (Rank s = 0; s < w.size(); ++s) {
      w.wait(flags, s, 1);
      sum += w.local<std::uint64_t>(slots)[s];
    }
    sums[w.rank()] = sum;
  });
  return sums;
}

TEST(PortabilityTest, TwoTargetsOverTcpEqualOneTargetOverSharedMemory) {
  const auto over_tcp = sum_of_all("tcp", "0:0:4,1:0:4");
  const auto over_shm = sum_of_all("shm", "0:0:4,0:1:4");
  EXPECT_EQ(over_tcp, over_shm);
  EXPECT_EQ(over_tcp.size(), 8U);
}

}  // namespace
}  // namespace gridlane
