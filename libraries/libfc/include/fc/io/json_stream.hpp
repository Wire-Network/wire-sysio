#pragma once

#include <fc/io/json_yield.hpp>

#include <cassert>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fc {

/**
 *  Streaming JSON writer that emits tokens directly into an output std::string.
 *
 *  Designed as a faster alternative to the fc::mutable_variant_object -> fc::variant ->
 *  fc::json::to_string pipeline used throughout the HTTP endpoints.  Instead of allocating
 *  per-field variant_object entries and per-string fc::variant wrappers, callers drive the
 *  writer with explicit begin_object / key / value_* calls that append bytes to the output
 *  buffer in place.
 *
 *  Contract:
 *    - Values must appear in value-position only (top-level, after a key(), or inside an
 *      array).  Nesting is validated with asserts in debug builds.
 *    - end_object / end_array must balance begin_object / begin_array.
 *    - String content is JSON-escaped via fc::escape_string; control characters become
 *      \\uXXXX, surrogate pairs are produced when needed.
 *    - raw_value appends an already-serialized JSON fragment verbatim; callers are
 *      responsible for its validity (used e.g. to embed fc::variant output from legacy
 *      code paths).
 */
class json_writer {
public:
   explicit json_writer(std::string& out)
   : out_(out)
   {
      // Enough room for a reasonable response without reallocation; callers that know
      // the expected size can reserve() themselves before constructing the writer.
      out_.reserve(out_.size() + 4096);
   }

   void begin_object() {
      value_prefix();
      out_.push_back('{');
      stack_.push_back(frame{context::object, false});
   }

   void end_object() {
      assert(!stack_.empty() && stack_.back().ctx == context::object);
      out_.push_back('}');
      stack_.pop_back();
   }

   void begin_array() {
      value_prefix();
      out_.push_back('[');
      stack_.push_back(frame{context::array, false});
   }

   void end_array() {
      assert(!stack_.empty() && stack_.back().ctx == context::array);
      out_.push_back(']');
      stack_.pop_back();
   }

   void key(std::string_view k) {
      assert(!stack_.empty() && stack_.back().ctx == context::object);
      if (stack_.back().has_item) {
         out_.push_back(',');
      } else {
         stack_.back().has_item = true;
      }
      out_.push_back('"');
      // key strings must also be escaped (eg field names containing special chars are rare,
      // but correctness matters on untrusted input echoed into error messages).
      fc::escape_string(k, out_, json_yield_function_t(), true);
      out_.push_back('"');
      out_.push_back(':');
      // A key mandates a value follows; suppress the comma from value_prefix for that value.
      awaiting_value_ = true;
   }

   void value_string(std::string_view s) {
      value_prefix();
      out_.push_back('"');
      fc::escape_string(s, out_, json_yield_function_t(), true);
      out_.push_back('"');
   }

   void value_int64(int64_t n)   { value_prefix(); append_signed(n); }
   void value_uint64(uint64_t n) { value_prefix(); append_unsigned(n); }
   void value_int32(int32_t n)   { value_int64(n); }
   void value_uint32(uint32_t n) { value_uint64(n); }
   void value_int16(int16_t n)   { value_int64(n); }
   void value_uint16(uint16_t n) { value_uint64(n); }
   void value_int8(int8_t n)     { value_int64(n); }
   void value_uint8(uint8_t n)   { value_uint64(n); }

   void value_double(double d) {
      value_prefix();
      char buf[32];
      // "%.17g" preserves round-trip precision for IEEE 754 doubles.  Callers that need
      // bit-exact formatting should emit via raw_value().
      int n = std::snprintf(buf, sizeof(buf), "%.17g", d);
      if (n > 0) out_.append(buf, static_cast<size_t>(n));
      else       out_.append("null", 4); // NaN/inf are not valid JSON; emit null defensively.
   }

   void value_bool(bool b) {
      value_prefix();
      if (b) out_.append("true", 4);
      else   out_.append("false", 5);
   }

   void value_null() {
      value_prefix();
      out_.append("null", 4);
   }

