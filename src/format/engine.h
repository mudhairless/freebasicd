#pragma once

#include "format/config.h"
#include <cstddef>
#include <string>

namespace fbformat {

std::string formatDocument(const std::string &content,
                           const std::string &filePath,
                           const FormatConfig &config);

// The edit for one range of a document (§9.6): the whole lines
// the range expands to, formatted with the state the lines before them
// leave behind. `changed == false` means the window already reads as
// formatted (or there is nothing to format in it) and text is empty.
struct RangeResult {
  size_t replacementStart = 0;
  size_t replacementEnd = 0;
  std::string text;
  bool changed = false;
};

RangeResult formatRange(const std::string &content, const std::string &filePath,
                        const FormatConfig &config, size_t rangeStart,
                        size_t rangeEnd);

} // namespace fbformat
