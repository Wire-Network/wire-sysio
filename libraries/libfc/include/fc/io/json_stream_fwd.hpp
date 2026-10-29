#pragma once

// Lightweight forward-declaration of fc::json_writer for use in type headers that want to
// declare to_json_stream overloads without pulling in the full json_writer definition
// (and the json/escape_string dependencies it transitively drags in).  Callers that need
// to actually emit must include <fc/io/json_stream.hpp>; callers that only need to
// declare the overload include this header.
//
// Pattern matches the existing `class variant;` forward declaration used by headers that
// declare to_variant without including fc/variant.hpp.

namespace fc {
   class json_writer;
}
