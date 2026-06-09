#pragma once

#include <sysio/chain/types.hpp>
#include <fc/io/raw.hpp>
#include <fc/crypto/base64.hpp>
#include <softfloat/softfloat.hpp>

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
// Used by get_table_rows API to decode/encode format=0 raw keys.
// Supports: uint8, int8, uint16, int16, uint32, int32, uint64, int64,
//           name, bool, string, float64/double.
// ---------------------------------------------------------------------------
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

inline fc::variant decode_field(reader& r, const std::string& type) {
   if (type == "uint8")         return fc::variant(r.read_u8());
   if (type == "int8")          return fc::variant(static_cast<int8_t>(r.read_u8() ^ 0x80u));
   if (type == "uint16")        return fc::variant(r.read_be16());
   if (type == "int16")         return fc::variant(static_cast<int16_t>(r.read_be16() ^ 0x8000u));
   if (type == "uint32")        return fc::variant(r.read_be32());
   if (type == "int32")         return fc::variant(static_cast<int32_t>(r.read_be32() ^ 0x80000000u));
   if (type == "uint64")        return fc::variant(r.read_be64());
   if (type == "int64")         return fc::variant(static_cast<int64_t>(r.read_be64() ^ (uint64_t(1) << 63)));
   if (type == "name")          return fc::variant(name(r.read_be64()).to_string());
   if (type == "bool")          return fc::variant(r.read_u8() != 0);
   if (type == "string")        return fc::variant(r.read_nul_escaped_string());
   if (type == "float32" || type == "float") {
      uint32_t bits = r.read_be32();
      if (bits >> 31) bits ^= (uint32_t(1) << 31);
      else            bits = ~bits;
      float v; memcpy(&v, &bits, 4);
      return fc::variant(static_cast<double>(v));
   }
   if (type == "float64" || type == "double") {
      uint64_t bits = r.read_be64();
      if (bits >> 63) bits ^= (uint64_t(1) << 63);
      else            bits = ~bits;
      double v; memcpy(&v, &bits, 8);
      return fc::variant(v);
   }
   FC_ASSERT(false, "Unsupported BE key type: {}", type);
}

inline void encode_field(writer& w, const std::string& type, const fc::variant& val) {
   if (type == "uint8")         { w.write_u8(static_cast<uint8_t>(val.as_uint64())); return; }
   if (type == "int8")          { w.write_u8(static_cast<uint8_t>(static_cast<uint8_t>(val.as_int64()) ^ 0x80u)); return; }
   if (type == "uint16")        { w.write_be16(static_cast<uint16_t>(val.as_uint64())); return; }
   if (type == "int16")         { w.write_be16(static_cast<uint16_t>(static_cast<uint16_t>(val.as_int64()) ^ 0x8000u)); return; }
   if (type == "uint32")        { w.write_be32(static_cast<uint32_t>(val.as_uint64())); return; }
   if (type == "int32")         { w.write_be32(static_cast<uint32_t>(static_cast<uint32_t>(val.as_int64()) ^ 0x80000000u)); return; }
   if (type == "uint64")        { w.write_be64(val.as_uint64()); return; }
   if (type == "int64")         { w.write_be64(static_cast<uint64_t>(val.as_int64()) ^ (uint64_t(1) << 63)); return; }
   if (type == "name")          { w.write_be64(name(val.as_string()).to_uint64_t()); return; }
   if (type == "bool")          { w.write_u8(val.as_bool() ? 1 : 0); return; }
   if (type == "string")        { w.write_nul_escaped_string(val.as_string()); return; }
   if (type == "float32" || type == "float") {
      float f = static_cast<float>(val.as_double());
      uint32_t bits; memcpy(&bits, &f, 4);
      if (bits >> 31) bits = ~bits;
      else            bits ^= (uint32_t(1) << 31);
      w.write_be32(bits); return;
   }
   if (type == "float64" || type == "double") {
      double d = val.as_double();
      uint64_t bits; memcpy(&bits, &d, 8);
      if (bits >> 63) bits = ~bits;
      else            bits ^= (uint64_t(1) << 63);
      w.write_be64(bits); return;
   }
   FC_ASSERT(false, "Unsupported BE key type: {}", type);
}

