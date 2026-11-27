#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace lunar::detail {

// Standard base64 with optional padding; whitespace is skipped. Null on bad input.
std::optional<std::string> decodeBase64(std::string_view text);

} // namespace lunar::detail
