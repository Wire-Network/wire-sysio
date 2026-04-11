#pragma once

#include <sysio/chain/database_utils.hpp>
#include <sysio/chain/multi_index_includes.hpp>
#include <sysio/chain/config.hpp>
#include <string_view>
#include <cstring>

namespace sysio { namespace chain {

   /**
    * Transparent comparator for shared_blob that supports heterogeneous lookup
    * with std::string_view. Used for secondary index key fields.
    */
   struct shared_blob_less {
      using is_transparent = void;

      bool operator()(const shared_blob& a, const shared_blob& b) const {
         return std::string_view(a.data(), a.size()) < std::string_view(b.data(), b.size());
      }
      bool operator()(const shared_blob& a, std::string_view b) const {
         return std::string_view(a.data(), a.size()) < b;
      }
      bool operator()(std::string_view a, const shared_blob& b) const {
         return a < std::string_view(b.data(), b.size());
      }
   };

   /**
    * @brief Primary KV storage object.
    *
    * Stores arbitrary variable-length keys and values as shared_blobs in
    * chainbase shared memory. key_size and value_size are billed separately
    * at billing time on top of the fixed struct overhead.
    *
    * table_id is a uint16 namespace identifier (DJB2 hash of the table name
    * string, truncated to 16 bits). Each table and each secondary index gets
    * a unique table_id, providing automatic partition in the composite index
    * with zero per-row key overhead.
    */
   class kv_object : public chainbase::object<kv_object_type, kv_object> {
      OBJECT_CTOR(kv_object, (key)(value))

   public:
      id_type        id;
      account_name   code;          ///< contract account owning this row
      account_name   payer;         ///< RAM payer (default=code, privileged contracts can set to other accounts)
      shared_blob    key;           ///< primary key bytes (opaque, layout determined by CDT)
      shared_blob    value;         ///< arbitrary byte value
      uint16_t       table_id = 0;  ///< table namespace (DJB2 hash of table name % 65536)

      std::string_view key_view() const {
         return {key.data(), key.size()};
      }
   };

   /// Key extractor returning string_view from the shared_blob key.
   struct kv_key_extractor {
      using result_type = std::string_view;
      result_type operator()(const kv_object& o) const {
         return o.key_view();
      }
   };

   /// Load 8 big-endian bytes as a host-order uint64_t for integer comparison.
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
   inline uint64_t kv_load_be64(const char* p) {
      uint64_t v; memcpy(&v, p, 8); return __builtin_bswap64(v);
   }
#else
   inline uint64_t kv_load_be64(const char* p) {
      uint64_t v; memcpy(&v, p, 8); return v;
   }
#endif

   /// Fast key comparator with integer fast-path for 8-byte keys.
   /// For 8 bytes, lexicographic byte comparison == big-endian uint64_t comparison.
   /// Load + convert-to-host + integer cmp is ~1 instruction vs memcmp's byte loop.
   struct kv_key_less {
      using is_transparent = void;

      bool operator()(std::string_view a, std::string_view b) const {
         if (__builtin_expect(a.size() == 8 && b.size() == 8, 1)) {
            return kv_load_be64(a.data()) < kv_load_be64(b.data());
         }
         return a < b;
      }
   };

   struct by_code_key;

   using kv_index = chainbase::shared_multi_index_container<
      kv_object,
      indexed_by<
         ordered_unique<tag<by_id>,
            member<kv_object, kv_object::id_type, &kv_object::id>
         >,
         ordered_unique<tag<by_code_key>,
            composite_key<kv_object,
               member<kv_object, account_name, &kv_object::code>,
               member<kv_object, uint16_t, &kv_object::table_id>,
               kv_key_extractor
            >,
            composite_key_compare<std::less<account_name>, std::less<uint16_t>, kv_key_less>
         >
      >
   >;

   /**
    * @brief Unified secondary index object. Replaces all 5 legacy secondary index types
    *        (index64, index128, index256, index_double, index_long_double).
    *
    * Uses shared_blob for both key fields. sec_key_size and pri_key_size are
    * billed separately at billing time on top of the fixed struct overhead.
    *
    * table_id identifies this secondary index's namespace (DJB2 hash of
    * "tablename.indexname" % 65536). Each secondary index gets its own
    * table_id, separate from the primary table's table_id.
    */
   class kv_index_object : public chainbase::object<kv_index_object_type, kv_index_object> {
      OBJECT_CTOR(kv_index_object, (sec_key)(pri_key))

