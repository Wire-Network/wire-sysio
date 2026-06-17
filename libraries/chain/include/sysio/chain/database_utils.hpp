#pragma once

#include <sysio/chain/types.hpp>
#include <sysio/chain/abi_def.hpp>
#include <fc/int128.hpp>
#include <fc/io/raw.hpp>
#include <fc/crypto/base64.hpp>
#include <softfloat/softfloat.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <string_view>

namespace sysio::chain {

   template<typename ...Indices>
   class index_set;

   template<typename Index>
   class index_utils {
      public:
         using index_t = Index;

         template<typename F>
         static void walk( const chainbase::database& db, F function ) {
            auto const& index = db.get_index<Index>().indices();
            const auto& first = index.begin();
            const auto& last = index.end();
            for (auto itr = first; itr != last; ++itr) {
               function(*itr);
            }
         }

         template<typename Secondary, typename F>
         static void walk_by( const chainbase::database& db, F function ) {
            const auto& idx = db.get_index<Index, Secondary>();
            const auto& first = idx.begin();
            const auto& last = idx.end();
            for (auto itr = first; itr != last; ++itr) {
               function(*itr);
            }
         }

         template<typename Secondary, typename Key, typename F>
         static void walk_range( const chainbase::database& db, const Key& begin_key, const Key& end_key, F function ) {
            const auto& idx = db.get_index<Index, Secondary>();
            auto begin_itr = idx.lower_bound(begin_key);
            auto end_itr = idx.lower_bound(end_key);
            for (auto itr = begin_itr; itr != end_itr; ++itr) {
               function(*itr);
            }
         }

         template<typename Secondary, typename Key>
         static size_t size_range( const chainbase::database& db, const Key& begin_key, const Key& end_key ) {
            const auto& idx = db.get_index<Index, Secondary>();
            auto begin_itr = idx.lower_bound(begin_key);
            auto end_itr = idx.lower_bound(end_key);
            size_t res = 0;
            while (begin_itr != end_itr) {
               res++; ++begin_itr;
            }
            return res;
         }

         template<typename F>
         static void create( chainbase::database& db, F cons ) {
            db.create<typename index_t::value_type>(cons);
         }

         static void preallocate( chainbase::database& db, size_t num_objects ) {
            db.preallocate<typename index_t::value_type>(num_objects);
         }
   };

   template<typename Index>
   class index_set<Index> {
   public:
      static void add_indices( chainbase::database& db ) {
         db.add_index<Index>();
      }

      template<typename F>
      static void walk_indices( F function ) {
         function( index_utils<Index>() );
      }

      template<typename F>
      static void walk_indices_via_post( boost::asio::io_context& ctx, F function ) {
         boost::asio::post(ctx, [function]() {
            function( index_utils<Index>() );
         });
      }
   };

   template<typename FirstIndex, typename ...RemainingIndices>
   class index_set<FirstIndex, RemainingIndices...> {
   public:
      static void add_indices( chainbase::database& db ) {
         index_set<FirstIndex>::add_indices(db);
         index_set<RemainingIndices...>::add_indices(db);
      }

      template<typename F>
      static void walk_indices( F function ) {
         index_set<FirstIndex>::walk_indices(function);
         index_set<RemainingIndices...>::walk_indices(function);
      }

      template<typename F>
      static void walk_indices_via_post( boost::asio::io_context& ctx, F function ) {
         index_set<FirstIndex>::walk_indices_via_post(ctx, function);
         index_set<RemainingIndices...>::walk_indices_via_post(ctx, function);
      }
   };

namespace detail {
   struct snapshot_key_value_object {
      template<typename Stream>
      friend Stream& operator>>(Stream& ds, snapshot_key_value_object& o) {
         fc::raw::unpack(ds, o.primary_key);
         fc::raw::unpack(ds, o.payer);

         fc::unsigned_int sz;
         fc::raw::unpack(ds, sz);
         if(sz) {
            o.value.resize(sz);
            ds.read(o.value.data(), sz);
         }

         return ds;
      }

      template<typename Stream>
      friend Stream& operator<<(Stream& ds, const snapshot_key_value_object& o) {
         fc::raw::pack(ds, o.primary_key);
         fc::raw::pack(ds, o.payer);

         fc::raw::pack(ds, fc::unsigned_int(o.value.size()));
         if(o.value.size())
            ds.write(o.value.data(), o.value.size());

         return ds;
      }

