// A worker application of the Gray-Scott demonstrator: one process of the
// layout, running one or more worker threads.
//
//   gridlane_worker --system FILE --layout L --node N [--inst I]
//                   [--coordinator-node C] [--out DIR] [--timeout-ms T]
//
// The main thread serves the control plane (version query, job, heartbeats,
// shutdown) and forwards what the worker threads report; the worker threads
// run the pipeline on the data plane.

#include <gridlane/fabric.hpp>
#include <gridlane/version.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <exception>
#include <gridlane_control_worker/interfaces/control.hpp>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <pushseg/pushseg.hpp>
#include <thread>
#include <vector>

#include "control_channel.hpp"
#include "options.hpp"
#include "pipeline.hpp"

namespace {

namespace gs = gridlane::grayscott;
namespace wm = gridlane::control::worker;
namespace wi = wm::control;
using Clock = std::chrono::steady_clock;

struct Inbox final : wi::Handler {
  void onVersionQueryReceived(const wm::VersionQuery& m) noexcept override {
    query = m;
  }
  void onJobStartReceived(const wm::JobStart& m) noexcept override {
    job = m;
  }
  void onPhaseStartReceived(const wm::PhaseStart& /*m*/) noexcept override {}
  void onHeartbeatReceived(const wm::Heartbeat& m) noexcept override {
    heartbeats.push_back(m);
  }
  void onShutdownReceived(const wm::Shutdown& m) noexcept override {
    shutdown = m;
  }
  std::optional<wm::VersionQuery> query;
  std::optional<wm::JobStart> job;
  std::vector<wm::Heartbeat> heartbeats;
  std::optional<wm::Shutdown> shutdown;
};

// Worker threads report through a queue; the main thread sends. (A pairring
// channel has one sending thread.)
class Reporter final : public gs::Observer {
public:
  Reporter(gridlane::Rank first_local, std::uint32_t every) : first_(first_local), every_(every) {}

  void on_progress(gridlane::Rank rank, std::uint32_t step, std::uint64_t ns) override {
    if (rank == first_ && step % every_ == 0) {
      wm::Progress m;
      m.step = step;
      m.stepNs = ns;
      std::lock_guard<std::mutex> lk(mu_);
      progress_.push_back(m);
    }
  }
  void on_phase_done(gridlane::Rank rank,
                     std::uint8_t phase,
                     std::uint32_t step,
                     std::uint64_t ns) override {
    if (rank == first_) {
      wm::PhaseDone m;
      m.phase = static_cast<wm::Phase>(phase);
      m.step = step;
      m.durationNs = ns;
      std::lock_guard<std::mutex> lk(mu_);
      done_.push_back(m);
    }
  }
  template <typename Channel>
  void flush(Channel& ch) {
    std::deque<wm::Progress> progress;
    std::deque<wm::PhaseDone> done;
    {
      std::lock_guard<std::mutex> lk(mu_);
      progress.swap(progress_);
      done.swap(done_);
    }
    for (const auto& m : progress) {
      (void)ch.send(m);
    }
    for (const auto& m : done) {
      (void)ch.send(m);
    }
  }

private:
  gridlane::Rank first_;
  std::uint32_t every_;
  std::mutex mu_;
  std::deque<wm::Progress> progress_;
  std::deque<wm::PhaseDone> done_;
};

gs::Params params_of(const wm::JobStart& j) {
  gs::Params p;
  p.width = j.width;
  p.height = j.height;
  p.steps = j.steps;
  p.snapshot_every = j.snapshotEvery;
  p.F = j.F;
  p.k = j.k;
  p.Du = j.Du;
  p.Dv = j.Dv;
  p.dt = j.dt;
  p.seed = j.seed;
  return p;
}

// One line per milestone on stderr: the launcher keeps it in worker-*.log.
void log(const char* what) {
  static const auto t0 = Clock::now();
  std::fprintf(stderr,
               "[%8.3f ms] %s\n",
               std::chrono::duration<double, std::milli>(Clock::now() - t0).count(),
               what);
}

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
}

