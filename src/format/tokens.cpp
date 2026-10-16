#include "format/tokens.h"

namespace fbformat {

TokenStream::TokenStream(const std::string &content) : content_(content) {
  // Minimal placeholder - full lexer to be implemented later
  // For now, just create empty stream
}

} // namespace fbformat