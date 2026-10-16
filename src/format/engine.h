#pragma once

#include "format/config.h"
#include <string>

namespace fbformat {

std::string formatDocument(const std::string &content,
                           const std::string &filePath,
                           const FormatConfig &config);

} // namespace fbformat