int run(const gs::Options& opt) {
  const pushseg::Config cfg = pushseg::Config::load(opt.str("system"));
  const gridlane::Layout layout = gridlane::Layout::parse(opt.str("layout"));
  const auto node = static_cast<pushseg::NodeId>(opt.num("node", 0));
  const auto inst = static_cast<pushseg::InstId>(opt.num("inst", 0));
  const auto coord_node = static_cast<pushseg::NodeId>(opt.num("coordinator-node", 0));
  const std::string out = opt.has("out") ? opt.str("out") : "";
  const std::chrono::milliseconds timeout(opt.num("timeout-ms", 30000));
  const std::size_t self = layout.find(node, inst);
  const gridlane::Rank first = layout.first_rank(self);
  const std::uint32_t threads = layout.process(self).threads;

  pushseg::Context ctx(cfg, node, "worker", inst);
  Inbox inbox;
  gs::ControlChannel<wi::Endpoint,
                     gridlane::glue::MessageLink<gridlane::glue::PairringTransport,
                                                 wi::Decoder,
                                                 wi::Dispatcher,
                                                 wi::kMaxFrameSize,
                                                 wi::kRxBufferBytes>,
                     Inbox>
      ctrl(ctx, gs::kWorkerControlSegment, inbox);
  log("started");
  ctrl.connect(coord_node,
               pushseg::SegKey{cfg.app_id("coordinator"),
                               0,
                               static_cast<pushseg::SegNo>(gs::kCoordinatorControlBase + self)},
               timeout);

  std::unique_ptr<gridlane::Fabric> fabric;
  std::unique_ptr<gs::Pipeline> pipeline;
  std::unique_ptr<Reporter> reporter;
  std::vector<std::thread> workers;
  std::vector<gs::RankResult> results(threads);
  std::atomic<std::uint32_t> finished{0};
  bool reported = false;
  const auto deadline = Clock::now() + std::chrono::hours(24);

  while (!inbox.shutdown && Clock::now() < deadline) {
    (void)ctrl.poll();
    if (!ctrl.established()) {
      gridlane::glue::wait_doorbell(ctrl.inbox(), std::chrono::milliseconds(5));
      continue;
    }
    if (ctrl.receive() == 0) {
      gridlane::glue::wait_doorbell(ctrl.inbox(), std::chrono::milliseconds(2));
    }
    for (const auto& hb : inbox.heartbeats) {
      wm::HeartbeatAck a;
      a.seq = hb.seq;
      a.sentNs = hb.sentNs;
      a.workerNs = now_ns();
      (void)ctrl.send(a);
    }
    inbox.heartbeats.clear();

    if (inbox.query) {
      wm::VersionReply r;
      r.protocol = {static_cast<std::uint8_t>(wi::kVersionMajor),
                    static_cast<std::uint8_t>(wi::kVersionMinor),
                    static_cast<std::uint8_t>(wi::kVersionPatch)};
      unsigned major = 0;
      unsigned minor = 0;
      unsigned patch = 0;
      (void)std::sscanf(gridlane::version(), "%u.%u.%u", &major, &minor, &patch);
      r.software = {static_cast<std::uint8_t>(major),
                    static_cast<std::uint8_t>(minor),
                    static_cast<std::uint8_t>(patch)};
      r.node = node;
      r.app = ctx.self().app;
      r.inst = inst;
      r.workers = static_cast<std::uint16_t>(threads);
      r.nonce = inbox.query->nonce;
      (void)ctrl.send(r);
      inbox.query.reset();
      log("version query answered");
    }

    if (inbox.job && !fabric) {
      const gs::Params p = params_of(*inbox.job);
      const std::uint32_t sim = inbox.job->simWorkers;
      const std::uint32_t ana = inbox.job->analysisWorkers;
      log("job received");
      fabric = std::make_unique<gridlane::Fabric>(
          ctx, layout, gs::Pipeline::bytes_per_rank(p, sim, ana));
      pipeline = std::make_unique<gs::Pipeline>(*fabric, p, sim, ana);
      reporter = std::make_unique<Reporter>(first, p.snapshot_every);
      log("data segment published; connecting to the other applications");
      fabric->connect(timeout);
      log("data plane connected; starting the workers");
      for (std::uint32_t t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
          gridlane::World w(*fabric, first + t);
          results[t] = pipeline->run(w, out, reporter.get());
          ++finished;
        });
      }
    }
    if (reporter) {
      reporter->flush(ctrl);
    }
    if (fabric && !reported && finished.load() == threads) {
      for (auto& t : workers) {
        t.join();
      }
      reporter->flush(ctrl);
      for (const auto& r : results) {
        wm::ResultSummary s;
        s.rank = static_cast<std::uint16_t>(r.rank);
        s.tileCrc = r.tile_crc;
        s.sumU = r.sum_u;
        s.sumV = r.sum_v;
        s.simNs = r.sim_ns;
        s.exchangeNs = r.exchange_ns;
        (void)ctrl.send(s);
      }
      reported = true;
      log("workers done; results sent");
    }
  }
  for (auto& t : workers) {
    if (t.joinable()) {
      t.join();
    }
  }
  return inbox.shutdown && inbox.shutdown->reason == wm::ShutdownReason::done ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(gridlane::grayscott::Options(argc, argv));
  } catch (const std::exception& e) {
    std::cerr << "gridlane_worker: " << e.what() << "\n";
    return 1;
  }
}