      uint64_t          primary_key;
      account_name      payer;
      std::vector<char> value;
   };
}

// ---------------------------------------------------------------------------
// BE key codec -- mirrors the CDT's be_key_stream encoding in kv_raw_table.hpp.
// Used by get_table_rows API to decode/encode format=0 raw keys. The set of
// directly-supported scalar leaf types is the `key_leaf_kind` enum below
// (struct keys are expanded field-by-field on top of those leaves).
// ---------------------------------------------------------------------------
// Forward declarations: the fc float128_t variant conversions are defined at
// the bottom of this header; the BE key codec below uses them.
} // namespace sysio::chain
namespace fc {
   class variant;
   inline void to_variant( const float128_t& f, variant& v );
   inline void from_variant( const variant& v, float128_t& f );
} // namespace fc
namespace sysio::chain {

namespace be_key_codec {

struct reader {
   const char* pos;
   const char* end;

   reader(const char* d, size_t s) : pos(d), end(d + s) {}
   size_t remaining() const { return static_cast<size_t>(end - pos); }

   uint8_t  read_u8()   { return read_be<uint8_t>(); }
   uint16_t read_be16() { return read_be<uint16_t>(); }
   uint32_t read_be32() { return read_be<uint32_t>(); }
   uint64_t read_be64() { return read_be<uint64_t>(); }

   // Copy `n` raw bytes (already order-preserving, e.g. a checksum digest).
   void read_bytes(char* out, size_t n) {
      FC_ASSERT(remaining() >= n, "BE key underflow reading {} raw bytes", n);
      std::memcpy(out, pos, n);
      pos += n;
   }

   // NUL-escape decoding: 0x00,0x01 = literal NUL byte, 0x00,0x00 = end of string.
   std::string read_nul_escaped_string() {
      std::string s;
      bool terminated = false;
      while (pos < end) {
         char c = *pos++;
         if (c == '\0') {
            FC_ASSERT(pos < end, "BE key underflow reading string terminator");
            char next = *pos++;
            if (next == '\0') { terminated = true; break; } // 0x00,0x00 = end
            FC_ASSERT(next == '\x01', "Invalid NUL-escape sequence in BE key string: 0x00,0x{}", next);
            s.push_back('\0');                 // 0x00,0x01 = literal NUL
         } else {
            s.push_back(c);
         }
      }
      FC_ASSERT(terminated, "Unterminated NUL-escaped string in BE key");
      return s;
   }

private:
   template<typename T>
   T read_be() {
      constexpr size_t N = sizeof(T);
      FC_ASSERT(remaining() >= N, "BE key underflow reading {}-byte integer", N);
      T v = 0;
      for (size_t i = 0; i < N; ++i)
         v = (v << 8) | static_cast<uint8_t>(pos[i]);
      pos += N;
      return v;
   }
};

struct writer {
   std::vector<char> buf;

   void write_u8(uint8_t v) { buf.push_back(static_cast<char>(v)); }

   // Append `n` raw bytes verbatim (already order-preserving).
   void write_bytes(const char* p, size_t n) { buf.insert(buf.end(), p, p + n); }

   void write_be16(uint16_t v) { write_be(v); }

   void write_be32(uint32_t v) { write_be(v); }

   void write_be64(uint64_t v) { write_be(v); }

   // NUL-escape encoding: 0x00 -> 0x00,0x01, terminated by 0x00,0x00.
   void write_nul_escaped_string(const std::string& s) {
      for (char c : s) {
         buf.push_back(c);
         if (c == '\0') buf.push_back('\x01');
      }
      buf.push_back('\0');
      buf.push_back('\0');
   }

