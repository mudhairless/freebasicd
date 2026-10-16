#include "format/engine.h"

namespace fbformat {

std::string formatDocument(const std::string &content,
                           const std::string &filePath,
                           const FormatConfig &config) {
  (void)filePath;
  (void)config;
  // Minimal placeholder - identity for now
  return content;
}

} // namespace fbformat