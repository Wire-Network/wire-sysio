#pragma once

#include <sysio/chain/database_utils.hpp>
#include <sysio/chain/multi_index_includes.hpp>
#include <sysio/chain/config.hpp>
#include <string_view>
#include <cstring>

namespace sysio { namespace chain {

   /// SSO capacity for inline key storage. Covers uint64 (8B), name (8B),
   /// name+uint64 composites (16B), and uint128 (16B) without heap allocation.
   inline constexpr uint32_t kv_key_sso_capacity = 24;

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
    * @brief Primary KV storage object with SSO (small-string optimization) for keys.
    *
    * Keys <= kv_key_sso_capacity bytes are stored inline, avoiding heap allocation
    * and pointer indirection. Larger keys fall back to shared_blob (key_heap).
    * This matches legacy uint64_t key performance for the common case while
    * supporting arbitrary byte keys.
    */
   class kv_object : public chainbase::object<kv_object_type, kv_object> {
      OBJECT_CTOR(kv_object, (key_heap)(value))

   public:
      id_type        id;
      account_name   code;          ///< contract account owning this row
      account_name   payer;         ///< RAM payer (default=code, privileged contracts can set to other accounts)
      shared_blob    key_heap;      ///< heap storage for keys > kv_key_sso_capacity
      shared_blob    value;         ///< arbitrary byte value
      char           key_inline[kv_key_sso_capacity] = {}; ///< SSO buffer for small keys
      uint16_t       key_size = 0;  ///< actual key size in bytes (0..256)
      uint8_t        key_format = 0;///< 0=raw, 1=standard [table:8B][scope:8B][pk:8B]

      const char* key_data() const {
         return key_size <= kv_key_sso_capacity ? key_inline : key_heap.data();
      }

      std::string_view key_view() const {
         return {key_data(), key_size};
      }

      void key_assign(const char* d, uint32_t s) {
         key_size = static_cast<uint16_t>(s);
         if (s <= kv_key_sso_capacity) {
            if (s > 0) memcpy(key_inline, d, s);
            if (s < kv_key_sso_capacity) memset(key_inline + s, 0, kv_key_sso_capacity - s);
         } else {
            key_heap.assign(d, s);
         }
      }
   };

   /// Key extractor returning string_view — works for both SSO and heap paths.
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
               member<kv_object, uint8_t, &kv_object::key_format>,
               kv_key_extractor
            >,
            composite_key_compare<std::less<account_name>, std::less<uint8_t>, kv_key_less>
         >
      >
   >;

   /**
    * @brief Unified secondary index object. Replaces all 5 legacy secondary index types
    *        (index64, index128, index256, index_double, index_long_double).
    *
    * Uses shared_blob for both key fields (secondary indices commonly have
    * variable-length keys). SSO could be added here in a follow-up if needed.
    */
   class kv_index_object : public chainbase::object<kv_index_object_type, kv_index_object> {
      OBJECT_CTOR(kv_index_object, (sec_key_heap)(pri_key_heap))

   public:
      id_type        id;
      account_name   code;
      account_name   payer;         ///< RAM payer (mirrors kv_object::payer)
      name           table;
      shared_blob    sec_key_heap;  ///< heap storage for secondary keys > SSO capacity
      shared_blob    pri_key_heap;  ///< heap storage for primary keys > SSO capacity
      char           sec_key_inline[kv_key_sso_capacity] = {}; ///< SSO buffer for secondary keys
      char           pri_key_inline[kv_key_sso_capacity] = {}; ///< SSO buffer for primary keys
      uint16_t       sec_key_size = 0;
      uint16_t       pri_key_size = 0;
      uint8_t        index_id = 0;

      const char* sec_key_data() const { return sec_key_size <= kv_key_sso_capacity ? sec_key_inline : sec_key_heap.data(); }
      std::string_view sec_key_view() const { return {sec_key_data(), sec_key_size}; }
      void sec_key_assign(const char* d, uint32_t s) {
         sec_key_size = static_cast<uint16_t>(s);
         if (s <= kv_key_sso_capacity) {
            if (s > 0) memcpy(sec_key_inline, d, s);
            if (s < kv_key_sso_capacity) memset(sec_key_inline + s, 0, kv_key_sso_capacity - s);
         } else {
            sec_key_heap.assign(d, s);
         }
      }

      const char* pri_key_data() const { return pri_key_size <= kv_key_sso_capacity ? pri_key_inline : pri_key_heap.data(); }
      std::string_view pri_key_view() const { return {pri_key_data(), pri_key_size}; }
      void pri_key_assign(const char* d, uint32_t s) {
         pri_key_size = static_cast<uint16_t>(s);
         if (s <= kv_key_sso_capacity) {
            if (s > 0) memcpy(pri_key_inline, d, s);
            if (s < kv_key_sso_capacity) memset(pri_key_inline + s, 0, kv_key_sso_capacity - s);
         } else {
            pri_key_heap.assign(d, s);
         }
      }
   };

