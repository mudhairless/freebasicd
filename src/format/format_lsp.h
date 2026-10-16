#pragma once

#include "format/config.h"
#include <string>
#include <vector>

namespace fbformat {

struct TextEdit {
  size_t start;
  size_t end;
  std::string newText;
};

std::vector<TextEdit> formatDocumentLsp(const std::string &content,
                                        const std::string &filePath,
                                        const FormatConfig &config);

std::vector<TextEdit> formatRangeLsp(const std::string &content,
                                     const std::string &filePath,
                                     const FormatConfig &config,
                                     size_t rangeStart, size_t rangeEnd);

} // namespace fbformat