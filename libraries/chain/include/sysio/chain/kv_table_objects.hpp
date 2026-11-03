#pragma once

#include <sysio/chain/database_utils.hpp>
#include <sysio/chain/multi_index_includes.hpp>
#include <sysio/chain/config.hpp>
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace sysio { namespace chain {

   /**
    * @brief Key bytes held inside the row when they fit, and in a shared_blob when they do not.
    *
    * A tree search compares a key at every level. A key held in the node costs no load beyond the node, where a key
    * behind a pointer costs a second cache line per level. A long key keeps the reference counting of shared_blob, so a
    * row copied to the undo stack shares its key.
    *
    * The row keeps the size, in the padding after table_id, so every byte of the slot can hold key. The form of a slot
    * follows from that size, so a slot cannot copy, move or destroy itself. The row does that through these members.
    */
   class kv_key_blob {
   public:
      using size_type = uint16_t;

      static constexpr std::size_t inline_capacity = 24; ///< all of the slot, since the size lives in the row

      kv_key_blob() noexcept {}
      kv_key_blob(const kv_key_blob&)            = delete;
      kv_key_blob& operator=(const kv_key_blob&) = delete;

      /// Fills this empty slot with a copy of `other`, which holds `size` bytes.
      void copy_from(const kv_key_blob& other, size_type size) {
         if (is_inline(size)) copy_inline(other, size);
         else new (heap_storage()) shared_blob(other.heap());
      }

      /// Fills this empty slot from `other`, which holds `size` bytes and ends up empty.
      void move_from(kv_key_blob& other, size_type size) noexcept {
         if (is_inline(size)) {
            copy_inline(other, size);
         } else {
            new (heap_storage()) shared_blob(std::move(other.heap()));
            other.heap().~shared_blob();
         }
      }

      /// Releases a slot that holds `size` bytes.
      void destroy(size_type size) noexcept {
         if (!is_inline(size)) heap().~shared_blob();
      }

      /// Replaces the `size` bytes held with `new_size` bytes at `ptr`, and updates `size`. `ptr` must not point into
      /// this key. `new_size` must fit size_type: every caller checks it against a limit that the static_assert below
      /// ties to that type.
      void assign(size_type& size, const char* ptr, std::size_t new_size) {
         assert(new_size <= std::numeric_limits<size_type>::max());
         if (new_size <= inline_capacity) {
            destroy(size);
            if (new_size > 0) std::memcpy(_storage, ptr, new_size);
         } else if (is_inline(size)) {
            new (heap_storage()) shared_blob(std::string_view(ptr, new_size));
         } else {
            heap().assign(ptr, new_size);
         }
         size = static_cast<size_type>(new_size);
      }

      /// The bytes of a slot that holds `size` bytes.
      const char* data(size_type size) const noexcept {
         return is_inline(size) ? reinterpret_cast<const char*>(_storage) : heap_data();
      }

   private:
      static bool is_inline(size_type size) noexcept { return size <= inline_capacity; }
      void* heap_storage() noexcept { return _storage; }
      shared_blob& heap() noexcept { return *std::launder(reinterpret_cast<shared_blob*>(_storage)); }
      const shared_blob& heap() const noexcept { return const_cast<kv_key_blob*>(this)->heap(); }

      /// Out of line for the reason on kv_key_less::compare_other_sizes.
      [[gnu::noinline]] const char* heap_data() const noexcept { return heap().data(); }

      void copy_inline(const kv_key_blob& other, size_type size) noexcept {
         if (size > 0) std::memcpy(_storage, other._storage, size);
      }

      /// The key bytes, or a shared_blob. The row knows which.
      alignas(shared_blob) unsigned char _storage[inline_capacity];

      static_assert(sizeof(shared_blob) <= inline_capacity, "a long key's blob must fit in _storage");
   };

   static_assert(sizeof(kv_key_blob) == kv_key_blob::inline_capacity, "kv_key_blob has padding");
   static_assert(config::max_kv_key_size_limit <= std::numeric_limits<kv_key_blob::size_type>::max(),
                 "kv_key_blob size too narrow");

   /**
    * @brief Primary KV storage object.
    *
    * Stores arbitrary variable-length keys and values in chainbase shared memory. key_size and value_size are billed
    * separately at billing time on top of config::kv_row_overhead_ram_bytes, so this layout does not move RAM prices.
    *
    * table_id is a uint16 namespace identifier. CDT computes it as the low 16
    * bits of a DJB2 hash over the 8 big-endian bytes of the table's name::raw.
    * The chain does not enforce uniqueness; CDT codegen rejects collisions
    * within one contract ABI. The namespace partitions the composite index with
    * zero per-row key overhead.
    */
   class kv_object : public chainbase::object<kv_object_type, kv_object> {
      kv_object() = default;

   public:
      /// OBJECT_CTOR's constructor, but delegating: a row that throws while it is built still runs its destructor,
      /// which is the only thing that releases a long key.
      template <typename Constructor>
      kv_object(Constructor&& c, chainbase::constructor_tag)
         : kv_object() {
         c(*this);
      }

      kv_object(const kv_object& other)
         : code(other.code)
         , table_id(other.table_id)
         , _key_size(other._key_size)
         , value_size(other.value_size)
         , id(other.id)
         , payer(other.payer)
         , value(other.value) {
         _key.copy_from(other._key, _key_size);
      }

      kv_object(kv_object&& other) noexcept
         : code(other.code)
         , table_id(other.table_id)
         , _key_size(std::exchange(other._key_size, 0))
         , value_size(other.value_size)
         , id(other.id)
         , payer(other.payer)
         , value(std::move(other.value)) {
         _key.move_from(other._key, _key_size);
      }

      kv_object& operator=(const kv_object&) = delete; ///< chainbase assigns objects only by move

      kv_object& operator=(kv_object&& other) noexcept {
         if (this != &other) {
            code       = other.code;
            table_id   = other.table_id;
            value_size = other.value_size;
            id         = other.id;
            payer      = other.payer;
            value      = std::move(other.value);
            _key.destroy(_key_size);
            _key_size = std::exchange(other._key_size, 0);
            _key.move_from(other._key, _key_size);
         }
         return *this;
      }

      ~kv_object() { _key.destroy(_key_size); }

      // A search reads code, table_id and key at every tree level, so they come first, next to the index's tree links.
      // The key size and value_size fill the padding after table_id. value_size has no part in a search.
      account_name   code;          ///< contract account owning this row
      uint16_t       table_id = 0;  ///< table namespace (see the class comment)
   private:
      kv_key_blob::size_type _key_size = 0; ///< size of `_key`
   public:
      uint32_t       value_size = 0; ///< size of the value, kept here so that a row's shape costs no load of the blob
   private:
      kv_key_blob    _key;          ///< primary key bytes (opaque, layout determined by CDT)
   public:
      id_type        id;
      account_name   payer;         ///< RAM payer, named on insert. See validate_account_ram_deltas for who can be one

      std::string_view key_view() const {
         return {_key.data(_key_size), _key_size};
      }

      /// Stores a key. `size` must fit kv_key_blob::size_type.
      void set_key(const char* ptr, std::size_t size) { _key.assign(_key_size, ptr, size); }

      std::string_view value_view() const {
         return {value.data(), value_size};
      }

      /// Stores a value. It is the only way to, so that `value_size` cannot drift from it.
      void set_value(std::string_view bytes) {
         value.assign(bytes.data(), bytes.size());
         value_size = static_cast<uint32_t>(bytes.size());
      }

   private:
      shared_blob    value;         ///< arbitrary byte value
   };

   /// Key extractor returning string_view from the row's key.
   struct kv_key_extractor {
      using result_type = std::string_view;
      result_type operator()(const kv_object& o) const {
         return o.key_view();
      }
   };

   /// Fast key comparator with integer fast-paths for 8-byte keys and for the 16-byte keys of multi_index and scoped
   /// tables. Over those sizes, lexicographic byte comparison == big-endian uint64_t comparison.
   /// Load + convert-to-host + integer cmp is ~1 instruction vs memcmp's byte loop.
   struct kv_key_less {
      using is_transparent = void;

      bool operator()(std::string_view a, std::string_view b) const {
         if (__builtin_expect(a.size() == b.size(), 1)) {
            if (a.size() == word_size)
               return kv_decode_be64(a.data()) < kv_decode_be64(b.data());
            if (a.size() == two_words_size) {
               const uint64_t a_high = kv_decode_be64(a.data());
               const uint64_t b_high = kv_decode_be64(b.data());
               if (a_high != b_high) return a_high < b_high;
               return kv_decode_be64(a.data() + word_size) < kv_decode_be64(b.data() + word_size);
            }
         }
         return compare_other_sizes(a, b);
      }

   private:
      static constexpr std::size_t word_size      = sizeof(uint64_t);
      static constexpr std::size_t two_words_size = 2 * word_size;
      static constexpr std::size_t half_word_size = sizeof(uint32_t);
      static constexpr std::size_t bits_per_byte  = 8;
      static constexpr std::size_t word_bits      = word_size * bits_per_byte;

      /// Compares whole words, then whatever is left. Out of line so that the common sizes stay small enough to inline
      /// into the tree search. A call at every tree level costs about 1 ns, which is most of a search through rows
      /// already in the cache.
      [[gnu::noinline]] static bool compare_other_sizes(std::string_view a, std::string_view b) {
         const std::size_t common = std::min(a.size(), b.size());
         std::size_t       done   = 0;
         for (; common - done >= word_size; done += word_size) {
            const uint64_t a_word = kv_decode_be64(a.data() + done);
            const uint64_t b_word = kv_decode_be64(b.data() + done);
            if (a_word != b_word) return a_word < b_word;
         }
         const std::size_t rest   = common - done;
         const uint64_t    a_tail = decode_tail(a.data() + done, rest);
         const uint64_t    b_tail = decode_tail(b.data() + done, rest);
         if (a_tail != b_tail) return a_tail < b_tail;
         // Both tails pad from the same length, so equal words mean equal bytes and the shorter key is the prefix.
         return a.size() < b.size();
      }

      /// Decodes 4 bytes big-endian from `p`.
      static uint64_t decode_half_word(const char* p) noexcept {
         uint32_t v;
         std::memcpy(&v, p, sizeof(v));
         if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap32(v);
         return v;
      }

      /// Byte `i` of `p`, moved to where it sits in a big-endian word.
      static uint64_t byte_in_word(const char* p, std::size_t i) noexcept {
         return uint64_t{static_cast<unsigned char>(p[i])} << (word_bits - bits_per_byte * (i + 1));
      }

      /**
       * The `size` bytes at `p`, fewer than a word, as a big-endian word padded with zeros. Overlapping loads cover any
       * size, so this reads nothing past `size` and calls nothing. Two tails of one size pad alike, so their words
       * order as their bytes do.
       */
      static uint64_t decode_tail(const char* p, std::size_t size) noexcept {
         if (size >= half_word_size) {
            const uint64_t first = decode_half_word(p) << (word_bits - half_word_size * bits_per_byte);
            const uint64_t last  = decode_half_word(p + size - half_word_size) << (word_bits - size * bits_per_byte);
            return first | last;
         }
         if (size == 0) return 0;
         return byte_in_word(p, 0) | byte_in_word(p, size / 2) | byte_in_word(p, size - 1);
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
    * sec_key_size and pri_key_size are billed separately at billing time on top of
    * config::kv_index_entry_overhead_ram_bytes, so this layout does not move RAM prices.
    *
    * table_id identifies this secondary index's namespace, separate from the
    * primary table's: the low 16 bits of DJB2 run over the table's name::raw
    * bytes, then continued over the index's name::raw bytes (index position + 1
    * for positional multi_index indices).
    */
   class kv_index_object : public chainbase::object<kv_index_object_type, kv_index_object> {
      kv_index_object() = default;

   public:
      /// Delegates for the reason on kv_object's constructor.
      template <typename Constructor>
      kv_index_object(Constructor&& c, chainbase::constructor_tag)
         : kv_index_object() {
         c(*this);
      }

      kv_index_object(const kv_index_object& other)
         : code(other.code)
         , table_id(other.table_id)
         , _sec_size(other._sec_size)
         , _pri_size(other._pri_size)
         , id(other.id)
         , payer(other.payer) {
         _sec_key.copy_from(other._sec_key, _sec_size);
         _pri_key.copy_from(other._pri_key, _pri_size);
      }

      kv_index_object(kv_index_object&& other) noexcept
         : code(other.code)
         , table_id(other.table_id)
         , _sec_size(std::exchange(other._sec_size, 0))
         , _pri_size(std::exchange(other._pri_size, 0))
         , id(other.id)
         , payer(other.payer) {
         _sec_key.move_from(other._sec_key, _sec_size);
         _pri_key.move_from(other._pri_key, _pri_size);
      }

      kv_index_object& operator=(const kv_index_object&) = delete; ///< chainbase assigns objects only by move

      kv_index_object& operator=(kv_index_object&& other) noexcept {
         if (this != &other) {
            code     = other.code;
            table_id = other.table_id;
            id       = other.id;
            payer    = other.payer;
            _sec_key.destroy(_sec_size);
            _pri_key.destroy(_pri_size);
            _sec_size = std::exchange(other._sec_size, 0);
            _pri_size = std::exchange(other._pri_size, 0);
            _sec_key.move_from(other._sec_key, _sec_size);
            _pri_key.move_from(other._pri_key, _pri_size);
         }
         return *this;
      }

      ~kv_index_object() {
         _sec_key.destroy(_sec_size);
         _pri_key.destroy(_pri_size);
      }

      // Search order, for the reason given on kv_object. Both key sizes fit in the padding after table_id, with 2 bytes
      // of it left over.
      account_name   code;
      uint16_t       table_id = 0;  ///< secondary index namespace (see the class comment)
   private:
      kv_key_blob::size_type _sec_size = 0; ///< size of `_sec_key`
      kv_key_blob::size_type _pri_size = 0; ///< size of `_pri_key`
      kv_key_blob    _sec_key;      ///< secondary key bytes
      kv_key_blob    _pri_key;      ///< primary key bytes
   public:
      id_type        id;
      account_name   payer;         ///< RAM payer (mirrors kv_object::payer)

      std::string_view sec_key_view() const { return {_sec_key.data(_sec_size), _sec_size}; }
      std::string_view pri_key_view() const { return {_pri_key.data(_pri_size), _pri_size}; }

      /// Stores a key. `size` must fit kv_key_blob::size_type.
      void set_sec_key(const char* ptr, std::size_t size) { _sec_key.assign(_sec_size, ptr, size); }
      void set_pri_key(const char* ptr, std::size_t size) { _pri_key.assign(_pri_size, ptr, size); }
   };

   /// Everything in a row but its key slot: code, the 8 bytes from table_id to value_size, id, payer and the value
   /// blob.
   inline constexpr std::size_t kv_object_fixed_size = 5 * sizeof(uint64_t);
   /// The same for an index entry, which has no value blob.
   inline constexpr std::size_t kv_index_object_fixed_size = 4 * sizeof(uint64_t);

   // The sizes sit in padding, so a node is what its key slots make it, and moving them grew neither object.
   static_assert(sizeof(kv_object) == kv_object_fixed_size + kv_key_blob::inline_capacity, "kv_object grew");
   static_assert(sizeof(kv_index_object) == kv_index_object_fixed_size + 2 * kv_key_blob::inline_capacity,
                 "kv_index_object grew");

   /// Key extractors for kv_index_object's key fields.
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
   // KV billing comes from protocol constants in config.hpp, not from sizeof or the index count of these objects, so
   // that a layout change or a new storage engine cannot move RAM prices. Variable bytes are added at billing time.
   template<>
   struct billable_size<kv_object> {
      static constexpr uint64_t value = kv_row_overhead_ram_bytes;
   };
   static_assert(billable_size_v<kv_object> == kv_row_overhead_ram_bytes, "billable_alignment moved the KV row price");

   template<>
   struct billable_size<kv_index_object> {
      static constexpr uint64_t value = kv_index_entry_overhead_ram_bytes;
   };
   static_assert(billable_size_v<kv_index_object> == kv_index_entry_overhead_ram_bytes,
                 "billable_alignment moved the KV index entry price");
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