   /// Emits a JSON string token whose content is the lowercase hex encoding of the
   /// byte range [data, data+size).  No intermediate std::string allocation; bytes
   /// are looped once and 2 hex chars are appended per source byte.  Equivalent
   /// observable output to value_string(fc::to_hex(data, size)) but avoids the
   /// per-call heap allocation in fc::to_hex.  Used by streaming JSON paths that
   /// previously paid the to_hex allocation on every action's data / return_value.
   void value_hex(const char* data, size_t size) {
      value_prefix();
      out_.push_back('"');
      static constexpr char digits[] = "0123456789abcdef";
      const auto* p = reinterpret_cast<const uint8_t*>(data);
      for (size_t i = 0; i < size; ++i) {
         out_.push_back(digits[p[i] >> 4]);
         out_.push_back(digits[p[i] & 0x0f]);
      }
      out_.push_back('"');
   }

   /// Append a preformatted JSON fragment at a value position.  Used to embed output
   /// from legacy fc::variant paths (eg abi_serializer::binary_to_variant + json::to_string)
   /// without an additional parse/re-emit cycle.
   void raw_value(std::string_view raw) {
      value_prefix();
      out_.append(raw.data(), raw.size());
   }

   /// Fused key + value emitter; the streaming-JSON counterpart to
   /// fc::mutable_variant_object::set / operator().  Returns *this so call sites
   /// chain naturally:
   ///
   ///   w.begin_object();
   ///   w.set("id", id)
   ///    .set("number", n)
   ///    .set("producer", producer);
   ///   w.end_object();
   ///
   /// Dispatches the value via to_json_stream, so any type with a
   /// to_json_stream overload (primitives, std containers, fc leaf types,
   /// FC_REFLECT'd structs via the reflector path) is supported.  Callers must
   /// have <fc/reflect/json_stream.hpp> included at the call site - that header
   /// is where the to_json_stream<T> primary template + reflector dispatch lives.
   template<typename T>
   json_writer& set(std::string_view name, const T& value) {
      key(name);
      to_json_stream(value, *this);
      return *this;
   }

   /// Fused key + raw-value emitter.  Same chaining shape as set(), but the
   /// value is a preformatted JSON fragment spliced verbatim (eg an
   /// abi_serializer::binary_to_variant result that has already been json'd).
   json_writer& set_raw(std::string_view name, std::string_view raw_json) {
      key(name);
      raw_value(raw_json);
      return *this;
   }

   /// True when all begin_* have been paired with end_*.  Asserted internally on destructor
   /// usage via value_prefix() is not possible, so callers can check this in tests.
   bool balanced() const { return stack_.empty(); }

private:
   enum class context : uint8_t { object, array };
   struct frame {
      context ctx;
      bool    has_item; // true once one element or key:value has been emitted in this frame
   };

   void value_prefix() {
      if (awaiting_value_) {
         // Value right after a key() - no separator, first-item bookkeeping already set.
         awaiting_value_ = false;
         return;
      }
      if (stack_.empty()) {
         // Top-level value (eg a bare array/object at the root). Nothing to prefix.
         return;
      }
      // Must be in array context when a raw value appears without a preceding key.
      assert(stack_.back().ctx == context::array);
      if (stack_.back().has_item) {
         out_.push_back(',');
      } else {
         stack_.back().has_item = true;
      }
   }

   // std::to_chars is non-throwing, locale-independent, and avoids the format-string
   // parsing overhead that snprintf pays on every call.  buf is sized for the longest
   // possible decimal of int64/uint64 (20 chars + sign + slack).  Failure case is
   // unreachable for fixed-width integers; the unconditional out_.append handles it.
   void append_signed(int64_t n) {
      char buf[24];
      auto r = std::to_chars(buf, buf + sizeof(buf), n);
      out_.append(buf, static_cast<size_t>(r.ptr - buf));
   }
   void append_unsigned(uint64_t n) {
      char buf[24];
      auto r = std::to_chars(buf, buf + sizeof(buf), n);
      out_.append(buf, static_cast<size_t>(r.ptr - buf));
   }

   std::string&             out_;
   std::vector<frame>       stack_;            // small; vector is fine and avoids extra deps
   bool                     awaiting_value_ = false;
};

} // namespace fc