   std::vector<char> release() { return std::move(buf); }

private:
   template<typename T>
   void write_be(T v) {
      char tmp[sizeof(T)];
      for (int i = sizeof(T) - 1; i >= 0; --i) {
         tmp[i] = static_cast<char>(v & 0xFF);
         v >>= 8;
      }
      buf.insert(buf.end(), tmp, tmp + sizeof(T));
   }
};

// ── Leaf key kinds ──────────────────────────────────────────────────────────
// The closed set of builtin scalar types the codec encodes/decodes directly;
// every other key type is a struct key, expanded field-by-field on top of these
// leaves (see the ABI-aware key shapes below). A key field's ABI spelling is
// resolved to one of these kinds ONCE, when its key_shape is built, so the
// encode/decode paths switch on an enum instead of re-comparing type strings on
// every row. Several ABI spellings collapse to one kind (e.g. "float"/"float32",
// "double"/"float64", "long double"/"float128").
enum class key_leaf_kind {
   uint8, int8, uint16, int16, uint32, int32, uint64, int64,
   uint128, int128, checksum256,
   name, boolean, string,
   float32, float64, float128,
};

/// One ABI type spelling and the leaf kind it resolves to.
struct leaf_key_spelling {
   std::string_view spelling;
   key_leaf_kind    kind;
};

/// Single source of truth for which ABI spellings are codec leaves and which
/// kind each maps to. `leaf_kind_of` looks up this table, and `build_key_shape`
/// uses the result to decide leaf-vs-struct; a spelling absent here is treated
/// as a struct key. Multiple spellings may share a kind (the float aliases).
/// INVARIANT: every `key_leaf_kind` reachable from this table MUST have a case
/// in BOTH the encode_field and decode_field switches below. Those switches are
/// exhaustive over the enum, so under -Wswitch a new kind without a branch is a
/// compile error; the be_key_codec_tests `leaf_support_list_roundtrips` case
/// additionally round-trips every spelling here to pin the mapping at test time.
/// The array size is deduced from the initializer (via std::to_array) so adding
/// a spelling needs no count to be kept in sync.
inline constexpr auto leaf_key_spellings = std::to_array<leaf_key_spelling>({
   {"uint8",       key_leaf_kind::uint8},
   {"int8",        key_leaf_kind::int8},
   {"uint16",      key_leaf_kind::uint16},
   {"int16",       key_leaf_kind::int16},
   {"uint32",      key_leaf_kind::uint32},
   {"int32",       key_leaf_kind::int32},
   {"uint64",      key_leaf_kind::uint64},
   {"int64",       key_leaf_kind::int64},
   {"uint128",     key_leaf_kind::uint128},
   {"int128",      key_leaf_kind::int128},
   {"checksum256", key_leaf_kind::checksum256},
   {"name",        key_leaf_kind::name},
   {"bool",        key_leaf_kind::boolean},
   {"string",      key_leaf_kind::string},
   {"float32",     key_leaf_kind::float32},
   {"float",       key_leaf_kind::float32},
   {"float64",     key_leaf_kind::float64},
   {"double",      key_leaf_kind::float64},
   {"float128",    key_leaf_kind::float128},
   {"long double", key_leaf_kind::float128},
});

/// Resolve an (already typedef-resolved) ABI type spelling to its leaf kind, or
/// nullopt if it is not a codec leaf — i.e. it must be expanded as a struct key.
inline std::optional<key_leaf_kind> leaf_kind_of(std::string_view type) {
   for (const auto& e : leaf_key_spellings)
      if (e.spelling == type)
         return e.kind;
   return std::nullopt;
}

inline fc::variant decode_field(reader& r, key_leaf_kind kind) {
   switch (kind) {
   case key_leaf_kind::uint8:  return fc::variant(r.read_u8());
   case key_leaf_kind::int8:   return fc::variant(static_cast<int8_t>(r.read_u8() ^ 0x80u));
   case key_leaf_kind::uint16: return fc::variant(r.read_be16());
   case key_leaf_kind::int16:  return fc::variant(static_cast<int16_t>(r.read_be16() ^ 0x8000u));
   case key_leaf_kind::uint32: return fc::variant(r.read_be32());
   case key_leaf_kind::int32:  return fc::variant(static_cast<int32_t>(r.read_be32() ^ 0x80000000u));
   case key_leaf_kind::uint64: return fc::variant(r.read_be64());
   case key_leaf_kind::int64:  return fc::variant(static_cast<int64_t>(r.read_be64() ^ (uint64_t(1) << 63)));
   case key_leaf_kind::uint128: {
      // 16-byte big-endian, high quadword first — mirrors the uint64 BE layout.
      const uint64_t hi = r.read_be64();
      const uint64_t lo = r.read_be64();
      fc::variant v;
      fc::to_variant(fc::to_uint128(hi, lo), v);
      return v;
   }
   case key_leaf_kind::int128: {
      // Sign-flip the high quadword (same bias trick as int64) so signed values BE-sort.
      const uint64_t hi = r.read_be64() ^ (uint64_t(1) << 63);
      const uint64_t lo = r.read_be64();
      fc::variant v;
      fc::to_variant(static_cast<fc::int128>(fc::to_uint128(hi, lo)), v);
      return v;
   }
   case key_leaf_kind::checksum256: {
      // Raw 32 digest bytes in display order: fixed_bytes packs its words
      // big-endian, so CDT's to_key emits the canonical byte sequence and
      // memcmp order matches checksum256 ordering. Canonical hex spelling
      // via fc::sha256's variant conversion.
      fc::sha256 h;
      r.read_bytes(h.data(), h.data_size());
      fc::variant v;
      fc::to_variant(h, v);
      return v;
   }
   case key_leaf_kind::name:    return fc::variant(name(r.read_be64()).to_string());
   case key_leaf_kind::boolean: return fc::variant(r.read_u8() != 0);
   case key_leaf_kind::string:  return fc::variant(r.read_nul_escaped_string());
   case key_leaf_kind::float32: {
      uint32_t bits = r.read_be32();
      if (bits >> 31) bits ^= (uint32_t(1) << 31);
      else            bits = ~bits;
      float v; memcpy(&v, &bits, 4);
      return fc::variant(static_cast<double>(v));
   }
   case key_leaf_kind::float64: {
      uint64_t bits = r.read_be64();
      if (bits >> 63) bits ^= (uint64_t(1) << 63);
      else            bits = ~bits;
      double v; memcpy(&v, &bits, 8);
      return fc::variant(v);
   }
   case key_leaf_kind::float128: {
      // Same IEEE sort-order trick as float32/64 at 128 bits. SOURCE OF TRUTH:
      // these bytes were produced by CDT kv_multi_index::encode_secondary(long
      // double) (wire-cdt kv_multi_index.hpp) and stored in the row's secondary
      // key — this branch must invert that transform byte-for-byte. The e2e
      // float128 pagination in get_table_tests (sec-9) pins the agreement; a
      // change to CDT's long-double key encoding would break it there. Spelling
      // is fc's canonical float128_t form ("0x" + 16 LE hex bytes).
      const uint64_t hi_enc = r.read_be64();
      const uint64_t lo_enc = r.read_be64();
      fc::uint128 bits = fc::to_uint128(hi_enc, lo_enc);
      if (bits >> 127) bits ^= (fc::uint128(1) << 127);
      else             bits = ~bits;
      float128_t v;
      memcpy(&v, &bits, sizeof(v)); // little-endian platform assumption, as elsewhere in this file
      fc::variant out;
      fc::to_variant(v, out);
      return out;
   }
   }
   // Unreachable: the switch is exhaustive over key_leaf_kind and `kind` is only
   // ever produced by leaf_kind_of. Guards against a future kind added without a
   // decode_field case (also surfaced at compile time under -Wswitch).
   FC_ASSERT(false, "Unhandled BE key leaf kind in decode_field — add a case for every key_leaf_kind");
}

inline void encode_field(writer& w, key_leaf_kind kind, const fc::variant& val) {
   switch (kind) {
   case key_leaf_kind::uint8:  w.write_u8(static_cast<uint8_t>(val.as_uint64())); return;
   case key_leaf_kind::int8:   w.write_u8(static_cast<uint8_t>(static_cast<uint8_t>(val.as_int64()) ^ 0x80u)); return;
   case key_leaf_kind::uint16: w.write_be16(static_cast<uint16_t>(val.as_uint64())); return;
   case key_leaf_kind::int16:  w.write_be16(static_cast<uint16_t>(static_cast<uint16_t>(val.as_int64()) ^ 0x8000u)); return;
   case key_leaf_kind::uint32: w.write_be32(static_cast<uint32_t>(val.as_uint64())); return;
   case key_leaf_kind::int32:  w.write_be32(static_cast<uint32_t>(static_cast<uint32_t>(val.as_int64()) ^ 0x80000000u)); return;
   case key_leaf_kind::uint64: w.write_be64(val.as_uint64()); return;
   case key_leaf_kind::int64:  w.write_be64(static_cast<uint64_t>(val.as_int64()) ^ (uint64_t(1) << 63)); return;
   case key_leaf_kind::uint128: {
      // Accepts a native uint128 variant, a uint64 number, or a decimal/hex string
      // (the JSON spelling for values past 2^64) via fc::from_variant.
      fc::uint128 u = 0;
      fc::from_variant(val, u);
      w.write_be64(static_cast<uint64_t>(u >> 64));
      w.write_be64(static_cast<uint64_t>(u));
      return;
   }
   case key_leaf_kind::int128: {
      fc::int128 i = 0;
      fc::from_variant(val, i);
      const auto u = static_cast<fc::uint128>(i);
      w.write_be64(static_cast<uint64_t>(u >> 64) ^ (uint64_t(1) << 63));
      w.write_be64(static_cast<uint64_t>(u));
      return;
   }
   case key_leaf_kind::checksum256: {
      fc::sha256 h;
      fc::from_variant(val, h);
      w.write_bytes(h.data(), h.data_size());
      return;
   }
   case key_leaf_kind::name:    w.write_be64(name(val.as_string()).to_uint64_t()); return;
   case key_leaf_kind::boolean: w.write_u8(val.as_bool() ? 1 : 0); return;
   case key_leaf_kind::string:  w.write_nul_escaped_string(val.as_string()); return;
   case key_leaf_kind::float32: {
      float f = static_cast<float>(val.as_double());
      uint32_t bits; memcpy(&bits, &f, 4);
      if (bits >> 31) bits = ~bits;
      else            bits ^= (uint32_t(1) << 31);
      w.write_be32(bits); return;
   }
   case key_leaf_kind::float64: {
      double d = val.as_double();
      uint64_t bits; memcpy(&bits, &d, 8);
      if (bits >> 63) bits = ~bits;
      else            bits ^= (uint64_t(1) << 63);
      w.write_be64(bits); return;
   }
   case key_leaf_kind::float128: {
      // Inverse of the decode_field float128 branch; must reproduce CDT
      // kv_multi_index::encode_secondary(long double) byte-for-byte so a JSON
      // bound compares against the stored secondary key (see that branch for the
      // source-of-truth note).
      float128_t f;
      fc::from_variant(val, f);
      fc::uint128 bits;
      memcpy(&bits, &f, sizeof(bits)); // little-endian platform assumption, as elsewhere in this file
      if (bits >> 127) bits = ~bits;
      else             bits ^= (fc::uint128(1) << 127);
      w.write_be64(static_cast<uint64_t>(bits >> 64));
      w.write_be64(static_cast<uint64_t>(bits));
      return;
   }
   }
   // Unreachable: the switch is exhaustive over key_leaf_kind and `kind` is only
   // ever produced by leaf_kind_of. Guards against a future kind added without an
   // encode_field case (also surfaced at compile time under -Wswitch).
   FC_ASSERT(false, "Unhandled BE key leaf kind in encode_field — add a case for every key_leaf_kind");
}

// ── ABI-aware key shapes ────────────────────────────────────────────────────
// kv/multi_index keys are not limited to the builtin leaf types above: CDT's
// to_key reflects through typedefs and struct key types — e.g. `slug_name`
// (struct { value: uint64 }) is the primary key of the v6 registry tables
// (sysio.chains chains, sysio.tokens tokens/chaintokens, sysio.reserv
// reserves). A key_shape is the resolved encode/decode plan for one key
// field: a leaf with a codec-supported type, or a struct node whose children
// encode in declaration order (matching to_key's reflected-field walk). Leaf
// types and their kinds are defined above, with the codec (key_leaf_kind /
// leaf_key_spellings / leaf_kind_of).

/// Canonicalize abigen template spellings and chase ABI typedefs to a fixpoint.
/// A visited set of resolved typedef names makes an alias cycle (a -> b -> ...
/// -> a) a precise diagnostic instead of the generic "Unsupported BE key type"
/// the caller would otherwise report once the loop landed on the unresolved
/// alias. The set holds at most one entry per typedef, so the walk still
/// terminates in O(typedef count).
inline std::string resolve_key_type(const abi_def& abi, std::string type) {
   std::set<std::string> seen;
   for (;;) {
      if (type == "fixed_bytes<32>") { type = "checksum256"; } // checksum256-equivalent key spelling
      auto it = std::find_if(abi.types.begin(), abi.types.end(),
                             [&](const type_def& td) { return td.new_type_name == type; });
      if (it == abi.types.end())
         return type; // not (or no longer) a typedef — fully resolved
      FC_ASSERT(seen.insert(type).second, "Typedef cycle while resolving BE key type '{}'", type);
      type = it->type;
   }
}

/// Maximum struct-key nesting depth. Bounds build_key_shape's recursion so a
/// self-referential struct definition (a key struct with a field of its own
/// type) is rejected with a clear error instead of overflowing the stack.
inline constexpr size_t max_key_struct_depth = 8;

/// Resolved encode/decode plan for one key field.
struct key_shape {
   std::string            name;            ///< field name (bound-object key / decoded-object key)
   std::string            type;            ///< resolved type — leaf spelling (diagnostics) or struct name for nodes
   bool                   is_leaf = false; ///< true → encode/decode via the leaf codec (dispatch on `kind`); false → struct node (recurse children)
   key_leaf_kind          kind{};          ///< leaf codec dispatch discriminant; meaningful only when is_leaf
   std::vector<key_shape> children;        ///< struct field shapes in declaration (encode) order
};

/// Build the shape for one declared key field, expanding struct key types
/// through the ABI. Struct bases are rejected (CDT's reflected to_key walks
/// only the declared fields, so a based key struct has no defined order here).
inline key_shape build_key_shape(const abi_def& abi, const std::string& field_name,
                                 const std::string& declared_type, size_t depth = 0) {
   FC_ASSERT(depth <= max_key_struct_depth, "BE key struct nesting too deep at field '{}'", field_name);
   key_shape shape;
   shape.name = field_name;
   shape.type = resolve_key_type(abi, declared_type);
   if (auto kind = leaf_kind_of(shape.type)) {
      shape.is_leaf = true;
      shape.kind    = *kind;
      return shape;
   }
   // Not a leaf: a struct node. A zero-field struct stays a node with no
   // children and encodes to zero bytes, matching to_key's reflected walk.
   auto it = std::find_if(abi.structs.begin(), abi.structs.end(),
                          [&](const struct_def& sd) { return sd.name == shape.type; });
   FC_ASSERT(it != abi.structs.end(), "Unsupported BE key type: {}", shape.type);
   FC_ASSERT(it->base.empty(), "BE key struct '{}' with a base is not supported", shape.type);
   shape.children.reserve(it->fields.size());
   for (const auto& f : it->fields)
      shape.children.push_back(build_key_shape(abi, f.name, f.type, depth + 1));
   return shape;
}

/// Build shapes for a table's full key field list (ABI key_names/key_types).
inline std::vector<key_shape> build_key_shapes(const abi_def& abi,
                                               const vector<std::string>& key_names,
                                               const vector<std::string>& key_types) {
   FC_ASSERT(key_names.size() == key_types.size(), "ABI key_names/key_types size mismatch");
   std::vector<key_shape> shapes;
   shapes.reserve(key_names.size());
   for (size_t i = 0; i < key_names.size(); ++i)
      shapes.push_back(build_key_shape(abi, key_names[i], key_types[i]));
   return shapes;
}

/// Encode one key field per its shape: a leaf goes straight to encode_field; a
/// struct node reads each child by name from `val` (a JSON object) and encodes
/// the children in declaration order, matching to_key's reflected-field walk.
inline void encode_shape(writer& w, const key_shape& shape, const fc::variant& val) {
   if (shape.is_leaf) {
      encode_field(w, shape.kind, val);
      return;
   }
   const auto& obj = val.get_object();
   for (const auto& child : shape.children) {
      auto it = obj.find(child.name);
      FC_ASSERT(it != obj.end(), "Key field '{}' not found in bound object for struct '{}'",
                child.name, shape.type);
      encode_shape(w, child, it->value());
   }
}

/// Decode one key field per its shape: a leaf returns the decoded scalar; a
/// struct node returns a nested object of its children, decoded in declaration
/// order (the inverse of encode_shape).
inline fc::variant decode_shape(reader& r, const key_shape& shape) {
   if (shape.is_leaf)
      return decode_field(r, shape.kind);
   fc::mutable_variant_object obj;
   for (const auto& child : shape.children)
      obj(child.name, decode_shape(r, child));
   return fc::variant(std::move(obj));
}

inline fc::variant decode_key(const char* data, size_t size, const std::vector<key_shape>& shapes) {
   reader r(data, size);
   fc::mutable_variant_object obj;
   for (const auto& shape : shapes)
      obj(shape.name, decode_shape(r, shape));
   FC_ASSERT(r.remaining() == 0, "BE key has {} trailing bytes after decoding all fields", r.remaining());
   return fc::variant(std::move(obj));
}

inline std::vector<char> encode_key(const fc::variant& key_var, const std::vector<key_shape>& shapes) {
   const auto& obj = key_var.get_object();
   writer w;
   for (const auto& shape : shapes) {
      auto it = obj.find(shape.name);
      FC_ASSERT(it != obj.end(), "Key field '{}' not found in bound object", shape.name);
      encode_shape(w, shape, it->value());
   }
   return w.release();
}

} // namespace be_key_codec

}

