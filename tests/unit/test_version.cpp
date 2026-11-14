#include "gridlane/version.hpp"

#include <gtest/gtest.h>

namespace gridlane {
namespace {

TEST(VersionTest, ReturnsNonNullString) {
  ASSERT_NE(version(), nullptr);
  EXPECT_GT(std::string(version()).size(), 0U);
}

}  // namespace
}  // namespace gridlane
