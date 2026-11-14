#include "gridlane/fabric.hpp"

#include <cstring>
#include <thread>
#include <utility>

namespace gridlane {
namespace {

constexpr std::size_t kAlign = 64;

std::size_t round_up(std::size_t n) {
  return (n + kAlign - 1) / kAlign * kAlign;
}

void check(pushseg::Status st, const char* what) {
  if (st != pushseg::Status::kOk) {
    throw Error(std::string("gridlane: ") + what + ": " + pushseg::to_string(st));
  }
}

}  // namespace

Fabric::Fabric(pushseg::Context& ctx, Layout layout, std::size_t bytes_per_rank)
    : ctx_(ctx),
      layout_(std::move(layout)),
      self_(layout_.find(ctx.self().node, ctx.self().inst)),
      per_rank_(round_up(bytes_per_rank)),
      out_(layout_.processes()) {
  if (per_rank_ == 0) {
    throw std::invalid_argument("fabric: bytes_per_rank must be > 0");
  }
  seg_ = ctx_.create_segment(kDataSegment, per_rank_ * layout_.process(self_).threads);
  seg_.publish();
}

void Fabric::connect(std::chrono::milliseconds timeout) {
  const pushseg::PeerId me = ctx_.self();
  for (std::size_t p = 0; p < layout_.processes(); ++p) {
    if (p == self_ || out_[p].valid()) {
      continue;
    }
    const ProcessSpec& spec = layout_.process(p);
    out_[p] = ctx_.connect(spec.node, pushseg::SegKey{me.app, spec.inst, kDataSegment}, timeout);
  }
}

Buffer Fabric::alloc(std::size_t bytes) {
  const std::size_t size = round_up(bytes);
  if (size > per_rank_ - used_) {
    throw std::length_error("fabric: rank area is full");
  }
  const Buffer b{used_, bytes};
  used_ += size;
  return b;
}

std::size_t Fabric::area_offset(Rank r) const {
  return static_cast<std::size_t>(layout_.local_index(r)) * per_rank_;
}

void Fabric::put(Rank dst, const Buffer& b, std::size_t off, const void* src, std::size_t len) {
  if (off > b.bytes || len > b.bytes - off) {
    throw std::out_of_range("fabric: put outside the buffer");
  }
  const std::size_t p = layout_.process_of(dst);
  const std::size_t at = area_offset(dst) + b.offset + off;
  if (p == self_) {
    std::memcpy(seg_.at<unsigned char>(at), src, len);
  } else {
    check(out_[p].write(at, src, len), "put");
  }
}

void Fabric::signal(Rank dst, const Buffer& b, std::size_t index, std::uint64_t value) {
  if ((index + 1) * sizeof(std::uint64_t) > b.bytes) {
    throw std::out_of_range("fabric: signal outside the buffer");
  }
  const std::size_t p = layout_.process_of(dst);
  const std::size_t at = area_offset(dst) + b.offset + index * sizeof(std::uint64_t);
  if (p == self_) {
    __atomic_store_n(seg_.at<std::uint64_t>(at), value, __ATOMIC_RELEASE);
  } else {
    check(out_[p].store<std::uint64_t>(at, value), "signal");
  }
}

std::uint64_t Fabric::peek(Rank self, const Buffer& b, std::size_t index) const {
  const std::size_t at = area_offset(self) + b.offset + index * sizeof(std::uint64_t);
  return pushseg::load<std::uint64_t>(seg_.at<std::uint64_t>(at));
}

void Fabric::wait(Rank self, const Buffer& b, std::size_t index, std::uint64_t value) const {
  // Spin briefly, then yield, then sleep: workers may outnumber the cores.
  for (std::uint64_t spins = 0; peek(self, b, index) < value; ++spins) {
    if (spins < 64) {
      continue;
    }
    if (spins < 4096) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
  }
}

void* Fabric::local(Rank self, const Buffer& b) const {
  return seg_.at<unsigned char>(area_offset(self) + b.offset);
}

}  // namespace gridlane