namespace fc {

   // overloads for to/from_variant
   template<typename OidType>
   void to_variant( const chainbase::oid<OidType>& oid, variant& v ) {
      v = variant(oid._id);
   }

   template<typename OidType>
   void from_variant( const variant& v, chainbase::oid<OidType>& oid ) {
      from_variant(v, oid._id);
   }

   inline
   void float64_to_double (const softfloat64_t& f, double& d) {
      memcpy(&d, &f, sizeof(d));
   }

   inline
   void double_to_float64 (const double& d, softfloat64_t& f) {
      memcpy(&f, &d, sizeof(f));
   }

   inline
   void float128_to_uint128 (const softfloat128_t& f, sysio::chain::uint128_t& u) {
      memcpy(&u, &f, sizeof(u));
   }

   inline
   void uint128_to_float128 (const sysio::chain::uint128_t& u,  softfloat128_t& f) {
      memcpy(&f, &u, sizeof(f));
   }

   inline
   void to_variant( const softfloat64_t& f, variant& v ) {
      double double_f;
      float64_to_double(f, double_f);
      v = variant(double_f);
   }

   inline
   void from_variant( const variant& v, softfloat64_t& f ) {
      double double_f;
      from_variant(v, double_f);
      double_to_float64(double_f, f);
   }

