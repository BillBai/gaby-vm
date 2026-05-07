// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

#include <iostream>
#include <ostream>
#include <string_view>

#include "gaby_vm/gaby_vm.h"

// gaby-vm CLI demo for macOS and Linux. Embeds gaby-vm as a static library
// and serves as the reference integration example until an iOS Xcode demo
// arrives. Argv parsing is intentionally hand-rolled — there are no flags
// worth a parser dependency yet.

namespace {

constexpr std::string_view kProgName = "gaby-vm";

void PrintVersion() {
  std::cout << kProgName << " " << gaby_vm::version() << "\n";
}

void PrintUsage(std::ostream& os) {
  os << "Usage: " << kProgName << " [OPTIONS]\n"
     << "\n"
     << "Options:\n"
     << "  -v, --version    Print version and exit\n"
     << "  -h, --help       Print this help and exit\n"
     << "\n"
     << "With no options, prints a startup banner. The simulator core is not\n"
     << "yet wired up; this binary currently demonstrates embedding gaby-vm\n"
     << "as a static library on macOS and Linux.\n";
}

void PrintBanner() {
  PrintVersion();
  std::cout << "(no program loaded — simulator not yet wired)\n";
}

enum class Mode { kBanner, kVersion, kHelp, kUnknown };

Mode ParseArgs(int argc, char** argv) {
  if (argc <= 1) return Mode::kBanner;
  if (argc > 2) return Mode::kUnknown;
  std::string_view arg = argv[1];
  if (arg == "-v" || arg == "--version") return Mode::kVersion;
  if (arg == "-h" || arg == "--help") return Mode::kHelp;
  return Mode::kUnknown;
}

}  // namespace

int main(int argc, char** argv) {
  switch (ParseArgs(argc, argv)) {
    case Mode::kBanner:
      PrintBanner();
      return 0;
    case Mode::kVersion:
      PrintVersion();
      return 0;
    case Mode::kHelp:
      PrintUsage(std::cout);
      return 0;
    case Mode::kUnknown:
      PrintUsage(std::cerr);
      return 2;
  }
  return 0;
}
