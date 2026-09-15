#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "app.hpp"
#include "log.hpp"
#include "version.hpp"

namespace {

void print_usage(const char* program) {
  std::cout
      << "USAGE: " << program << " [options]\n"
      << "\n"
      << "AES67 <-> SIP intercom gateway: bridges AES67/Ravenna intercom\n"
      << "endpoints (through the Merging RAVENNA ALSA device driven by\n"
      << "aes67-daemon) to SIP calls on an off-site PBX.\n"
      << "\n"
      << "Options:\n"
      << "  -c <file>   configuration file (default /etc/aes67-sip.conf)\n"
      << "  -a <addr>   override the HTTP bind address\n"
      << "  -p <port>   override the HTTP port\n"
      << "  -f          force fake mode: null audio, simulated daemon, stub SIP\n"
      << "  -t          validate the configuration and exit\n"
      << "  -d          debug logging\n"
      << "  -v          print the version and exit\n"
      << "  -h          this help\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  aes67sip::AppOptions options;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = (i + 1) < argc;
    if (arg == "-c" || arg == "--config") {
      if (!has_value) {
        std::cerr << "error: " << arg << " requires a file path\n";
        return 2;
      }
      options.config_path = argv[++i];
    } else if (arg == "-a" || arg == "--http-addr") {
      if (!has_value) {
        std::cerr << "error: " << arg << " requires an address\n";
        return 2;
      }
      options.http_addr = argv[++i];
    } else if (arg == "-p" || arg == "--http-port") {
      if (!has_value) {
        std::cerr << "error: " << arg << " requires a port\n";
        return 2;
      }
      options.http_port = std::atoi(argv[++i]);
      if (options.http_port <= 0 || options.http_port > 65535) {
        std::cerr << "error: invalid HTTP port\n";
        return 2;
      }
    } else if (arg == "-f" || arg == "--fake") {
      options.force_fake = true;
    } else if (arg == "-t" || arg == "--validate") {
      options.validate_only = true;
    } else if (arg == "-d" || arg == "--debug") {
      options.debug = true;
    } else if (arg == "-v" || arg == "--version") {
      std::cout << aes67sip::version() << " (" << aes67sip::build_info() << ")\n";
      return 0;
    } else if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else {
      std::cerr << "error: unknown option '" << arg << "'\n\n";
      print_usage(argv[0]);
      return 2;
    }
  }

  aes67sip::App app(options);
  return app.run();
}
