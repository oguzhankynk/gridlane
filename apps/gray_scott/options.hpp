#ifndef GRIDLANE_APPS_GRAY_SCOTT_OPTIONS_HPP
#define GRIDLANE_APPS_GRAY_SCOTT_OPTIONS_HPP

// "--key value" command line options of the demonstrator programs.

#include <cstdint>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>

namespace gridlane::grayscott {

class Options {
public:
  Options(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      const std::string key = argv[i];
      if (key.rfind("--", 0) != 0 || i + 1 >= argc) {
        throw std::invalid_argument("expected --option value, got '" + key + "'");
      }
      values_[key.substr(2)] = argv[++i];
    }
  }
  bool has(const std::string& k) const {
    return values_.count(k) != 0;
  }
  std::string str(const std::string& k, const std::string& def = "") const {
    const auto it = values_.find(k);
    if (it != values_.end()) {
      return it->second;
    }
    if (def.empty()) {
      throw std::invalid_argument("missing option --" + k);
    }
    return def;
  }
  std::uint64_t num(const std::string& k, std::uint64_t def) const {
    return has(k) ? std::strtoull(values_.at(k).c_str(), nullptr, 10) : def;
  }
  double real(const std::string& k, double def) const {
    return has(k) ? std::strtod(values_.at(k).c_str(), nullptr) : def;
  }

private:
  std::map<std::string, std::string> values_;
};

}  // namespace gridlane::grayscott

#endif  // GRIDLANE_APPS_GRAY_SCOTT_OPTIONS_HPP
