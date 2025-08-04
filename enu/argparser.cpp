#include "argparser.h"
#include "log.h"

#include <unistd.h>
#include <span>
#include <string>
#include <string_view>
#include <print>
#include <algorithm>


namespace enu {

Args Args::parse(int argc, const char* argv[]) {
  // Skip program path parameter.
  std::span<const char*> prog_args(argv + 1, argc - 1);

  auto strip_dash = [&](std::string_view &arg) -> bool {
    if (arg.starts_with("--")) {
      arg = arg.substr(2);
    } else if (arg.starts_with("-")) {
      arg = arg.substr(1);
    } else {
      return false;
    }
    return true;
  };

  auto read_flag = [&](std::string_view exp) -> bool {
    std::string_view arg = prog_args[0];

    if (!strip_dash(arg))
      return false;

    if (arg != exp)
      return false;

    prog_args = prog_args.subspan(1);
    return true;
  };

  std::string_view value;
  auto read_value = [&](std::string_view exp) -> bool {
    if (!read_flag(exp))
      return false;

    if (prog_args.empty()) {
      std::println(stderr, "flag --{} requires a value", exp);
      exit(1);
    }

    value = prog_args[0];
    prog_args = prog_args.subspan(1);
    return true;
  };

  auto read_target_host = [&] -> bool {
    std::string_view arg = prog_args[0];
    if (std::count(arg.begin(), arg.end(), '@') != 1)
      return false;
    value = arg;
    prog_args = prog_args.subspan(1);
    return true;
  };

  Args arg;
  while (prog_args.size() > 0) {
    if (read_flag("daemon")) {
      arg.daemon = true;
    } else if (read_value("path")) {
      arg.path = value;
    } else if (read_value("latency")) {
      arg.latency = std::stod(std::string(value));
    } else if (read_target_host()) {
      arg.user_host = value;
    } else {
      std::println(stderr, "invalid argument: '{}'", prog_args[0]);
      exit(1);
    }
  }

  if (!arg.daemon && !arg.sequencer && arg.user_host.empty()) {
      std::println(stderr, "missing user@hostname argument");
      exit(1);
  }

  return arg;
}

} // namespace enu
