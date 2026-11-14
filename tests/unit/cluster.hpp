#ifndef GRIDLANE_TESTS_UNIT_CLUSTER_HPP
#define GRIDLANE_TESTS_UNIT_CLUSTER_HPP

// Several gridlane processes inside the test process: one pushseg context and
// one fabric per process of the layout, and one thread per rank.

#include "gridlane/collective.hpp"
#include "gridlane/fabric.hpp"
#include "gridlane/layout.hpp"

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_util.hpp"

namespace gridlane::test {

class Cluster {
public:
  Cluster(const std::string& backend, const std::string& layout, std::size_t bytes_per_rank)
      : cfg_(make_config(backend)), layout_(Layout::parse(layout)) {
    for (std::size_t p = 0; p < layout_.processes(); ++p) {
      const ProcessSpec& spec = layout_.process(p);
      ctx_.push_back(std::make_unique<pushseg::Context>(cfg_, spec.node, "test", spec.inst));
      fabric_.push_back(std::make_unique<Fabric>(*ctx_.back(), layout_, bytes_per_rank));
    }
    coll_.resize(layout_.processes());
  }

  // Same calls in every process, as a real run makes them: the same offsets.
  Buffer alloc(std::size_t bytes) {
    Buffer b;
    for (auto& f : fabric_) {
      b = f->alloc(bytes);
    }
    return b;
  }

  // A collective of `group` in every process; returns its index for collective().
  std::size_t add_collective(const Group& group, std::size_t max_bytes) {
    for (std::size_t p = 0; p < fabric_.size(); ++p) {
      coll_[p].push_back(std::make_unique<Collective>(*fabric_[p], group, max_bytes));
    }
    return coll_[0].size() - 1;
  }

  // The collective `i` as seen by the process of `w`.
  Collective& collective(const World& w, std::size_t i) {
    return *coll_[layout_.process_of(w.rank())][i];
  }

  void connect() {
    for (auto& f : fabric_) {
      f->connect(5s);
    }
  }

  // Runs fn on every rank, one thread each.
  void run(const std::function<void(World&)>& fn) {
    std::vector<std::thread> threads;
    for (Rank r = 0; r < layout_.size(); ++r) {
      threads.emplace_back([this, r, &fn] {
        World w(*fabric_[layout_.process_of(r)], r);
        fn(w);
      });
    }
    for (auto& t : threads) {
      t.join();
    }
  }

  const Layout& layout() const {
    return layout_;
  }

  Fabric& fabric(std::size_t p) {
    return *fabric_[p];
  }

private:
  pushseg::Config cfg_;
  Layout layout_;
  std::vector<std::unique_ptr<pushseg::Context>> ctx_;
  std::vector<std::unique_ptr<Fabric>> fabric_;
  std::vector<std::vector<std::unique_ptr<Collective>>> coll_;  // per process
};

}  // namespace gridlane::test

#endif  // GRIDLANE_TESTS_UNIT_CLUSTER_HPP
