#include "format/engine.h"
#include <algorithm>
#include "format/tokens.h"

namespace fbformat {

std::string formatDocument(const std::string& content, const std::string& filePath,
                          const FormatConfig& config) {
  (void)filePath;
  (void)config;
  // Minimal pass: tokenize and preserve content - identity for now
  // This preserves block comments losslessly since token ranges come from original
  TokenStream ts(content);
  (void)ts;
  return content;
}

}  // namespace fbformat
