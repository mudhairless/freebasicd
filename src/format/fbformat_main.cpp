#include "format/config.h"
#include "format/engine.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static std::string readFile(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return "";
  std::stringstream buffer;
  buffer << f.rdbuf();
  return buffer.str();
}

static void writeFile(const std::string &path, const std::string &content) {
  std::ofstream f(path, std::ios::binary);
  if (f)
    f << content;
}

int main(int argc, char **argv) {
  bool inPlace = false;
  bool checkOnly = false;
  std::string stdinFilepath;
  std::string explicitConfig;
  std::string explicitStyle;
  std::vector<std::string> files;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--in-place" || arg == "-i") {
      inPlace = true;
    } else if (arg == "--check") {
      checkOnly = true;
    } else if (arg == "--stdin-filepath" && i + 1 < argc) {
      stdinFilepath = argv[++i];
    } else if (arg == "--config" && i + 1 < argc) {
      explicitConfig = argv[++i];
      (void)explicitConfig;
    } else if (arg == "--style" && i + 1 < argc) {
      explicitStyle = argv[++i];
      (void)explicitStyle;
    } else if (!arg.empty() && arg[0] != '-') {
      files.push_back(arg);
    }
  }

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
      return (formatted == content) ? 0 : 1;
    }
    std::cout << formatted;
    return 0;
  }

  int exitCode = 0;
  for (const auto &f : files) {
    std::string content = readFile(f);
    fbformat::FormatConfig cfg = fbformat::loadConfigForPath(f);
    std::string formatted = fbformat::formatDocument(content, f, cfg);
    if (formatted != content) {
      if (checkOnly) {
        exitCode = 1;
      } else if (inPlace) {
        writeFile(f, formatted);
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