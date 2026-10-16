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
  (void)rangeStart;
  (void)rangeEnd;
  // For now, defer range formatting - return full document edit if changed
  return formatDocumentLsp(content, filePath, config);
}

} // namespace fbformat