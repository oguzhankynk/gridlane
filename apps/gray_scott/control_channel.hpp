#ifndef GRIDLANE_APPS_GRAY_SCOTT_CONTROL_CHANNEL_HPP
#define GRIDLANE_APPS_GRAY_SCOTT_CONTROL_CHANNEL_HPP

// One control-plane connection: a pushseg inbox segment, a connection to the
// peer's inbox, a pairring channel over both, and the wireforge link of the
// generated interface on top. The same stack for the coordinator and the
// workers, instantiated with each unit's generated types.

#include <gridlane/glue/pairring_transport.hpp>
#include <gridlane/glue/pushseg_region.hpp>

#include <chrono>
#include <optional>
#include <pairring/pairring.hpp>
#include <pushseg/pushseg.hpp>

namespace gridlane::grayscott {

inline constexpr pairring::Config kControlRing{64, 256, 1024};
inline constexpr pushseg::SegNo kWorkerControlSegment = 2;      // in every worker process
inline constexpr pushseg::SegNo kCoordinatorControlBase = 100;  // + worker process index

template <typename Endpoint, typename Link, typename Handler>
class ControlChannel {
public:
  // Creates and publishes this side's inbox.
  ControlChannel(pushseg::Context& ctx, pushseg::SegNo inbox, Handler& handler)
      : ctx_(ctx),
        inbox_(ctx.create_segment(inbox, pairring::region_size(kControlRing))),
        endpoint_(handler) {
    inbox_.publish();
  }

  // Connects to the peer's inbox and starts the channel.
  void connect(pushseg::NodeId node, pushseg::SegKey key, std::chrono::milliseconds timeout) {
    out_ = ctx_.connect(node, key, timeout);
    region_.emplace(out_);
    channel_.emplace(glue::local_region(inbox_), *region_, kControlRing);
    transport_.emplace(*channel_);
    link_.emplace(*transport_, endpoint_.decoder(), endpoint_.dispatcher());
  }

  pairring::State poll() {
    return channel_->poll();
  }
  bool established() const {
    return channel_ && channel_->state() == pairring::State::kEstablished;
  }
  // Dispatches every message that has arrived; returns how many.
  std::size_t receive() {
    std::size_t n = 0;
    while (link_->poll().frames > 0) {
      ++n;
    }
    return n;
  }
  // Sends a message; retries while the peer's ring is full.
  template <typename Message>
  bool send(const Message& m, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      const wireforge::Status st = Endpoint::send(*link_, m);
      if (st == wireforge::Status::Ok) {
        return true;
      }
      // BufferTooSmall: the peer's ring is full, try again. Anything else
      // (e.g. a value outside the model's range) will not change.
      if (st != wireforge::Status::BufferTooSmall || std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      glue::wait_doorbell(inbox_, std::chrono::milliseconds(1));
      (void)channel_->poll();
    }
  }
  pushseg::LocalSegment& inbox() {
    return inbox_;
  }

private:
  pushseg::Context& ctx_;
  pushseg::LocalSegment inbox_;
  pushseg::Connection out_;
  std::optional<glue::PushsegRegion> region_;
  std::optional<pairring::Channel> channel_;
  std::optional<glue::PairringTransport> transport_;
  Endpoint endpoint_;
  std::optional<Link> link_;
};

}  // namespace gridlane::grayscott

#endif  // GRIDLANE_APPS_GRAY_SCOTT_CONTROL_CHANNEL_HPP
