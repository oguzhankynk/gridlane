// Groups and collective operations over an uneven layout (7 workers in 3
// processes on 2 targets), over TCP and POSIX shared memory.

#include "gridlane/collective.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "cluster.hpp"

namespace gridlane {
namespace {

using namespace gridlane::test;  // NOLINT(google-build-using-namespace)

constexpr const char* kLayout = "0:0:3,1:0:2,1:1:2";  // 7 workers

TEST(GroupTest, Basics) {
  const Group g({5, 2, 3});
  EXPECT_EQ(g.root(), 5U);
  EXPECT_EQ(g.index_of(3), 2U);
  EXPECT_EQ(g.index_of(9), 3U);
  EXPECT_FALSE(g.contains(0));
  EXPECT_EQ(Group::range(4, 3).member(2), 6U);
  EXPECT_THROW(Group({}), std::invalid_argument);
  EXPECT_THROW(Group({1, 1}), std::invalid_argument);
}

class CollectiveTest : public ::testing::TestWithParam<std::string> {};

// Nobody leaves a barrier before everyone has entered it.
TEST_P(CollectiveTest, Barrier) {
  Cluster c(GetParam(), kLayout, 1 << 16);
  const auto all = c.add_collective(Group::range(0, 7), 64);
  c.connect();
  constexpr int kRounds = 100;
  std::atomic<int> entered{0};
  std::atomic<int> early{0};
  c.run([&](World& w) {
    for (int round = 1; round <= kRounds; ++round) {
      ++entered;
      c.collective(w, all).barrier(w);
      if (entered.load() < round * 7) {
        ++early;
      }
      c.collective(w, all).barrier(w);  // keep the next round's increments out
    }
  });
  EXPECT_EQ(early.load(), 0);
}

TEST_P(CollectiveTest, BroadcastScatterGather) {
  Cluster c(GetParam(), kLayout, 1 << 16);
  const auto all = c.add_collective(Group::range(0, 7), 64);
  c.connect();
  std::atomic<int> bad{0};
  c.run([&](World& w) {
    Collective& coll = c.collective(w, all);
    for (std::uint64_t round = 1; round <= 50; ++round) {
      std::uint64_t b = w.rank() == 0 ? round * 11 : 0;
      coll.broadcast(w, &b, sizeof(b));
      bad += b == round * 11 ? 0 : 1;

      std::vector<std::uint64_t> parts(7);
      std::iota(parts.begin(), parts.end(), round * 100);
      std::uint64_t mine = 0;
      coll.scatter(w, parts.data(), &mine, sizeof(mine));
      bad += mine == round * 100 + w.rank() ? 0 : 1;

      std::vector<std::uint64_t> all_parts(7, 0);
      const std::uint64_t back = mine * 2;
      coll.gather(w, &back, all_parts.data(), sizeof(back));
      if (w.rank() == 0) {
        for (std::uint64_t i = 0; i < 7; ++i) {
          bad += all_parts[i] == (round * 100 + i) * 2 ? 0 : 1;
        }
      }
    }
  });
  EXPECT_EQ(bad.load(), 0);
}

TEST_P(CollectiveTest, AllreduceIsTheSameEverywhereAndOrdered) {
  Cluster c(GetParam(), kLayout, 1 << 16);
  const auto all = c.add_collective(Group::range(0, 7), 64);
  c.connect();
  std::vector<double> sums(7);
  std::vector<std::uint64_t> maxes(7);
  c.run([&](World& w) {
    Collective& coll = c.collective(w, all);
    // Values whose floating point sum depends on the order of addition.
    const double v = w.rank() % 2 == 0 ? 1e16 : 1.0;
    sums[w.rank()] = coll.allreduce(w, v, [](double a, double b) { return a + b; });
    maxes[w.rank()] =
        coll.allreduce(w, std::uint64_t{w.rank()} * 3, [](std::uint64_t a, std::uint64_t b) {
          return std::max(a, b);
        });
  });
  // Member order: ((((((1e16 + 1) + 1e16) + 1) + 1e16) + 1) + 1e16).
  double expected = 0;
  for (int r = 0; r < 7; ++r) {
    expected = r == 0 ? 1e16 : expected + (r % 2 == 0 ? 1e16 : 1.0);
  }
  for (Rank r = 0; r < 7; ++r) {
    EXPECT_EQ(sums[r], expected) << r;
    EXPECT_EQ(maxes[r], 18U) << r;
  }
}

// Disjoint groups work at the same time; a group's root need not be rank 0.
TEST_P(CollectiveTest, SubgroupsAtTheSameTime) {
  Cluster c(GetParam(), kLayout, 1 << 16);
  const auto pair_a = c.add_collective(Group({1, 0}), 64);
  const auto pair_b = c.add_collective(Group({3, 4}), 64);
  const auto trio = c.add_collective(Group({6, 2, 5}), 64);
  c.connect();
  std::vector<std::uint64_t> got(7, 0);
  c.run([&](World& w) {
    const auto pick = [&]() -> Collective& {
      if (w.rank() <= 1) {
        return c.collective(w, pair_a);
      }
      if (w.rank() == 3 || w.rank() == 4) {
        return c.collective(w, pair_b);
      }
      return c.collective(w, trio);
    };
    Collective& coll = pick();
    std::uint64_t total = 0;
    for (int i = 0; i < 100; ++i) {
      total += coll.allreduce(
          w, std::uint64_t{w.rank()}, [](std::uint64_t a, std::uint64_t b) { return a + b; });
    }
    got[w.rank()] = total;
  });
  const std::vector<std::uint64_t> expect = {100, 100, 1300, 700, 700, 1300, 1300};
  EXPECT_EQ(got, expect);
}

TEST_P(CollectiveTest, RejectsMisuse) {
  Cluster c(GetParam(), kLayout, 1 << 16);
  const auto pair = c.add_collective(Group({0, 1}), 8);
  c.connect();
  c.run([&](World& w) {
    Collective& coll = c.collective(w, pair);
    if (w.rank() == 2) {
      EXPECT_THROW(coll.barrier(w), std::invalid_argument);  // not a member
    }
    if (w.rank() <= 1) {
      std::uint64_t big[2] = {};
      EXPECT_THROW(coll.broadcast(w, big, sizeof(big)), std::length_error);
    }
  });
}

INSTANTIATE_TEST_SUITE_P(Backends,
                         CollectiveTest,
                         ::testing::Values("tcp", "shm"),
                         [](const ::testing::TestParamInfo<std::string>& i) { return i.param; });

}  // namespace
}  // namespace gridlane