   // Key extractors for kv_index_object SSO fields
   struct kv_sec_key_extractor {
      using result_type = std::string_view;
      result_type operator()(const kv_index_object& o) const { return o.sec_key_view(); }
   };

   struct kv_pri_key_extractor {
      using result_type = std::string_view;
      result_type operator()(const kv_index_object& o) const { return o.pri_key_view(); }
   };

   struct by_code_table_idx_seckey;
   struct by_code_table_idx_prikey;

   using kv_index_index = chainbase::shared_multi_index_container<
      kv_index_object,
      indexed_by<
         ordered_unique<tag<by_id>,
            member<kv_index_object, kv_index_object::id_type, &kv_index_object::id>
         >,
         ordered_unique<tag<by_code_table_idx_seckey>,
            composite_key<kv_index_object,
               member<kv_index_object, account_name,  &kv_index_object::code>,
               member<kv_index_object, name,           &kv_index_object::table>,
               member<kv_index_object, uint8_t,        &kv_index_object::index_id>,
               kv_sec_key_extractor,
               kv_pri_key_extractor
            >,
            composite_key_compare<std::less<account_name>, std::less<name>, std::less<uint8_t>,
                                  kv_key_less, kv_key_less>
         >,
         ordered_unique<tag<by_code_table_idx_prikey>,
            composite_key<kv_index_object,
               member<kv_index_object, account_name,  &kv_index_object::code>,
               member<kv_index_object, name,           &kv_index_object::table>,
               member<kv_index_object, uint8_t,        &kv_index_object::index_id>,
               kv_pri_key_extractor
            >,
            composite_key_compare<std::less<account_name>, std::less<name>, std::less<uint8_t>,
                                  kv_key_less>
         >
      >
   >;

namespace config {
   template<>
   struct billable_size<kv_object> {
      static const uint64_t overhead = overhead_per_row_per_index_ram_bytes * 2;  ///< 2 indices: by_id, by_code_key
      // Fixed fields: 8 id + 8 code + 8 payer + 1 key_format + 2 key_size + 24 key_inline = 51
      // shared_blob headers: ~12 each for key_heap + value = 24
      // Struct padding/alignment: 5
      // Total fixed: 80.  key_size and value_size are added separately at billing time.
      static const uint64_t value = 80 + overhead;
   };

   template<>
   struct billable_size<kv_index_object> {
      static const uint64_t overhead = overhead_per_row_per_index_ram_bytes * 3;  ///< 3 indices: by_id, by_code_table_idx_seckey, by_code_table_idx_prikey
      // Reordered for minimal padding: 8-byte fields first, then shared_blobs (16B each),
      // then char arrays (1B align), then uint16s, then uint8.
      // Fixed fields: 8 code + 8 payer + 8 table + 16 sec_key_heap + 16 pri_key_heap
      //             + 24 sec_key_inline + 24 pri_key_inline + 2 sec_key_size + 2 pri_key_size
      //             + 1 index_id + 3 padding = 112.  Measured sizeof (excl id) = 112.
      // sec_key_size and pri_key_size data are added separately at billing time.
      static const uint64_t value = 112 + overhead;
   };
} // namespace config

   // ------------------------------------------------------------------
   // Snapshot DTO structs — flatten SSO keys into vectors for
   // portable serialization via FC_REFLECT.  These are transient
   // (never stored in chainbase), so plain vectors are used.
   // ------------------------------------------------------------------
   struct snapshot_kv_object {
      account_name        code;
      account_name        payer;
      uint8_t             key_format = 0;
      std::vector<char>   key;          ///< flattened from SSO inline/heap
      std::vector<char>   value;
   };

   struct snapshot_kv_index_object {
      account_name        code;
      account_name        payer;
      name                table;
      uint8_t             index_id = 0;
      std::vector<char>   sec_key;      ///< flattened from SSO inline/heap
      std::vector<char>   pri_key;      ///< flattened from SSO inline/heap
   };

} } // namespace sysio::chain

CHAINBASE_SET_INDEX_TYPE(sysio::chain::kv_object, sysio::chain::kv_index)
CHAINBASE_SET_INDEX_TYPE(sysio::chain::kv_index_object, sysio::chain::kv_index_index)

FC_REFLECT(sysio::chain::snapshot_kv_object, (code)(payer)(key_format)(key)(value))
FC_REFLECT(sysio::chain::snapshot_kv_index_object, (code)(payer)(table)(index_id)(sec_key)(pri_key))