   inline
   void to_variant( const softfloat128_t& f, variant& v ) {
      // Assumes platform is little endian and hex representation of 128-bit integer is in little endian order.	
      char as_bytes[sizeof(sysio::chain::uint128_t)];
      memcpy(as_bytes, &f, sizeof(as_bytes));
      std::string s = "0x";	
      s.append( to_hex( as_bytes, sizeof(as_bytes) ) );
      v = s;
   }

   inline
   void from_variant( const variant& v, softfloat128_t& f ) {
      // Temporarily hold the binary in uint128_t before casting it to softfloat128_t
      char temp[sizeof(sysio::chain::uint128_t)];
      memset(temp, 0, sizeof(temp));
      auto s = v.as_string();
      FC_ASSERT( s.size() == 2 + 2 * sizeof(temp) && s.find("0x") == 0,
                 "Failure in converting hex data into a softfloat128_t" );
      auto sz = from_hex( s.substr(2), temp, sizeof(temp) );
      // Assumes platform is little endian and hex representation of 128-bit integer is in little endian order.
      FC_ASSERT( sz == sizeof(temp), "Failure in converting hex data into a softfloat128_t" );
      memcpy(&f, temp, sizeof(f));
   }

   inline
   void to_variant( const sysio::chain::shared_string& s, variant& v ) {
      v = variant(std::string(s.begin(), s.end()));
   }