inline fc::variant decode_key(const char* data, size_t size,
                              const vector<std::string>& key_names,
                              const vector<std::string>& key_types) {
   FC_ASSERT(key_names.size() == key_types.size(), "ABI key_names/key_types size mismatch");
   reader r(data, size);
   fc::mutable_variant_object obj;
   for (size_t i = 0; i < key_names.size(); ++i) {
      obj(key_names[i], decode_field(r, key_types[i]));
   }
   FC_ASSERT(r.remaining() == 0, "BE key has {} trailing bytes after decoding all fields", r.remaining());
   return fc::variant(std::move(obj));
}

inline std::vector<char> encode_key(const fc::variant& key_var,
                                    const vector<std::string>& key_names,
                                    const vector<std::string>& key_types) {
   FC_ASSERT(key_names.size() == key_types.size(), "ABI key_names/key_types size mismatch");
   const auto& obj = key_var.get_object();
   writer w;
   for (size_t i = 0; i < key_names.size(); ++i) {
      auto it = obj.find(key_names[i]);
      FC_ASSERT(it != obj.end(), "Key field '{}' not found in bound object", key_names[i]);
      encode_field(w, key_types[i], it->value());
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
   void float64_to_double (const float64_t& f, double& d) {
      memcpy(&d, &f, sizeof(d));
   }

   inline
   void double_to_float64 (const double& d, float64_t& f) {
      memcpy(&f, &d, sizeof(f));
   }

   inline
   void float128_to_uint128 (const float128_t& f, sysio::chain::uint128_t& u) {
      memcpy(&u, &f, sizeof(u));
   }

   inline
   void uint128_to_float128 (const sysio::chain::uint128_t& u,  float128_t& f) {
      memcpy(&f, &u, sizeof(f));
   }

   inline
   void to_variant( const float64_t& f, variant& v ) {
      double double_f;
      float64_to_double(f, double_f);
      v = variant(double_f);
   }

   inline
   void from_variant( const variant& v, float64_t& f ) {
      double double_f;
      from_variant(v, double_f);
      double_to_float64(double_f, f);
   }

   inline
   void to_variant( const float128_t& f, variant& v ) {
      // Assumes platform is little endian and hex representation of 128-bit integer is in little endian order.	
      char as_bytes[sizeof(sysio::chain::uint128_t)];
      memcpy(as_bytes, &f, sizeof(as_bytes));
      std::string s = "0x";	
      s.append( to_hex( as_bytes, sizeof(as_bytes) ) );
      v = s;
   }

   inline
   void from_variant( const variant& v, float128_t& f ) {
      // Temporarily hold the binary in uint128_t before casting it to float128_t
      char temp[sizeof(sysio::chain::uint128_t)];
      memset(temp, 0, sizeof(temp));
      auto s = v.as_string();	
      FC_ASSERT( s.size() == 2 + 2 * sizeof(temp) && s.find("0x") == 0,	"Failure in converting hex data into a float128_t");	
      auto sz = from_hex( s.substr(2), temp, sizeof(temp) );
      // Assumes platform is little endian and hex representation of 128-bit integer is in little endian order.	
      FC_ASSERT( sz == sizeof(temp), "Failure in converting hex data into a float128_t" );	
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
DataStream& operator << ( DataStream& ds, const float64_t& v ) {
   double double_v;
   fc::float64_to_double(v, double_v);
   fc::raw::pack(ds, double_v);
   return ds;
}

template<typename DataStream>
DataStream& operator >> ( DataStream& ds, float64_t& v ) {
   double double_v;
   fc::raw::unpack(ds, double_v);
   fc::double_to_float64(double_v, v);
   return ds;
}

template<typename DataStream>
DataStream& operator << ( DataStream& ds, const float128_t& v ) {
   sysio::chain::uint128_t uint128_v;
   fc::float128_to_uint128(v, uint128_v);
   fc::raw::pack(ds, uint128_v);
   return ds;
}

template<typename DataStream>
DataStream& operator >> ( DataStream& ds, float128_t& v ) {
   sysio::chain::uint128_t uint128_v;
   fc::raw::unpack(ds, uint128_v);
   fc::uint128_to_float128(uint128_v, v);
   return ds;
}
