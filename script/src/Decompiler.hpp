#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace lunar::lua {

// Luau bytecode back into readable Luau, the way tools/luau_decompile.py
// makes it (this is that tool in C++, function for function, so its notes
// hold here): if/else and the loops, and/or conditions, locals and their
// scopes, closures with what they capture, table constructors and method
// calls. Locals are named after what they hold where that's clear and
// v<register> otherwise; with `lines` each statement ends with the source
// line it came from (--@<line>). The opcodes may be as the VM reads them or
// scrambled (op * 227); the error is why the bytecode didn't decompile.
std::expected<std::string, std::string> decompileBytecode(std::string_view bytecode, bool lines = true);

} // namespace lunar::lua