   inline
   void from_variant( const variant& v, sysio::chain::shared_string& s ) {
      std::string _s;
      from_variant(v, _s);
      s = _s;
   }

   inline
   void to_variant( const sysio::chain::shared_blob& b, variant& v ) {
      v = variant(base64_encode(b.data(), b.size()));
   }

   inline
   void from_variant( const variant& v, sysio::chain::shared_blob& b ) {
      std::vector<char> b64 = base64_decode(v.as_string());
      b = std::string_view(b64.data(), b64.size());
   }

   template<typename T>
   void to_variant( const sysio::chain::shared_vector<T>& sv, variant& v ) {
      to_variant(std::vector<T>(sv.begin(), sv.end()), v);
   }

   template<typename T>
   void from_variant( const variant& v, sysio::chain::shared_vector<T>& sv ) {
      std::vector<T> _v;
      from_variant(v, _v);
      sv = v;
   }

   inline
   void to_variant(const sysio::chain::detail::snapshot_key_value_object& a, fc::variant& v) {
      v = fc::mutable_variant_object("primary_key", a.primary_key)
                                    ("payer", a.payer)
                                    ("value", base64_encode(a.value.data(), a.value.size()));
   }

   inline
   void from_variant(const fc::variant& v, sysio::chain::detail::snapshot_key_value_object& a) {
      from_variant(v["primary_key"], a.primary_key);
      from_variant(v["payer"], a.payer);
      a.value = base64_decode(v["value"].as_string());
   }
}

