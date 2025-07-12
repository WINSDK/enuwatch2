#pragma once

#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace enu {

struct Args {
  bool daemon = false;
  bool sequencer = false;
  fs::path path;
  double latency = 0.0;
  std::string user_host;

  static Args parse(int argc, const char* argv[]);
};

}
