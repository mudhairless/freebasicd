#include "format/config.h"
#include "format/engine.h"
#include <argparse/argparse.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Exit codes (M17 plan §2): 0 = formatted or unchanged, 1 = changes needed
// under --check, 2 = error (usage, config, IO).
static constexpr int kExitOk = 0;
static constexpr int kExitChangesNeeded = 1;
static constexpr int kExitError = 2;

static std::string readFile(const std::string &path, bool *ok) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    *ok = false;
    return "";
  }
  std::stringstream buffer;
  buffer << f.rdbuf();
  *ok = true;
  return buffer.str();
}

static bool writeFile(const std::string &path, const std::string &content) {
  std::ofstream f(path, std::ios::binary);
  if (!f)
    return false;
  f << content;
  return static_cast<bool>(f);
}

int main(int argc, char **argv) {
  argparse::ArgumentParser program("fbformat");
  program.add_description(
      "Format FreeBASIC source. The formatted document is written to stdout "
      "unless --in-place or --check is given.");
  program.add_argument("-i", "--in-place")
      .help("write formatted output back to the file(s)")
      .flag();
  program.add_argument("--check")
      .help("exit 1 if any file would be reformatted, 0 if unchanged")
      .flag();
  program.add_argument("--stdin-filepath")
      .help("path used for config discovery when reading from stdin")
      .default_value(std::string{});
  program.add_argument("--config")
      .help("explicit config file, skipping discovery "
            "(parsed; not yet honored)")
      .default_value(std::string{});
  program.add_argument("--style")
      .help("preset name, overriding the config file's style "
            "(parsed; not yet honored)")
      .default_value(std::string{});
  program.add_argument("files")
      .help("files to format; reads stdin when no file is given")
      .nargs(argparse::nargs_pattern::any);

  try {
    program.parse_args(argc, argv);
  } catch (std::exception const &e) {
    std::cerr << e.what() << '\n' << program;
    return kExitError;
  }

  const bool inPlace = program.get<bool>("--in-place");
  const bool checkOnly = program.get<bool>("--check");
  const std::string stdinFilepath =
      program.get<std::string>("--stdin-filepath");
  // Parsed for flag-surface parity with the plan (§2); the config layer does
  // not accept overrides yet, so discovery still wins.
  (void)program.get<std::string>("--config");
  (void)program.get<std::string>("--style");
  const std::vector<std::string> files =
      program.get<std::vector<std::string>>("files");

  if (files.empty()) {
    // Read from stdin
    std::stringstream buffer;
    buffer << std::cin.rdbuf();
    std::string content = buffer.str();
    std::string filepath = stdinFilepath;
    if (filepath.empty()) {
      filepath = "stdin.bas";
    }
    fbformat::FormatConfig cfg = fbformat::loadConfigForPath(filepath);
    std::string formatted = fbformat::formatDocument(content, filepath, cfg);
    if (checkOnly) {
      return (formatted == content) ? kExitOk : kExitChangesNeeded;
    }
    std::cout << formatted;
    return kExitOk;
  }

  int exitCode = kExitOk;
  for (const auto &f : files) {
    bool readOk = false;
    std::string content = readFile(f, &readOk);
    if (!readOk) {
      std::cerr << "fbformat: cannot read " << f << '\n';
      return kExitError;
    }
    fbformat::FormatConfig cfg = fbformat::loadConfigForPath(f);
    std::string formatted = fbformat::formatDocument(content, f, cfg);
    if (formatted != content) {
      if (checkOnly) {
        exitCode = kExitChangesNeeded;
      } else if (inPlace) {
        if (!writeFile(f, formatted)) {
          std::cerr << "fbformat: cannot write " << f << '\n';
          return kExitError;
        }
      } else {
        std::cout << formatted;
      }
    } else if (!checkOnly && !inPlace) {
      // Print unchanged content if writing to stdout
      std::cout << formatted;
    }
  }
  return exitCode;
}
