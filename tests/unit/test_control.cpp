// The control plane end to end: wireforge codecs generated from
// model/project.yaml, over a pairring channel, over pushseg, over TCP and
// POSIX shared memory.

#include "gridlane/glue/pairring_transport.hpp"

#include <gtest/gtest.h>

#include <gridlane_control_coordinator/interfaces/control.hpp>
#include <gridlane_control_worker/interfaces/control.hpp>
#include <optional>
#include <string>

#include "test_util.hpp"

namespace gridlane {
namespace {

using namespace gridlane::test;  // NOLINT(google-build-using-namespace)

namespace cm = gridlane::control::coordinator;  // coordinator's messages
namespace wm = gridlane::control::worker;       // worker's messages
namespace ci = cm::control;                     // coordinator's interface
namespace wi = wm::control;                     // worker's interface

using CoordinatorLink = glue::MessageLink<glue::PairringTransport,
                                          ci::Decoder,
                                          ci::Dispatcher,
                                          ci::kMaxFrameSize,
                                          ci::kRxBufferBytes>;
using WorkerLink = glue::MessageLink<glue::PairringTransport,
                                     wi::Decoder,
                                     wi::Dispatcher,
                                     wi::kMaxFrameSize,
                                     wi::kRxBufferBytes>;

// Remembers what arrived; a real worker would act on it.
struct WorkerInbox final : wi::Handler {
  void onVersionQueryReceived(const wm::VersionQuery& m) noexcept override {
    query = m;
  }
  void onJobStartReceived(const wm::JobStart& m) noexcept override {
    job = m;
  }
  void onPhaseStartReceived(const wm::PhaseStart& m) noexcept override {
    phase = m;
  }
  void onHeartbeatReceived(const wm::Heartbeat& m) noexcept override {
    heartbeat = m;
  }
  void onShutdownReceived(const wm::Shutdown& m) noexcept override {
    shutdown = m;
  }
  std::optional<wm::VersionQuery> query;
  std::optional<wm::JobStart> job;
  std::optional<wm::PhaseStart> phase;
  std::optional<wm::Heartbeat> heartbeat;
  std::optional<wm::Shutdown> shutdown;
};

struct CoordinatorInbox final : ci::Handler {
  void onVersionReplyReceived(const cm::VersionReply& m) noexcept override {
    reply = m;
  }
  void onPhaseDoneReceived(const cm::PhaseDone& m) noexcept override {
    done = m;
  }
  void onProgressReceived(const cm::Progress& m) noexcept override {
    progress = m;
  }
  void onHeartbeatAckReceived(const cm::HeartbeatAck& m) noexcept override {
    ack = m;
  }
  void onResultSummaryReceived(const cm::ResultSummary& m) noexcept override {
    result = m;
  }
  std::optional<cm::VersionReply> reply;
  std::optional<cm::PhaseDone> done;
  std::optional<cm::Progress> progress;
  std::optional<cm::HeartbeatAck> ack;
  std::optional<cm::ResultSummary> result;
};

// Polls `link` (draining the endpoint's doorbells) until `arrived()` holds.
template <typename Link, typename Pred>
bool pump(Link& link, ChannelEndpoint& at, Pred arrived) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!arrived() && std::chrono::steady_clock::now() < deadline) {
    if (link.poll().frames == 0) {
      glue::wait_doorbell(at.inbox, 10ms);
    }
  }
  return arrived();
}

class ControlTest : public ::testing::TestWithParam<std::string> {};

