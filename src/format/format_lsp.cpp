#include "format/format_lsp.h"
#include "format/engine.h"

namespace fbformat {

std::vector<TextEdit> formatDocumentLsp(const std::string &content,
                                        const std::string &filePath,
                                        const FormatConfig &config) {
  std::string formatted = formatDocument(content, filePath, config);
  if (formatted == content) {
    return {};
  }
  TextEdit edit;
  edit.start = 0;
  edit.end = content.size();
  edit.newText = formatted;
  return {edit};
}

std::vector<TextEdit> formatRangeLsp(const std::string &content,
                                     const std::string &filePath,
                                     const FormatConfig &config,
                                     size_t rangeStart, size_t rangeEnd) {
  RangeResult const range =
      formatRange(content, filePath, config, rangeStart, rangeEnd);
  if (!range.changed) {
    return {};
  }
  TextEdit edit;
  edit.start = range.replacementStart;
  edit.end = range.replacementEnd;
  edit.newText = range.text;
  return {edit};
}

} // namespace fbformat