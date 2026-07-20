#pragma once

#include <fc/exception/exception.hpp>
#include <fc/io/json_escape.hpp>

#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace fc {

/// Slack the writer guarantees in the output buffer at construction so typical responses
/// append without an early reallocation.
inline constexpr size_t json_writer_initial_reserve = 4096;

/// Decimal-digit buffer that fits any int64/uint64 (uint64 max = 20 digits, plus sign and slack).
inline constexpr size_t int64_decimal_buf_size = std::numeric_limits<int64_t>::digits10 + 3;
static_assert(int64_decimal_buf_size >= std::numeric_limits<uint64_t>::digits10 + 1);

/// Shortest-roundtrip std::to_chars of a finite double fits well within this.
inline constexpr size_t double_shortest_buf_size = 32;

/// 64-bit integers whose magnitude exceeds this are emitted as quoted JSON strings to
/// preserve precision past JS's 2^53 mantissa.  Single source of truth for the streaming
/// emitters; matches fc::json::to_string's emission (libfc/src/io/json.cpp int64/uint64 cases).
inline constexpr int64_t json_integer_quote_magnitude = 0xffffffff;

/// Depth cap for the fc::variant / variant_object streaming walkers; matches
/// DEFAULT_MAX_RECURSION_DEPTH in fc/io/json.hpp so the streaming path throws where
/// fc::json::to_string's yield would.
inline constexpr uint32_t json_stream_max_depth = 200;

