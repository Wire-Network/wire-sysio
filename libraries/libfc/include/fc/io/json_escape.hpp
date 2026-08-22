#pragma once

// JSON string-escape primitives shared by both the legacy variant-tree path and
// the streaming json_writer path.  Decoupled from class json so that
// fc/io/json_stream.hpp (and downstream variant.hpp / flat.hpp container
// streaming overloads) can call escape_string without pulling in class json --
// avoiding the cycle: variant.hpp -> json_stream.hpp -> json.hpp -> variant.hpp.
//
// The yield-function alias is the parameter type used by escape_string;
// class json::yield_function_t is an alias to this type so existing callers
// continue to compile unchanged.

#include <fc/utility.hpp>
#include <string>
#include <string_view>

namespace fc {

   // The yield callback used by streaming serialisers to check deadlines and bail out
   // of long-running JSON conversions.  class json::yield_function_t (in fc/io/json.hpp)
   // is an alias to this type so existing callers continue to compile.
   using json_yield_function_t = fc::optional_delegate<void(size_t)>;

   /// Escape a UTF-8 string for inclusion in a JSON value.  Returns the new string.
   std::string escape_string( const std::string_view& str, const json_yield_function_t& yield, bool escape_control_chars = true );

   /// Append the escaped form of `str` to `out`.  Returns true if any characters were
   /// escaped or invalid UTF-8 was pruned.
   bool        escape_string( const std::string_view& str, std::string& out, const json_yield_function_t& yield, bool escape_control_chars = true );

} // namespace fc
