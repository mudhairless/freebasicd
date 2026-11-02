#include "format/config.h"
#include "format/engine.h"
#include "i18n.h"
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

static std::string readStdin() {
  std::stringstream buffer;
  buffer << std::cin.rdbuf();
  return buffer.str();
}

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

// Resolve the formatter config for one document. A config error (unreadable or
// invalid file, unknown style) is fatal: formatting with a guessed style is the
// silent-re-tab bug the presence rule exists to prevent. Warnings go to stderr
// and do not stop the run.
static bool configFor(const std::string &path, const std::string &configFile,
                      const std::string &style, fbformat::FormatConfig *out) {
  fbformat::ConfigQuery query;
  query.configFile = configFile;
  query.style = style;
  fbformat::ResolvedConfig const resolved =
      fbformat::resolveConfig(path, query);
  for (const std::string &warning : resolved.warnings) {
    std::cerr << "fbformat: warning: " << warning << '\n';
  }
  if (!resolved.ok) {
    std::cerr << "fbformat: " << resolved.error << '\n';
    return false;
  }
  *out = resolved.config;
  return true;
}

int main(int argc, char **argv) {
  fblang::initI18n();
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
      .help("explicit config file, skipping discovery")
      .default_value(std::string{});
  program.add_argument("--style")
      .help("preset name, overriding the config file's style")
      .default_value(std::string{});
  program.add_argument("files")
      .help("files to format; `-` or no file reads stdin")
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
  const std::string configFile = program.get<std::string>("--config");
  const std::string style = program.get<std::string>("--style");
  const std::vector<std::string> files =
      program.get<std::vector<std::string>>("files");

  // `-` is stdin, and it cannot be mixed with file arguments: stdout would
  // receive two documents with nothing between them, and `-i` has no meaning
  // for the stream.
  bool readFromStdin = files.empty();
  std::vector<std::string> paths;
  for (const std::string &f : files) {
    if (f == "-") {
      if (readFromStdin) {
        std::cerr << "fbformat: stdin (-) given more than once\n";
        return kExitError;
      }
      readFromStdin = true;
    } else {
      paths.push_back(f);
    }
  }
  if (readFromStdin && !paths.empty()) {
    std::cerr << "fbformat: cannot mix stdin (-) with file arguments\n";
    return kExitError;
  }
  // Config discovery needs a path. Without one, the user must name the config
  // or style explicitly rather than inheriting whatever the cwd happens to sit
  // under.
  if (readFromStdin && stdinFilepath.empty() && configFile.empty() &&
      style.empty()) {
    std::cerr << "fbformat: reading stdin needs --stdin-filepath, --config, or "
                 "--style\n";
    return kExitError;
  }
  // Concatenating several documents on stdout is not a format anyone can read
  // back; require an explicit destination.
  if (!readFromStdin && !inPlace && !checkOnly && paths.size() > 1) {
    std::cerr << "fbformat: more than one file to stdout needs --in-place or "
                 "--check\n";
    return kExitError;
  }

  if (readFromStdin) {
    std::string const content = readStdin();
    fbformat::FormatConfig cfg;
    if (!configFor(stdinFilepath, configFile, style, &cfg)) {
      return kExitError;
    }
    std::string const formatted =
        fbformat::formatDocument(content, stdinFilepath, cfg);
    if (checkOnly) {
      return formatted == content ? kExitOk : kExitChangesNeeded;
    }
    std::cout << formatted;
    return kExitOk;
  }

  int exitCode = kExitOk;
  for (const std::string &f : paths) {
    bool readOk = false;
    std::string const content = readFile(f, &readOk);
    if (!readOk) {
      std::cerr << "fbformat: cannot read " << f << '\n';
      return kExitError;
    }
    fbformat::FormatConfig cfg;
    if (!configFor(f, configFile, style, &cfg)) {
      return kExitError;
    }
    std::string const formatted = fbformat::formatDocument(content, f, cfg);
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