namespace chainbase {
   // overloads for OID packing
   template<typename DataStream, typename OidType>
   DataStream& operator << ( DataStream& ds, const oid<OidType>& oid ) {
      fc::raw::pack(ds, oid._id);
      return ds;
   }

   template<typename DataStream, typename OidType>
   DataStream& operator >> ( DataStream& ds, oid<OidType>& oid ) {
      fc::raw::unpack(ds, oid._id);
      return ds;
   }

   // chainbase::shared_cow_string
   // ----------------------------
   template<typename Stream>
   inline Stream& operator<<(Stream& s, const chainbase::shared_cow_string& v)  {
      FC_ASSERT(v.size() <= MAX_NUM_ARRAY_ELEMENTS);
      fc::raw::pack(s, fc::unsigned_int((uint32_t)v.size()));
      if( v.size() )
         s.write((const char*)v.data(), v.size());
      return s;
   }

   template<typename Stream>
   inline Stream& operator>>(Stream& s, chainbase::shared_cow_string& v)  {
      fc::unsigned_int sz;
      fc::raw::unpack(s, sz);
      FC_ASSERT(sz.value <= MAX_SIZE_OF_BYTE_ARRAYS);
      if (sz) {
         v.resize_and_fill(sz, [&](char* buf, std::size_t sz) {
            s.read(buf, sz);
         });
      }
      return s;
   }

