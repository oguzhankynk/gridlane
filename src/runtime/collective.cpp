#include "gridlane/collective.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace gridlane {

Group::Group(std::vector<Rank> members) : members_(std::move(members)) {
  if (members_.empty()) {
    throw std::invalid_argument("group: no members");
  }
  if (std::set<Rank>(members_.begin(), members_.end()).size() != members_.size()) {
    throw std::invalid_argument("group: a rank appears twice");
  }
}

Group Group::range(Rank first, Rank count) {
  std::vector<Rank> m(count);
  for (Rank i = 0; i < count; ++i) {
    m[i] = first + i;
  }
  return Group(std::move(m));
}

std::size_t Group::index_of(Rank r) const noexcept {
  const auto it = std::find(members_.begin(), members_.end(), r);
  return static_cast<std::size_t>(it - members_.begin());
}

Collective::Collective(Fabric& fabric, Group group, std::size_t max_bytes)
    : group_(std::move(group)),
      max_bytes_(std::max<std::size_t>(max_bytes, 1)),
      arrive_(fabric.alloc(group_.size() * sizeof(std::uint64_t))),
      release_(fabric.alloc(sizeof(std::uint64_t))),
      up_(fabric.alloc(group_.size() * max_bytes_)),
      down_(fabric.alloc(max_bytes_)),
      seq_(group_.size(), 0) {
  for (std::size_t i = 0; i < group_.size(); ++i) {
    if (group_.member(i) >= fabric.layout().size()) {
      throw std::invalid_argument("collective: member outside the layout");
    }
  }
}

std::size_t Collective::index(const World& w) const {
  const std::size_t i = group_.index_of(w.rank());
  if (i == group_.size()) {
    throw std::invalid_argument("collective: caller is not a member");
  }
  return i;
}

void Collective::check_len(std::size_t len) const {
  if (len > max_bytes_) {
    throw std::length_error("collective: payload larger than max_bytes");
  }
}

void Collective::exchange(World& w,
                          const void* up,
                          std::size_t len,
                          const Reduce& combine,
                          void* down,
                          std::size_t down_len) {
  const std::size_t me = index(w);
  const std::uint64_t seq = ++seq_[me];
  const Rank root = group_.root();

  // Arrival: my part (if any) into my slot at the root, then my flag there.
  if (len > 0) {
    w.put(root, up_, me * max_bytes_, up, len);
  }
  w.signal(root, arrive_, me, seq);

  if (me == 0) {
    for (std::size_t i = 0; i < group_.size(); ++i) {
      w.wait(arrive_, i, seq);
    }
    const void* result = combine ? combine(w.local<unsigned char>(up_)) : nullptr;
    for (std::size_t i = 0; i < group_.size(); ++i) {
      const Rank m = group_.member(i);
      if (result != nullptr && down_len > 0) {
        w.put(m, down_, 0, result, down_len);
      }
      w.signal(m, release_, 0, seq);
    }
  }

  // Release: wait for the root, then take my part (if any).
  w.wait(release_, 0, seq);
  if (down != nullptr && down_len > 0) {
    std::memcpy(down, w.local<unsigned char>(down_), down_len);
  }
}

void Collective::barrier(World& w) {
  exchange(w, nullptr, 0, nullptr, nullptr, 0);
}

void Collective::broadcast(World& w, void* data, std::size_t len) {
  check_len(len);
  const bool root = index(w) == 0;
  exchange(
      w,
      root ? data : nullptr,
      root ? len : 0,
      [&](const unsigned char* parts) -> const void* { return parts; },
      data,
      len);
}

void Collective::gather(World& w, const void* mine, void* all, std::size_t len) {
  check_len(len);
  const bool root = index(w) == 0;
  exchange(
      w,
      mine,
      len,
      [&](const unsigned char* parts) -> const void* {
        if (root && all != nullptr) {
          for (std::size_t i = 0; i < group_.size(); ++i) {
            std::memcpy(static_cast<unsigned char*>(all) + i * len, parts + i * max_bytes_, len);
          }
        }
        return nullptr;
      },
      nullptr,
      0);
}

void Collective::scatter(World& w, const void* parts, void* mine, std::size_t len) {
  check_len(len);
  const std::size_t me = index(w);
  const std::uint64_t seq = ++seq_[me];
  const Rank root = group_.root();
  // Arrival without data: a member is ready once it took its previous part.
  w.signal(root, arrive_, me, seq);
  if (me == 0) {
    for (std::size_t i = 0; i < group_.size(); ++i) {
      w.wait(arrive_, i, seq);
    }
    for (std::size_t i = 0; i < group_.size(); ++i) {
      const Rank m = group_.member(i);
      w.put(m, down_, 0, static_cast<const unsigned char*>(parts) + i * len, len);
      w.signal(m, release_, 0, seq);
    }
  }
  w.wait(release_, 0, seq);
  std::memcpy(mine, w.local<unsigned char>(down_), len);
}

}  // namespace gridlane
