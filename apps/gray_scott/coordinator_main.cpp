// The coordinator of the Gray-Scott demonstrator: queries the workers'
// versions, starts the job, sends heartbeats, follows the phases, collects the
// results and ends the run.
//
//   gridlane_coordinator --system FILE --layout L --sim S --analysis A
//                        [--node C] [--width W] [--height H] [--steps N]
//                        [--every K] [--seed S] [--heartbeat-ms M]
//                        [--report FILE] [--timeout-ms T]

#include <gridlane/layout.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <gridlane_control_coordinator/interfaces/control.hpp>
#include <iostream>
#include <memory>
#include <optional>
#include <pushseg/pushseg.hpp>
#include <string>
#include <vector>

#include "control_channel.hpp"
#include "options.hpp"

namespace {

namespace gs = gridlane::grayscott;
namespace cm = gridlane::control::coordinator;
namespace ci = cm::control;
using Clock = std::chrono::steady_clock;

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
}

// What one worker application sent.
struct WorkerInbox final : ci::Handler {
  void onVersionReplyReceived(const cm::VersionReply& m) noexcept override {
    reply = m;
  }
  void onPhaseDoneReceived(const cm::PhaseDone& m) noexcept override {
    phases.push_back(m);
  }
  void onProgressReceived(const cm::Progress& m) noexcept override {
    progress = m;
  }
  void onHeartbeatAckReceived(const cm::HeartbeatAck& m) noexcept override {
    acks.push_back(m);
  }
  void onResultSummaryReceived(const cm::ResultSummary& m) noexcept override {
    results.push_back(m);
  }
  std::optional<cm::VersionReply> reply;
  std::vector<cm::PhaseDone> phases;
  std::optional<cm::Progress> progress;
  std::vector<cm::HeartbeatAck> acks;
  std::vector<cm::ResultSummary> results;
};

using Channel = gs::ControlChannel<ci::Endpoint,
                                   gridlane::glue::MessageLink<gridlane::glue::PairringTransport,
                                                               ci::Decoder,
                                                               ci::Dispatcher,
                                                               ci::kMaxFrameSize,
                                                               ci::kRxBufferBytes>,
                                   WorkerInbox>;

const char* phase_name(cm::Phase p) {
  switch (p) {
    case cm::Phase::distribute:
      return "distribute";
    case cm::Phase::simulate:
      return "simulate";
    case cm::Phase::measure:
      return "measure";
    case cm::Phase::transfer:
      return "transfer";
    case cm::Phase::analyze:
      return "analyze";
    case cm::Phase::collect:
      return "collect";
  }
  return "?";
}