   // chainbase::shared_cow_vector
   // ----------------------------
   template<typename Stream, typename T>
   inline Stream& operator<<(Stream& s, const chainbase::shared_cow_vector<T>& v)  {
      FC_ASSERT(v.size() <= MAX_NUM_ARRAY_ELEMENTS);
      fc::raw::pack( s, fc::unsigned_int((uint32_t)v.size()));
      for (const auto& el : v)
         fc::raw::pack(s, el);
      return s;
   }

   template<typename Stream, typename T>
   inline Stream& operator>>(Stream& s, chainbase::shared_cow_vector<T>& v)  {
      fc::unsigned_int size;
      fc::raw::unpack( s, size );
      FC_ASSERT(size.value  <= MAX_NUM_ARRAY_ELEMENTS);
      FC_ASSERT(v.size() == 0);
      v.clear_and_construct(size.value, 0, [&](auto* dest, std::size_t i) {
         new (dest) T(); // unpack expects a constructed variable
         fc::raw::unpack(s, *dest);
      });
      return s;
   }

   // chainbase::shared_cow_vector<char>
   // ----------------------------------
   template<typename Stream, typename T>
   inline Stream& operator<<(Stream& s, const chainbase::shared_cow_vector<char>& v)  {
      FC_ASSERT(v.size() <= MAX_NUM_ARRAY_ELEMENTS);
      fc::raw::pack( s, fc::unsigned_int((uint32_t)v.size()));
      if( v.size() )
         s.write((const char*)v.data(), v.size());
      return s;
   }
}

// overloads for softfloat packing
template<typename DataStream>
DataStream& operator << ( DataStream& ds, const softfloat64_t& v ) {
   double double_v;
   fc::float64_to_double(v, double_v);
   fc::raw::pack(ds, double_v);
   return ds;
}

template<typename DataStream>
DataStream& operator >> ( DataStream& ds, softfloat64_t& v ) {
   double double_v;
   fc::raw::unpack(ds, double_v);
   fc::double_to_float64(double_v, v);
   return ds;
}

template<typename DataStream>
DataStream& operator << ( DataStream& ds, const softfloat128_t& v ) {
   sysio::chain::uint128_t uint128_v;
   fc::float128_to_uint128(v, uint128_v);
   fc::raw::pack(ds, uint128_v);
   return ds;
}

template<typename DataStream>
DataStream& operator >> ( DataStream& ds, softfloat128_t& v ) {
   sysio::chain::uint128_t uint128_v;
   fc::raw::unpack(ds, uint128_v);
   fc::uint128_to_float128(uint128_v, v);
   return ds;
}