TEST_P(ControlTest, VersionQueryHeartbeatsJobAndShutdown) {
  const auto cfg = make_config(GetParam());
  ChannelEndpoint coord(cfg, 0);
  ChannelEndpoint work(cfg, 1);
  coord.connect(1);
  work.connect(0);
  ASSERT_TRUE(establish(coord, work));

  CoordinatorInbox coord_in;
  WorkerInbox work_in;
  ci::Endpoint coord_ep(coord_in);
  wi::Endpoint work_ep(work_in);
  ASSERT_EQ(coord_ep.status(), wireforge::Status::Ok);
  ASSERT_EQ(work_ep.status(), wireforge::Status::Ok);
  glue::PairringTransport coord_tr(*coord.channel);
  glue::PairringTransport work_tr(*work.channel);
  CoordinatorLink coord_link(coord_tr, coord_ep.decoder(), coord_ep.dispatcher());
  WorkerLink work_link(work_tr, work_ep.decoder(), work_ep.dispatcher());

  // Version query and reply, matched by nonce.
  cm::VersionQuery q;
  q.protocol = {static_cast<std::uint8_t>(ci::kVersionMajor),
                static_cast<std::uint8_t>(ci::kVersionMinor),
                static_cast<std::uint8_t>(ci::kVersionPatch)};
  q.nonce = 0xC0FFEE;
  ASSERT_EQ(ci::Endpoint::send(coord_link, q), wireforge::Status::Ok);
  ASSERT_TRUE(pump(work_link, work, [&] { return work_in.query.has_value(); }));
  EXPECT_EQ(work_in.query->protocol.major, wi::kVersionMajor);

  wm::VersionReply r;
  r.protocol = work_in.query->protocol;
  r.software = {0, 1, 0};
  r.node = 1;
  r.app = 1;
  r.workers = 4;
  r.nonce = work_in.query->nonce;
  ASSERT_EQ(wi::Endpoint::send(work_link, r), wireforge::Status::Ok);
  ASSERT_TRUE(pump(coord_link, coord, [&] { return coord_in.reply.has_value(); }));
  EXPECT_EQ(coord_in.reply->nonce, 0xC0FFEEU);
  EXPECT_EQ(coord_in.reply->workers, 4U);

  // Heartbeats, each answered.
  for (std::uint32_t seq = 1; seq <= 5; ++seq) {
    cm::Heartbeat hb;
    hb.seq = seq;
    hb.sentNs = 1000U * seq;
    ASSERT_EQ(ci::Endpoint::send(coord_link, hb), wireforge::Status::Ok);
    ASSERT_TRUE(
        pump(work_link, work, [&] { return work_in.heartbeat && work_in.heartbeat->seq == seq; }));
    wm::HeartbeatAck a;
    a.seq = work_in.heartbeat->seq;
    a.sentNs = work_in.heartbeat->sentNs;
    a.workerNs = 7;
    ASSERT_EQ(wi::Endpoint::send(work_link, a), wireforge::Status::Ok);
    ASSERT_TRUE(pump(coord_link, coord, [&] { return coord_in.ack && coord_in.ack->seq == seq; }));
    EXPECT_EQ(coord_in.ack->sentNs, 1000U * seq);
  }

  // The model's ranges keep an invalid job from leaving the coordinator.
  cm::JobStart bad;
  bad.width = 2;  // min is 8
  EXPECT_EQ(ci::Endpoint::send(coord_link, bad), wireforge::Status::OutOfRange);

  cm::JobStart job;
  job.width = 512;
  job.height = 256;
  job.simWorkers = 6;
  job.analysisWorkers = 2;
  job.seed = 42;
  ASSERT_EQ(ci::Endpoint::send(coord_link, job), wireforge::Status::Ok);
  ASSERT_TRUE(pump(work_link, work, [&] { return work_in.job.has_value(); }));
  EXPECT_EQ(work_in.job->width, 512U);
  EXPECT_EQ(work_in.job->simWorkers, 6U);
  EXPECT_DOUBLE_EQ(work_in.job->F, 0.0367);  // the model's default
  EXPECT_EQ(work_in.job->seed, 42U);

  cm::Shutdown bye;
  ASSERT_EQ(ci::Endpoint::send(coord_link, bye), wireforge::Status::Ok);
  ASSERT_TRUE(pump(work_link, work, [&] { return work_in.shutdown.has_value(); }));
  EXPECT_EQ(work_in.shutdown->reason, wm::ShutdownReason::done);
}

// The largest message of the interface followed at once by another one: both
// must arrive (see glue::MessageLink).
TEST_P(ControlTest, FullSizeMessageFollowedByAnother) {
  const auto cfg = make_config(GetParam());
  ChannelEndpoint coord(cfg, 0);
  ChannelEndpoint work(cfg, 1);
  coord.connect(1);
  work.connect(0);
  ASSERT_TRUE(establish(coord, work));
  CoordinatorInbox coord_in;
  WorkerInbox work_in;
  ci::Endpoint coord_ep(coord_in);
  wi::Endpoint work_ep(work_in);
  glue::PairringTransport coord_tr(*coord.channel);
  glue::PairringTransport work_tr(*work.channel);
  CoordinatorLink coord_link(coord_tr, coord_ep.decoder(), coord_ep.dispatcher());
  WorkerLink work_link(work_tr, work_ep.decoder(), work_ep.dispatcher());

  cm::JobStart job;  // the largest message
  job.seed = 99;
  cm::Heartbeat hb;
  hb.seq = 7;
  ASSERT_EQ(ci::Endpoint::send(coord_link, job), wireforge::Status::Ok);
  ASSERT_EQ(ci::Endpoint::send(coord_link, hb), wireforge::Status::Ok);
  ASSERT_TRUE(pump(work_link, work, [&] { return work_in.job && work_in.heartbeat; }));
  EXPECT_EQ(work_in.job->seed, 99U);
  EXPECT_EQ(work_in.heartbeat->seq, 7U);
}

INSTANTIATE_TEST_SUITE_P(Backends,
                         ControlTest,
                         ::testing::Values("tcp", "shm"),
                         [](const ::testing::TestParamInfo<std::string>& i) { return i.param; });

}  // namespace
}  // namespace gridlane
