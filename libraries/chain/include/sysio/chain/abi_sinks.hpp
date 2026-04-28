#pragma once

#include <sysio/chain/abi_serializer.hpp>

#include <fc/io/datastream.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/reflect/json_stream.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sysio::chain::impl {

/**
 *  Sink consumed by `abi_serializer::_binary_walk<Sink>` while it walks an ABI-typed
 *  binary blob.  Two implementations exist in lock-step:
 *
 *    - `variant_sink`: assembles an `fc::variant` tree.  Drives the legacy
 *      `binary_to_variant` API after step 3 of the streaming migration.
 *    - `stream_sink`:  emits JSON tokens directly into an `fc::json_writer` (no
 *      intermediate variant allocation).  Drives the new `binary_to_json_stream` API.
 *
 *  Both sinks expose the same write-side surface (`begin_object`, `value_string`, ...).
 *  The walker calls `unpack_built_in` / `unpack_protobuf` to delegate the type-specific
 *  read+emit step; each sink owns its own per-type unpack table.
 */

/**
 *  Builds an `fc::variant` tree token-by-token.  Object frames accumulate fields into
 *  a `mutable_variant_object`; array frames accumulate elements into an `fc::variants`.
 *  When the outermost frame closes (or a single scalar value is emitted at top level),
 *  `take_result()` yields the assembled variant.
 */
class variant_sink {
public:
   explicit variant_sink(const abi_serializer& abi) noexcept : abi_(abi) {}

   void begin_object();
   void end_object();
   void begin_array();
   void end_array();
   void key(std::string_view k);

   void value_string(std::string_view s);
   void value_int64(int64_t n);
   void value_uint64(uint64_t n);
   void value_int32(int32_t n)   { value_int64(n); }
   void value_uint32(uint32_t n) { value_uint64(n); }
   void value_int16(int16_t n)   { value_int64(n); }
   void value_uint16(uint16_t n) { value_uint64(n); }
   void value_int8(int8_t n)     { value_int64(n); }
   void value_uint8(uint8_t n)   { value_uint64(n); }
   void value_double(double d);
   void value_bool(bool b);
   void value_null();
   void value_hex(const char* data, size_t size);
   void raw_value(std::string_view raw_json);

   /// Inject an already-built `fc::variant` at the current value position.  Used by
   /// `unpack_built_in` and `unpack_protobuf` to avoid re-tokenising values that the
   /// existing variant-side unpack functions produce as a single `fc::variant`.
   void value_variant(fc::variant v) { emit_value(std::move(v)); }

   /// True if at least one item has been added to the current frame.  Used by the walker
   /// to enforce the legacy "Unable to unpack '...' from stream" guard on empty structs.
   bool frame_has_items() const noexcept;

   void unpack_built_in(std::string_view ftype, fc::datastream<const char*>& stream,
                        bool is_array, bool is_optional, abi_traverse_context& ctx);
   void unpack_protobuf(std::string_view ftype, fc::datastream<const char*>& stream);

   fc::variant take_result() && { return std::move(result_); }

private:
   void emit_value(fc::variant v);

   const abi_serializer& abi_;

   enum class frame_kind : uint8_t { object, array };
   struct frame {
      frame_kind                  kind;
      fc::mutable_variant_object  mvo;
      fc::variants                arr;
      std::string                 pending_key;
      bool                        has_pending_key = false;
      uint32_t                    item_count = 0;
   };
   std::vector<frame>  stack_;
   fc::variant         result_;
};

/**
 *  Emits JSON tokens directly into an `fc::json_writer`.  No `fc::variant` is
 *  constructed for built-in scalars; container framing forwards directly to the
 *  writer.  Per-frame item counts are tracked locally so the walker can run the
 *  same empty-struct guard the variant path enforces.
 *
 *  In the initial scaffolding commit, `unpack_built_in` falls back to the variant-side
 *  unpack and pipes the result through `to_json_stream(variant, w)`.  Subsequent commits
 *  replace those entries with direct streaming unpacks.
 */
class stream_sink {
public:
   stream_sink(const abi_serializer& abi, fc::json_writer& w) noexcept : abi_(abi), w_(w) {}

   void begin_object();
   void end_object();
   void begin_array();
   void end_array();
   void key(std::string_view k);

   void value_string(std::string_view s);
   void value_int64(int64_t n);
   void value_uint64(uint64_t n);
   void value_int32(int32_t n)   { value_int64(n); }
   void value_uint32(uint32_t n) { value_uint64(n); }
   void value_int16(int16_t n)   { value_int64(n); }
   void value_uint16(uint16_t n) { value_uint64(n); }
   void value_int8(int8_t n)     { value_int64(n); }
   void value_uint8(uint8_t n)   { value_uint64(n); }
   void value_double(double d);
   void value_bool(bool b);
   void value_null();
   void value_hex(const char* data, size_t size);
   void raw_value(std::string_view raw_json);

   /// Splice an already-built variant.  Internally serialises via `to_json_stream`
   /// so the writer state stays balanced.  Used by the protobuf bridge today and
   /// by the variant-fallback built-in path until commit 2 lands the direct unpacks.
   void value_variant(const fc::variant& v);

   bool frame_has_items() const noexcept;

   void unpack_built_in(std::string_view ftype, fc::datastream<const char*>& stream,
                        bool is_array, bool is_optional, abi_traverse_context& ctx);
   void unpack_protobuf(std::string_view ftype, fc::datastream<const char*>& stream);

private:
   void on_value_emitted() noexcept;

   const abi_serializer& abi_;
   fc::json_writer&      w_;
   std::vector<uint32_t> frame_items_;
};

} // namespace sysio::chain::impl