/// Default byte-growth stride between growth-guard invocations (see json_writer's guarded
/// constructor).  Small enough that a caller enforcing a memory budget observes emission
/// growth promptly; large enough that the guard is a negligible fraction of emission cost
/// (one invocation per ~64 KiB appended).  Responses smaller than one stride never invoke
/// the guard mid-emission.
inline constexpr size_t json_writer_default_guard_stride = 64 * 1024;

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
   /// Growth-guard callback: invoked with the output buffer's current size (bytes) each time
   /// the buffer has grown one stride past the previous invocation.  The guard may throw to
   /// abort emission -- eg an HTTP memory-budget enforcer rejecting a response mid-serialize.
   /// The guard must not emit through the writer it guards.
   using growth_guard_t = std::function<void(size_t buffer_size)>;

   /// @param out          buffer tokens are appended to; also the guard's measured quantity.
   /// @param growth_guard optional guard consulted as the buffer grows (empty = never).
   /// @param guard_stride minimum byte growth between guard invocations; a stride of 0
   ///                     re-checks on every token (test hook, not for production use).
   ///
   /// Guard-throw semantics: if the guard throws, the writer re-arms so the NEXT token
   /// emitted re-invokes the guard immediately (regardless of stride).  Mid-emission
   /// rollback handlers (eg abi_serializer's catch-rewind-hex-fallback path) legitimately
   /// swallow exceptions from nested serializers; re-arming guarantees a guard abort
   /// re-raises out of every such handler instead of being silently absorbed -- unless the
   /// guard's condition has cleared, in which case emission resumes legitimately.
   explicit json_writer(std::string& out, growth_guard_t growth_guard = {},
                        size_t guard_stride = json_writer_default_guard_stride)
   : out_(out)
   , guard_(std::move(growth_guard))
   , guard_stride_(guard_stride)
   {
      if (guard_) {
         next_guard_check_ = out_.size() + guard_stride_;
      }
      // Enough room for a reasonable response without reallocation; callers that know
      // the expected size can reserve() themselves before constructing the writer.
      // Skip if the caller already has slack so we don't risk a spec-permitted shrink.
      if (out_.capacity() - out_.size() < json_writer_initial_reserve) {
         out_.reserve(out_.size() + json_writer_initial_reserve);
      }
   }

   void begin_object() {
      value_prefix();
      out_.push_back('{');
      stack_.push_back(frame{context::object, false});
   }

   void end_object() {
      assert(!stack_.empty() && stack_.back().ctx == context::object);
      assert(!awaiting_value_); // key() without a value would leave a dangling colon
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
      guard_check();
      assert(!stack_.empty() && stack_.back().ctx == context::object);
      assert(!awaiting_value_); // two key() calls in a row would produce "a":,"b": (invalid JSON)
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
      // NaN / +-inf have no JSON representation: any encoder would emit non-conforming
      // tokens that no parser will accept.  Reject before mutating the output buffer
      // so the writer's frame state stays consistent on throw.
      FC_ASSERT(std::isfinite(d), "fc::json_writer::value_double: non-finite double cannot be JSON-encoded");
      value_prefix();
      // std::to_chars is locale-independent (period radix always) and shortest-roundtrip:
      // the parsed-back double is bit-identical to the input.  snprintf with %g would
      // honor LC_NUMERIC and emit comma radix in non-C locales -- invalid JSON.
      char buf[double_shortest_buf_size];
      auto r = std::to_chars(buf, buf + sizeof(buf), d);
      assert(r.ec == std::errc{}); // unreachable: finite double + buffer sized for shortest-roundtrip
      out_.append(buf, static_cast<size_t>(r.ptr - buf));
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

   /// True when all begin_* have been paired with end_* AND no key() is awaiting
   /// its value.  Used by tests/sinks that wrap the writer to confirm end-state
   /// integrity after a streaming sequence completes.
   bool balanced() const { return stack_.empty() && !awaiting_value_; }

   /// Snapshot of the writer's mutable state.  Pair with rewind() to discard
   /// any tokens emitted between the two calls.  Cheap (~3 words copied) and
   /// non-throwing.  Used by the abi_serializer streaming path to roll back
   /// half-written ABI fields when the inner unpack throws partway.
   struct checkpoint_t {
      size_t buf_size                = 0;
      size_t stack_size              = 0;
      bool   surviving_top_has_item  = false; // restored value of stack_[stack_size-1].has_item
      bool   awaiting_value          = false;
   };
   checkpoint_t checkpoint() const noexcept {
      return {
         out_.size(),
         stack_.size(),
         stack_.empty() ? false : stack_.back().has_item,
         awaiting_value_
      };
   }
   void rewind(const checkpoint_t& cp) noexcept {
      // rewind() only discards tokens APPENDED since checkpoint(); a caller that closed
      // frames past the checkpoint and re-opened new ones would resize the stack UP here
      // with value-initialized frames -- silent corruption, so the contract is asserted.
      assert(out_.size() >= cp.buf_size);
      assert(stack_.size() >= cp.stack_size);
      out_.resize(cp.buf_size);
      stack_.resize(cp.stack_size);
      if (!stack_.empty()) stack_.back().has_item = cp.surviving_top_has_item;
      awaiting_value_ = cp.awaiting_value;
   }

private:
   enum class context : uint8_t { object, array };
   struct frame {
      context ctx      = context::object;
      bool    has_item = false; // true once one element or key:value has been emitted in this frame
   };

   /// Consult the growth guard if the buffer has crossed the next stride boundary (or on
   /// every token once a prior guard invocation threw -- see the constructor contract).
   /// Called from the head of key() and value_prefix(), which between them front every
   /// token-emitting entry point except end_object/end_array (1 byte each; the final
   /// buffer size is the caller's to observe after emission completes).
   void guard_check() {
      if (out_.size() < next_guard_check_) {
         return;
      }
      // Re-arm across a guard throw: 0 makes every subsequent token re-enter here while
      // the guard keeps throwing, so a catch-and-continue serializer upstream cannot
      // absorb the abort.  Restored to a real stride only when the guard returns cleanly.
      next_guard_check_ = 0;
      guard_(out_.size());
      next_guard_check_ = out_.size() + guard_stride_;
   }

   void value_prefix() {
      guard_check();
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
   // parsing overhead that snprintf pays on every call.
   void append_signed(int64_t n) {
      char buf[int64_decimal_buf_size];
      auto r = std::to_chars(buf, buf + sizeof(buf), n);
      assert(r.ec == std::errc{}); // unreachable: buffer sized for any int64 decimal
      out_.append(buf, static_cast<size_t>(r.ptr - buf));
   }
   void append_unsigned(uint64_t n) {
      char buf[int64_decimal_buf_size];
      auto r = std::to_chars(buf, buf + sizeof(buf), n);
      assert(r.ec == std::errc{}); // unreachable: buffer sized for any uint64 decimal
      out_.append(buf, static_cast<size_t>(r.ptr - buf));
   }

   std::string&             out_;
   std::vector<frame>       stack_;            // small; vector is fine and avoids extra deps
   growth_guard_t           guard_;            // empty unless the guarded constructor form was used
   size_t                   guard_stride_     = json_writer_default_guard_stride;
   size_t                   next_guard_check_ = std::numeric_limits<size_t>::max(); // SIZE_MAX = no guard
   bool                     awaiting_value_ = false;
};

/// to_json_stream overload for an emit-closure: a `std::function<void(json_writer&)>`
/// whose JSON output is whatever the closure writes when invoked.  Used by callers
/// that want to deliver a streaming response through machinery (eg `bind_stream`'s
/// async/typed-T paths) that wraps the payload in `to_json_stream(payload, w)` --
/// the closure itself is the emitter, so we just invoke it.
inline void to_json_stream(const std::function<void(json_writer&)>& fn, json_writer& w) {
   if (fn) fn(w);
   else    w.value_null();
}

} // namespace fc
