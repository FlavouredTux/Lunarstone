#pragma once

#include <stdexcept>

namespace lunar {

// Anything Roblox would raise as a script error.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace lunar