   public:
      id_type        id;
      account_name   code;
      account_name   payer;         ///< RAM payer (mirrors kv_object::payer)
      shared_blob    sec_key;       ///< secondary key bytes
      shared_blob    pri_key;       ///< primary key bytes
      uint16_t       table_id = 0;  ///< secondary index namespace (DJB2 hash of "table.index" % 65536)

      std::string_view sec_key_view() const { return {sec_key.data(), sec_key.size()}; }
      std::string_view pri_key_view() const { return {pri_key.data(), pri_key.size()}; }
   };

   /// Key extractors for kv_index_object shared_blob fields.
   struct kv_sec_key_extractor {
      using result_type = std::string_view;
      result_type operator()(const kv_index_object& o) const { return o.sec_key_view(); }
   };

   struct kv_pri_key_extractor {
      using result_type = std::string_view;
      result_type operator()(const kv_index_object& o) const { return o.pri_key_view(); }
   };

   struct by_code_table_id_seckey;

   using kv_index_index = chainbase::shared_multi_index_container<
      kv_index_object,
      indexed_by<
         ordered_unique<tag<by_id>,
            member<kv_index_object, kv_index_object::id_type, &kv_index_object::id>
         >,
         ordered_unique<tag<by_code_table_id_seckey>,
            composite_key<kv_index_object,
               member<kv_index_object, account_name, &kv_index_object::code>,
               member<kv_index_object, uint16_t,     &kv_index_object::table_id>,
               kv_sec_key_extractor,
               kv_pri_key_extractor
            >,
            composite_key_compare<std::less<account_name>, std::less<uint16_t>,
                                  kv_key_less, kv_key_less>
         >
      >
   >;

namespace config {
   template<>
   struct billable_size<kv_object> {
      static const uint64_t overhead = overhead_per_row_per_index_ram_bytes * 2;  ///< 2 indices: by_id, by_code_key
      // Fixed fields: 8 id + 8 code + 8 payer + 8 key (offset_ptr) + 8 value (offset_ptr)
      //             + 2 table_id + 6 padding = 48
      // key.size() and value.size() are added separately at billing time.
      static const uint64_t value = 48 + overhead;
   };

   template<>
   struct billable_size<kv_index_object> {
      static const uint64_t overhead = overhead_per_row_per_index_ram_bytes * 2;  ///< 2 indices: by_id, by_code_table_id_seckey
      // Fixed fields: 8 id + 8 code + 8 payer + 8 sec_key (offset_ptr)
      //             + 8 pri_key (offset_ptr) + 2 table_id + 6 padding = 48
      // sec_key.size() and pri_key.size() are added separately at billing time.
      static const uint64_t value = 48 + overhead;
   };
} // namespace config

   // ------------------------------------------------------------------
   // Snapshot DTO structs — portable serialization via FC_REFLECT.
   // These are transient (never stored in chainbase), so plain vectors
   // are used instead of shared_blob.
   // ------------------------------------------------------------------
   struct snapshot_kv_object {
      account_name        code;
      account_name        payer;
      std::vector<char>   key;
      std::vector<char>   value;
      uint16_t            table_id = 0;
   };

   struct snapshot_kv_index_object {
      account_name        code;
      account_name        payer;
      std::vector<char>   sec_key;
      std::vector<char>   pri_key;
      uint16_t            table_id = 0;
   };

} } // namespace sysio::chain

CHAINBASE_SET_INDEX_TYPE(sysio::chain::kv_object, sysio::chain::kv_index)
CHAINBASE_SET_INDEX_TYPE(sysio::chain::kv_index_object, sysio::chain::kv_index_index)

FC_REFLECT(sysio::chain::snapshot_kv_object, (code)(payer)(key)(value)(table_id))
FC_REFLECT(sysio::chain::snapshot_kv_index_object, (code)(payer)(sec_key)(pri_key)(table_id))