int run(const gs::Options& opt) {
  const pushseg::Config cfg = pushseg::Config::load(opt.str("system"));
  const gridlane::Layout layout = gridlane::Layout::parse(opt.str("layout"));
  const auto node = static_cast<pushseg::NodeId>(opt.num("node", 0));
  const std::chrono::milliseconds timeout(opt.num("timeout-ms", 30000));
  const std::chrono::milliseconds hb_period(opt.num("heartbeat-ms", 100));

  cm::JobStart job;
  job.width = static_cast<std::uint32_t>(opt.num("width", job.width));
  job.height = static_cast<std::uint32_t>(opt.num("height", job.height));
  job.steps = static_cast<std::uint32_t>(opt.num("steps", job.steps));
  job.snapshotEvery = static_cast<std::uint32_t>(opt.num("every", job.snapshotEvery));
  job.seed = opt.num("seed", 0);
  job.F = opt.real("F", job.F);
  job.k = opt.real("k", job.k);
  job.simWorkers = static_cast<std::uint16_t>(opt.num("sim", 1));
  job.analysisWorkers = static_cast<std::uint16_t>(opt.num("analysis", 1));
  if (std::uint32_t{job.simWorkers} + job.analysisWorkers != layout.size()) {
    std::cerr << "gridlane_coordinator: --sim + --analysis must equal the layout's "
              << layout.size() << " workers\n";
    return 2;
  }

  pushseg::Context ctx(cfg, node, "coordinator", 0);
  const pushseg::AppId worker_app = cfg.app_id("worker");
  std::vector<WorkerInbox> inbox(layout.processes());
  std::vector<std::unique_ptr<Channel>> ch;
  for (std::size_t p = 0; p < layout.processes(); ++p) {
    ch.push_back(std::make_unique<Channel>(
        ctx, static_cast<pushseg::SegNo>(gs::kCoordinatorControlBase + p), inbox[p]));
  }
  for (std::size_t p = 0; p < layout.processes(); ++p) {
    const auto& spec = layout.process(p);
    ch[p]->connect(
        spec.node, pushseg::SegKey{worker_app, spec.inst, gs::kWorkerControlSegment}, timeout);
  }
  const auto t_start = Clock::now();
  const auto deadline = t_start + timeout;
  const auto pump = [&] {
    std::size_t n = 0;
    for (auto& c : ch) {
      (void)c->poll();
      n += c->established() ? c->receive() : 0;
    }
    if (n == 0) {
      gridlane::glue::wait_doorbell(ch[0]->inbox(), std::chrono::milliseconds(1));
    }
  };
  const auto all = [&](auto pred) {
    for (std::size_t p = 0; p < ch.size(); ++p) {
      if (!pred(p)) {
        return false;
      }
    }
    return true;
  };

  // Control channels up.
  while (!all([&](std::size_t p) { return ch[p]->established(); }) && Clock::now() < deadline) {
    pump();
  }

  // Version query.
  for (std::size_t p = 0; p < ch.size(); ++p) {
    cm::VersionQuery q;
    q.protocol = {static_cast<std::uint8_t>(ci::kVersionMajor),
                  static_cast<std::uint8_t>(ci::kVersionMinor),
                  static_cast<std::uint8_t>(ci::kVersionPatch)};
    q.nonce = static_cast<std::uint32_t>(p);
    (void)ch[p]->send(q);
  }
  while (!all([&](std::size_t p) { return inbox[p].reply.has_value(); }) &&
         Clock::now() < deadline) {
    pump();
  }
  bool compatible = true;
  for (std::size_t p = 0; p < ch.size(); ++p) {
    const auto& r = inbox[p].reply;
    const bool ok = r && r->protocol.major == ci::kVersionMajor && r->nonce == p &&
                    r->workers == layout.process(p).threads;
    std::printf("worker %zu (node %u, inst %u): %s\n",
                p,
                unsigned{layout.process(p).node},
                unsigned{layout.process(p).inst},
                !r   ? "no reply"
                : ok ? "version ok"
                     : "incompatible");
    compatible = compatible && ok;
  }
  if (!compatible) {
    cm::Shutdown bye;
    bye.reason = cm::ShutdownReason::abort;
    for (auto& c : ch) {
      (void)c->send(bye);
    }
    return 1;
  }

  // Job.
  for (auto& c : ch) {
    if (!c->send(job)) {
      std::cerr << "gridlane_coordinator: job rejected (outside the model's ranges?)\n";
      return 2;
    }
  }

  // Heartbeats and progress until every rank has reported.
  std::uint32_t hb_seq = 0;
  auto next_hb = Clock::now();
  std::vector<std::uint64_t> rtt_ns;
  std::uint32_t last_progress = 0;
  const auto results = [&] {
    std::size_t n = 0;
    for (const auto& in : inbox) {
      n += in.results.size();
    }
    return n;
  };
  const auto run_deadline = Clock::now() + std::chrono::hours(1);
  while (results() < layout.size() && Clock::now() < run_deadline) {
    if (Clock::now() >= next_hb) {
      ++hb_seq;
      for (auto& c : ch) {
        cm::Heartbeat hb;
        hb.seq = hb_seq;
        hb.sentNs = now_ns();
        (void)c->send(hb);
      }
      next_hb += hb_period;
    }
    pump();
    const std::uint64_t now = now_ns();
    for (auto& in : inbox) {
      for (const auto& a : in.acks) {
        rtt_ns.push_back(now - a.sentNs);
      }
      in.acks.clear();
      if (in.progress && in.progress->step > last_progress) {
        last_progress = in.progress->step;
        std::printf("step %u/%u\n", last_progress, job.steps);
      }
      for (const auto& ph : in.phases) {
        if (ph.phase == cm::Phase::simulate || ph.phase == cm::Phase::distribute) {
          std::printf("  %s done in %.3f ms\n",
                      phase_name(ph.phase),
                      static_cast<double>(ph.durationNs) / 1e6);
        }
      }
      in.phases.clear();
    }
    std::fflush(stdout);
  }

  cm::Shutdown bye;
  bye.reason = results() == layout.size() ? cm::ShutdownReason::done : cm::ShutdownReason::abort;
  for (auto& c : ch) {
    (void)c->send(bye);
  }
  const double wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - t_start).count();

  std::vector<cm::ResultSummary> all_results;
  for (const auto& in : inbox) {
    all_results.insert(all_results.end(), in.results.begin(), in.results.end());
  }
  std::sort(all_results.begin(), all_results.end(), [](const auto& a, const auto& b) {
    return a.rank < b.rank;
  });
  std::sort(rtt_ns.begin(), rtt_ns.end());
  const auto rtt = [&](double q) {
    return rtt_ns.empty()
               ? 0.0
               : static_cast<double>(
                     rtt_ns[static_cast<std::size_t>(q * static_cast<double>(rtt_ns.size() - 1))]) /
                     1e3;
  };
  std::printf("done in %.1f ms; %zu heartbeats answered, round trip p50 %.1f us, max %.1f us\n",
              wall_ms,
              rtt_ns.size(),
              rtt(0.5),
              rtt(1.0));

  if (opt.has("report")) {
    std::ofstream f(opt.str("report"));
    f << "{\n  \"layout\": \"" << opt.str("layout") << "\",\n  \"link\": \"" << cfg.default_backend
      << "\",\n  \"sim_workers\": " << job.simWorkers
      << ",\n  \"analysis_workers\": " << job.analysisWorkers << ",\n  \"wall_ms\": " << wall_ms
      << ",\n  \"heartbeats\": {\"answered\": " << rtt_ns.size() << ", \"rtt_us_p50\": " << rtt(0.5)
      << ", \"rtt_us_max\": " << rtt(1.0) << "},\n  \"ranks\": [";
    for (std::size_t i = 0; i < all_results.size(); ++i) {
      const auto& r = all_results[i];
      char crc[16];
      std::snprintf(crc, sizeof(crc), "%08x", r.tileCrc);
      f << (i == 0 ? "\n" : ",\n") << "    {\"rank\": " << r.rank << ", \"tile_crc\": \"" << crc
        << "\", \"sim_ms\": " << static_cast<double>(r.simNs) / 1e6
        << ", \"exchange_ms\": " << static_cast<double>(r.exchangeNs) / 1e6 << "}";
    }
    f << "\n  ]\n}\n";
  }
  return bye.reason == cm::ShutdownReason::done ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(gridlane::grayscott::Options(argc, argv));
  } catch (const std::exception& e) {
    std::cerr << "gridlane_coordinator: " << e.what() << "\n";
    return 1;
  }
}
