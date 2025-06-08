#pragma once

#include <string>

namespace enu {

struct Args {
  bool daemon = false;
  std::string path;
  double latency = 0.0;
  std::string user_host;

  static Args parse(int argc, const char* argv[]);
};

}
