// pairring channels over pushseg: the two layers composed through the region
// adapter, over TCP and POSIX shared memory.

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "test_util.hpp"

namespace gridlane {
namespace {

using namespace gridlane::test;  // NOLINT(google-build-using-namespace)

class GlueTest : public ::testing::TestWithParam<std::string> {};

TEST_P(GlueTest, ChannelOverPushseg) {
  const auto cfg = make_config(GetParam());
  ChannelEndpoint a(cfg, 0);
  ChannelEndpoint b(cfg, 1);
  a.connect(1);
  b.connect(0);
  ASSERT_TRUE(establish(a, b));

  constexpr int kCount = 2000;
  int received = 0;
  int sent = 0;
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  while (received < kCount && std::chrono::steady_clock::now() < deadline) {
    while (sent < kCount && a.channel->send(&sent, sizeof(sent))) {
      ++sent;
    }
    glue::wait_doorbell(b.inbox, 10ms);
    b.channel->drain([&](const void* p, std::size_t n) {
      int v = -1;
      ASSERT_EQ(n, sizeof(v));
      std::memcpy(&v, p, n);
      EXPECT_EQ(v, received);
      ++received;
    });
  }
  EXPECT_EQ(received, kCount);
}

INSTANTIATE_TEST_SUITE_P(Backends,
                         GlueTest,
                         ::testing::Values("tcp", "shm"),
                         [](const ::testing::TestParamInfo<std::string>& i) { return i.param; });

}  // namespace
}  // namespace gridlane
