#ifndef GRIDLANE_TESTS_UNIT_TEST_UTIL_HPP
#define GRIDLANE_TESTS_UNIT_TEST_UTIL_HPP

// Shared by the tests: a pushseg configuration unique to the test process,
// and an endpoint that is a pushseg process plus a pairring channel to a peer.

#include "gridlane/glue/pushseg_region.hpp"

#include <chrono>
#include <optional>
#include <pairring/pairring.hpp>
#include <pushseg/pushseg.hpp>
#include <string>

#include <unistd.h>

namespace gridlane::test {

using namespace std::chrono_literals;

inline constexpr pushseg::SegNo kInbox = 1;
inline constexpr pairring::Config kRing{64, 256, 1024};

#if defined(__SANITIZE_THREAD__)
#define GRIDLANE_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define GRIDLANE_TEST_TSAN 1
#endif
#endif

// In these tests several "processes" share one test process. With "shm" the
// same memory is then mapped at two addresses: the owner's and the writer's.
// ThreadSanitizer tracks synchronization by address, so it misses every
// release/acquire pair that goes through two mappings and reports races that
// cannot happen. Under ThreadSanitizer the tests therefore use "loopmem": the
// same mapped-level path in pushseg's core, one address for the memory.
inline std::string effective_backend(const std::string& backend) {
#ifdef GRIDLANE_TEST_TSAN
  return backend == "shm" ? "loopmem" : backend;
#else
  return backend;
#endif
}

// Ports and names unique per test process: ctest runs tests in parallel.
inline pushseg::Config make_config(const std::string& requested) {
  const std::string backend = effective_backend(requested);
  const auto pid = static_cast<unsigned>(::getpid());
  const auto base = 10000U + (pid % 5000U) * 4U;
  const std::string prefix = "/gridlane-test-" + std::to_string(pid);
  pushseg::Config cfg;
  cfg.apps["test"] = 1;
  for (pushseg::NodeId n = 0; n < 2; ++n) {
    cfg.nodes[n]["tcp"] = "127.0.0.1:" + std::to_string(base + n);
    cfg.nodes[n]["shm"] = "";
    cfg.nodes[n]["loopmem"] = "";
  }
  cfg.backend_options["shm"] = {{"name_prefix", prefix}};
  cfg.default_backend = backend;
  return cfg;
}

// A pushseg process with an inbox segment, a connection to the peer's inbox,
// and a pairring channel over both.
struct ChannelEndpoint {
  ChannelEndpoint(const pushseg::Config& cfg, pushseg::NodeId self) : ctx(cfg, self, "test") {
    inbox = ctx.create_segment(kInbox, pairring::region_size(kRing));
    inbox.publish();
  }
  void connect(pushseg::NodeId peer) {
    out = ctx.connect(peer, "test", 0, kInbox, 5s);
    region.emplace(out);
    channel.emplace(glue::local_region(inbox), *region, kRing);
  }

  pushseg::Context ctx;
  pushseg::LocalSegment inbox;
  pushseg::Connection out;
  std::optional<glue::PushsegRegion> region;
  std::optional<pairring::Channel> channel;
};

// Polls both channels until both are established. Each side needs the other's
// acknowledgement, so both are polled every round.
inline bool establish(ChannelEndpoint& a, ChannelEndpoint& b) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto sa = a.channel->poll();
    const auto sb = b.channel->poll();
    if (sa == pairring::State::kEstablished && sb == pairring::State::kEstablished) {
      return true;
    }
    glue::wait_doorbell(a.inbox, 10ms);
    glue::wait_doorbell(b.inbox, 10ms);
  }
  return false;
}

}  // namespace gridlane::test

#endif  // GRIDLANE_TESTS_UNIT_TEST_UTIL_HPP
