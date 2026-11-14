#include "gridlane/layout.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace gridlane {

Layout::Layout(std::vector<ProcessSpec> processes) : processes_(std::move(processes)) {
  if (processes_.empty()) {
    throw std::invalid_argument("layout: no processes");
  }
  std::set<std::pair<pushseg::NodeId, pushseg::InstId>> seen;
  for (const auto& p : processes_) {
    if (p.threads == 0) {
      throw std::invalid_argument("layout: a process needs at least one thread");
    }
    if (!seen.emplace(p.node, p.inst).second) {
      throw std::invalid_argument("layout: (node, inst) used twice");
    }
    first_.push_back(size_);
    size_ += p.threads;
  }
}

Layout Layout::parse(const std::string& text) {
  std::vector<ProcessSpec> out;
  std::istringstream in(text);
  std::string item;
  while (std::getline(in, item, ',')) {
    unsigned long node = 0;
    unsigned long inst = 0;
    unsigned long threads = 0;
    char c1 = 0;
    char c2 = 0;
    std::istringstream f(item);
    if (!(f >> node >> c1 >> inst >> c2 >> threads) || c1 != ':' || c2 != ':' || !f.eof() ||
        node > 0xFFFFU || inst > 0xFFU || threads > 0xFFFFU) {
      throw std::invalid_argument("layout: expected node:inst:threads, got '" + item + "'");
    }
    out.push_back(ProcessSpec{static_cast<pushseg::NodeId>(node),
                              static_cast<pushseg::InstId>(inst),
                              static_cast<std::uint32_t>(threads)});
  }
  return Layout(std::move(out));
}

std::size_t Layout::process_of(Rank r) const {
  if (r >= size_) {
    throw std::out_of_range("layout: rank out of range");
  }
  const auto it = std::upper_bound(first_.begin(), first_.end(), r);
  return static_cast<std::size_t>(it - first_.begin()) - 1;
}

std::size_t Layout::find(pushseg::NodeId node, pushseg::InstId inst) const {
  for (std::size_t p = 0; p < processes_.size(); ++p) {
    if (processes_[p].node == node && processes_[p].inst == inst) {
      return p;
    }
  }
  throw std::invalid_argument("layout: no process (node " + std::to_string(node) + ", inst " +
                              std::to_string(inst) + ")");
}

}  // namespace gridlane
