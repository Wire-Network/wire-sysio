#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/chain/kv_context.hpp>
#include <sysio/chain/resource_limits.hpp>

#include <fc/crypto/hex.hpp>

#include "kv_write_driver.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

BOOST_AUTO_TEST_SUITE(kv_tests)

// Snapshots and integrity hashes walk rows in kv_key_less order, so every path through it has to be unsigned byte order
// with a prefix first. Sizes sit around each word boundary, and the fills include both sides of the sign bit.
BOOST_AUTO_TEST_CASE(kv_key_less_is_byte_order) {
   constexpr std::array<size_t, 15> sizes = {0, 1, 7, 8, 9, 15, 16, 17, 23, 24, 25, 31, 32, 33, 40};
   constexpr std::array<char, 4>    fills = {'\x00', '\x7f', '\x80', '\xff'};
   constexpr char                   sign_bit = '\x80';

   std::vector<std::string> keys;
   for (const size_t size : sizes) {
      for (const char fill : fills) {
         keys.emplace_back(size, fill);
         for (size_t at = 0; at < size; ++at) {
            std::string differs(size, fill);
            differs[at] ^= sign_bit;
            keys.push_back(std::move(differs));
         }
      }
   }

   const kv_key_less less;
   size_t wrong = 0;
   for (const auto& a : keys) {
      for (const auto& b : keys)
         wrong += less(a, b) != (std::string_view(a) < std::string_view(b));
   }
   BOOST_TEST(wrong == 0u);
}

namespace {

/// Checks kv_key_less against plain memcmp order and fails the test at the first pair that differs. Each key sits in a
/// heap block of exactly its size, so a sanitizer build also catches a read past the end of a key.
class key_order_check {
public:
   using key = std::vector<char>;

   /// Ordered pairs the two walks check, so that neither can shrink without the test noticing.
   static constexpr size_t expected_pairs = 3'840'462;

   key_order_check() {
      for (char& byte : noise) byte = static_cast<char>(rng());
   }

   /// All short tails after every prefix size, so that each tail size is reached at each word offset. A key against
   /// itself with a zero added is where a padded tail leaves only the sizes to tell the keys apart.
   void every_short_tail() {
      for (const size_t prefix_size : prefix_sizes) {
         for (const unsigned char fill : prefix_fills) {
            std::vector<key> keys;
            collect_tails(key(prefix_size, static_cast<char>(fill)), longest_short_tail, keys);
            for (size_t i = 0; i < keys.size(); ++i) {
               for (size_t j = i; j < keys.size(); ++j) {
                  check(keys[i], keys[j]);
                  check(keys[j], keys[i]);
               }
            }
         }
      }
   }

   /// Each pair of sizes, sharing each possible number of leading bytes, then differing on a hard pair of bytes.
   void every_size_pair() {
      std::vector<size_t> sizes(small_size_end);
      std::iota(sizes.begin(), sizes.end(), size_t{0});
      sizes.insert(sizes.end(), large_sizes.begin(), large_sizes.end());
      for (const size_t a_size : sizes) {
         for (const size_t b_size : sizes) {
            const size_t common = std::min(a_size, b_size);
            for (size_t shared = 0; shared <= common; ++shared) {
               for (const auto& [a_byte, b_byte] : byte_pairs) {
                  key a = random_key(a_size);
                  key b = random_key(b_size);
                  if (shared > 0) std::memcpy(b.data(), a.data(), shared);
                  if (shared < common) {
                     a[shared] = static_cast<char>(a_byte);
                     b[shared] = static_cast<char>(b_byte);
                  }
                  check(a, b);
               }
            }
         }
      }
   }

   size_t pairs_checked() const { return pairs; }

private:
   static constexpr uint64_t seed = 0x6b220881cacf4d73;
   /// Bytes where a signed compare, a wrong byte order or a padding mistake would show.
   static constexpr std::array<unsigned char, 6> hard_bytes = {0x00, 0x01, 0x7f, 0x80, 0xfe, 0xff};
   static constexpr std::array<unsigned char, 3> prefix_fills = {0x00, 0x5a, 0xff};
   /// Around every word boundary the comparator walks over.
   static constexpr std::array<size_t, 17> prefix_sizes = {0,  1,  7,  8,  9,  15, 16, 17, 23,
                                                           24, 25, 31, 32, 33, 39, 40, 41};
   static constexpr std::array<std::pair<unsigned char, unsigned char>, 6> byte_pairs = {
      {{0x00, 0x01}, {0x7f, 0x80}, {0x00, 0xff}, {0x80, 0x00}, {0xff, 0xfe}, {0x01, 0x00}}};
   static constexpr std::array<size_t, 8> large_sizes = {63, 64, 65, 127, 128, 255, 256, 1024};
   static constexpr size_t longest_short_tail = 3;
   static constexpr size_t small_size_end     = 49; ///< every size below this goes against every other
   static constexpr size_t noise_size         = 4096;

   /// The order every comparator on the consensus path has to match: unsigned bytes, then the shorter key.
   static bool memcmp_less(const key& a, const key& b) {
      const size_t common = std::min(a.size(), b.size());
      const int    order  = common == 0 ? 0 : std::memcmp(a.data(), b.data(), common);
      return order != 0 ? order < 0 : a.size() < b.size();
   }

   /// Appends `prefix`, then every string of up to `max_size` hard bytes after it, to `out`.
   static void collect_tails(const key& prefix, size_t max_size, std::vector<key>& out) {
      out.push_back(prefix);
      if (max_size == 0) return;
      for (const unsigned char byte : hard_bytes) {
         key longer(prefix.size() + 1);
         std::copy(prefix.begin(), prefix.end(), longer.begin());
         longer.back() = static_cast<char>(byte);
         collect_tails(longer, max_size - 1, out);
      }
   }

   void check(const key& a, const key& b) {
      ++pairs;
      const bool less = kv_key_less{}(std::string_view{a.data(), a.size()}, std::string_view{b.data(), b.size()});
      if (less != memcmp_less(a, b))
         BOOST_FAIL("kv_key_less says " << less << " for " << fc::to_hex(a) << " against " << fc::to_hex(b));
   }

   key random_key(size_t size) {
      const size_t start = rng() % (noise.size() - size + 1);
      return key(noise.begin() + start, noise.begin() + start + size);
   }

   std::mt19937_64 rng{seed};
   key             noise = key(noise_size);
   size_t          pairs = 0;
};

} // namespace

// The tail of a key compares as a padded word, so when the words match only the sizes can tell a key from a longer one.
// This walks far more pairs than kv_key_less_is_byte_order, and still takes only seconds.
BOOST_AUTO_TEST_CASE(kv_key_less_matches_memcmp_order) {
   key_order_check order;
   order.every_short_tail();
   order.every_size_pair();
   BOOST_TEST(order.pairs_checked() == key_order_check::expected_pairs);
}

namespace {

/// A chain with the KV write driver on one account and a second account to pay for things.
struct kv_driver_tester : tester {
   static constexpr uint16_t rows = 1, entries = 2;
   const account_name writer = "kvwriter"_n;
   const account_name payer  = "kvpayer"_n;

   kv_driver_tester() {
      create_accounts({writer, payer});
      set_code(writer, kv_write_driver::kv_write_wast);
      produce_block();
   }

   kv_write_driver::kv_state state() const { return kv_write_driver::kv_state_of(*this, {writer, payer}); }

   /// Pushes `writes` in one transaction, with a last write that aborts it after the others have applied.
   void abort_after(std::vector<kv_write_driver::kv_write> writes) {
      using namespace kv_write_driver;
      writes.push_back(expecting(kv_status::exists, set_row(rows, writer, "abort", "x")));
      BOOST_CHECK_EXCEPTION(push_kv_writes(*this, writer, writes), sysio_assert_message_exception,
                            sysio_assert_message_is("unexpected kv status"));
   }
};

} // namespace

// A failed transaction leaves no trace of what it created, updated or erased, in rows, index entries or RAM.
BOOST_FIXTURE_TEST_CASE(kv_writes_undo_with_the_transaction, kv_driver_tester) {
   using namespace kv_write_driver;
   push_kv_writes(*this, writer, {set_row(rows, writer, "keep", "one"), set_row(rows, writer, "gone", "two"),
                                  store_entry(entries, writer, "pk1", "keep"),
                                  store_entry(entries, writer, "pk2", "gone")});
   produce_block();
   const auto before = state();
   BOOST_REQUIRE_EQUAL(before.rows.size(), 2u);
   BOOST_REQUIRE_EQUAL(before.entries.size(), 2u);

   abort_after({set_row(rows, writer, "keep", "changed value", kv_set_mode::update), erase_row(rows, "gone"),
                set_row(rows, writer, "new", "row", kv_set_mode::insert),
                update_entry(entries, writer, "pk1", "keep", "moved"), erase_entry(entries, "pk2", "gone"),
                store_entry(entries, writer, "pk3", "new")});

   check_same_kv_state(state(), before);
}

// A same-size update is the one write that could land on the bytes in place, since nothing about the row's allocation
// changes. Only the undo session's copy keeps the old bytes, so this is the case where a rollback depends on it.
BOOST_FIXTURE_TEST_CASE(kv_same_size_writes_undo_with_the_transaction, kv_driver_tester) {
   using namespace kv_write_driver;
   push_kv_writes(*this, writer, {set_row(rows, writer, "row", "value-a"), store_entry(entries, writer, "pk", "sec-a")});
   produce_block();
   const auto before = state();

   abort_after({set_row(rows, writer, "row", "value-b", kv_set_mode::update),
                update_entry(entries, writer, "pk", "sec-a", "sec-b")});

   check_same_kv_state(state(), before);
}

// A payer change moves a whole charge between two accounts, and an abort has to move it back.
BOOST_FIXTURE_TEST_CASE(kv_payer_changes_undo_with_the_transaction, kv_driver_tester) {
   using namespace kv_write_driver;
   push_kv_writes(*this, writer, {set_row(rows, writer, "row", "value"), store_entry(entries, writer, "pk", "sec")});
   produce_block();
   const auto before = state();

   abort_after({set_row(rows, payer, "row", "value", kv_set_mode::update),
                update_entry(entries, payer, "pk", "sec", "sec")});

   check_same_kv_state(state(), before);
   BOOST_CHECK_EQUAL(before.rows[0].payer, writer);
   BOOST_CHECK_EQUAL(before.entries[0].payer, writer);
}

// Several writes to one key in one transaction. The undo session holds the row as it first found it, or nothing for a row
// the transaction created, and the rollback has to come out right whichever way each key went.
BOOST_FIXTURE_TEST_CASE(kv_stacked_writes_undo_with_the_transaction, kv_driver_tester) {
   using namespace kv_write_driver;
   push_kv_writes(*this, writer, {set_row(rows, writer, "again", "original"),
                                  set_row(rows, payer, "paid", "by the payer"),
                                  store_entry(entries, writer, "pk", "aa")});
   produce_block();
   const auto before = state();

   abort_after({// Created, then updated.
                set_row(rows, writer, "new", "one", kv_set_mode::insert),
                set_row(rows, writer, "new", "two-two", kv_set_mode::update),
                // Created, then re-keyed.
                store_entry(entries, writer, "pk2", "cc"), update_entry(entries, writer, "pk2", "cc", "dd"),
                // Erased, then created again at the key, by someone else.
                erase_row(rows, "again"), set_row(rows, payer, "again", "replaced", kv_set_mode::insert),
                // Updated by one payer, then another.
                set_row(rows, writer, "paid", "by the writer", kv_set_mode::update),
                set_row(rows, payer, "paid", "by the payer again", kv_set_mode::update),
                // Re-keyed, then a new entry at the pair it left.
                update_entry(entries, writer, "pk", "aa", "bb"), store_entry(entries, writer, "pk", "aa")});

   check_same_kv_state(state(), before);

   // The indexes came through sound: every key answers as it did, and a committed write goes where it should.
   push_kv_writes(*this, writer, {read_row(rows, writer, "again", "original"),
                                  expecting(kv_status::no_row, read_row(rows, writer, "new", "")),
                                  find_entry(entries, writer, "pk", "aa"),
                                  expecting(kv_status::no_row, find_entry(entries, writer, "pk", "bb")),
                                  expecting(kv_status::no_row, find_entry(entries, writer, "pk2", "dd")),
                                  expecting(kv_status::exists, store_entry(entries, writer, "pk", "aa")),
                                  update_entry(entries, writer, "pk", "aa", "bb"),
                                  erase_row(rows, "again")});
   produce_block();
   const auto after = state();
   BOOST_REQUIRE_EQUAL(after.rows.size(), 1u);
   BOOST_CHECK_EQUAL(after.rows[0].key, "paid");
   BOOST_CHECK_EQUAL(after.rows[0].payer, payer);
   BOOST_REQUIRE_EQUAL(after.entries.size(), 1u);
   BOOST_CHECK_EQUAL(after.entries[0].sec_key, "bb");
}

// Aborting the block takes back every transaction in it, which the integrity hash confirms for the whole database.
BOOST_FIXTURE_TEST_CASE(kv_writes_undo_with_the_block, kv_driver_tester) {
   using namespace kv_write_driver;
   push_kv_writes(*this, writer, {set_row(rows, writer, "keep", "one"), set_row(rows, payer, "gone", "two"),
                                  store_entry(entries, writer, "pk1", "keep"),
                                  store_entry(entries, payer, "pk2", "gone")});
   produce_block();
   control->abort_block();
   const auto before      = state();
   const auto before_hash = control->calculate_integrity_hash();

   push_kv_writes(*this, writer, {set_row(rows, writer, "keep", "uno", kv_set_mode::update),
                                  set_row(rows, writer, "keep", "changed size", kv_set_mode::update),
                                  set_row(rows, writer, "new", "row", kv_set_mode::insert),
                                  store_entry(entries, writer, "pk3", "new")});
   push_kv_writes(*this, writer, {erase_row(rows, "gone"), erase_entry(entries, "pk2", "gone"),
                                  update_entry(entries, payer, "pk1", "keep", "held")});
   BOOST_REQUIRE(!(state() == before));

   control->abort_block();

   check_same_kv_state(state(), before);
   BOOST_CHECK_EQUAL(control->calculate_integrity_hash().str(), before_hash.str());

   // The chain carries on from the state it went back to.
   push_kv_writes(*this, writer, {read_row(rows, writer, "keep", "one"), find_entry(entries, writer, "pk1", "keep"),
                                  set_row(rows, writer, "new", "row", kv_set_mode::insert)});
   produce_block();
}

// Each action of a transaction reads what the actions before it wrote, by every kind of read.
BOOST_FIXTURE_TEST_CASE(kv_reads_see_the_writes_of_earlier_actions, kv_driver_tester) {
   using namespace kv_write_driver;
   push_kv_writes(*this, writer, {
      set_row(rows, writer, "key", "one"),
      store_entry(entries, writer, "pk", "sec"),
      read_row(rows, writer, "key", "one"),
      scan_row(rows, writer, "key", "one"),
      find_entry(entries, writer, "pk", "sec"),

      set_row(rows, writer, "key", "two-two", kv_set_mode::update),
      update_entry(entries, writer, "pk", "sec", "moved"),
      read_row(rows, writer, "key", "two-two"),
      scan_row(rows, writer, "key", "two-two"),
      find_entry(entries, writer, "pk", "moved"),
      expecting(kv_status::no_row, find_entry(entries, writer, "pk", "sec")),

      erase_row(rows, "key"),
      erase_entry(entries, "pk", "moved"),
      expecting(kv_status::no_row, read_row(rows, writer, "key", "")),
      expecting(kv_status::end, scan_row(rows, writer, "key", "")),
      expecting(kv_status::no_row, find_entry(entries, writer, "pk", "moved")),
   });
   produce_block();
   BOOST_CHECK(state().rows.empty());
   BOOST_CHECK(state().entries.empty());
}

// The reads above can fail: a wrong byte, a wrong size and a wrong key each abort, so a pass means the bytes matched.
BOOST_FIXTURE_TEST_CASE(kv_driver_reads_compare_what_they_find, kv_driver_tester) {
   using namespace kv_write_driver;
   push_kv_writes(*this, writer, {set_row(rows, writer, "key", "value"), store_entry(entries, writer, "pk", "sec")});
   produce_block();

   const auto refused = [&](const kv_write& read) {
      BOOST_CHECK_EXCEPTION(push_kv_writes(*this, writer, {read}), sysio_assert_message_exception,
                            sysio_assert_message_is("unexpected kv status"));
   };
   refused(read_row(rows, writer, "key", "valuf"));
   refused(read_row(rows, writer, "key", "valu"));
   refused(scan_row(rows, writer, "kex", "value"));
   refused(scan_row(rows, writer, "key", "valuf"));
   refused(find_entry(entries, writer, "pk2", "sec"));
   refused(expecting(kv_status::no_row, read_row(rows, writer, "key", "value")));

   // Another account's tables, through the code the read names.
   push_kv_writes(*this, writer, {expecting(kv_status::no_row, read_row(rows, payer, "key", "value")),
                                  expecting(kv_status::no_row, find_entry(entries, payer, "pk", "sec"))});
}

BOOST_AUTO_TEST_CASE(kv_iterator_pool_basic) {
   kv_primary_iterator_pool prim_pool;
   kv_secondary_iterator_pool sec_pool;

   // Treat the value returned by primary allocate() as a contract-facing handle and route every access via the
   // handle helpers so the test does not bake in the current "primary handle == slot index" detail.
   uint32_t h1 = prim_pool.allocate(uint16_t(0), "test"_n, "prefix");
   BOOST_CHECK(!kv_handle_is_secondary(h1));
   auto& slot1 = prim_pool.get(kv_handle_slot_index(h1));
   BOOST_CHECK_EQUAL(slot1.code, "test"_n);
   BOOST_CHECK_EQUAL(slot1.prefix.size(), 6u);

   // Secondary: wrap the slot index with the tag bit, then verify round-trip through the helpers.
   uint32_t s2 = sec_pool.allocate(uint16_t(100), "test"_n, {});
   uint32_t h2 = kv_make_secondary_handle(s2);
   BOOST_CHECK(kv_handle_is_secondary(h2));
   BOOST_CHECK_EQUAL(kv_handle_slot_index(h2), s2);
   auto& slot2 = sec_pool.get(kv_handle_slot_index(h2));
   BOOST_CHECK_EQUAL(slot2.code, "test"_n);

   // Each pool has its own free-list. A primary release does not affect the secondary, and vice versa.
   prim_pool.release(kv_handle_slot_index(h1));
   uint32_t h3 = prim_pool.allocate(uint16_t(0), "other"_n, {});
   BOOST_CHECK_EQUAL(kv_handle_slot_index(h3), kv_handle_slot_index(h1)); // reuses the freed slot

   sec_pool.release(kv_handle_slot_index(h2));
   prim_pool.release(kv_handle_slot_index(h3));
}

BOOST_AUTO_TEST_CASE(kv_iterator_pool_release_all) {
   kv_primary_iterator_pool prim_pool;
   kv_secondary_iterator_pool sec_pool;

   // An unused pool has nothing to release and stays usable.
   prim_pool.release_all();
   sec_pool.release_all();

   BOOST_CHECK_EQUAL(prim_pool.allocate(uint16_t(0), "test"_n, "prefix"), 0u);
   const uint32_t held = prim_pool.allocate(uint16_t(0), "test"_n, {});
   BOOST_CHECK_EQUAL(held, 1u);
   BOOST_CHECK_EQUAL(sec_pool.allocate(uint16_t(100), "test"_n, {}), 0u);

   prim_pool.release_all();
   sec_pool.release_all();

   // Every handle is dead, and allocation restarts at slot 0 with a clean slot.
   BOOST_CHECK_THROW(prim_pool.get(held), kv_invalid_iterator);
   BOOST_CHECK_THROW(sec_pool.get(0), kv_invalid_iterator);
   BOOST_CHECK_EQUAL(prim_pool.allocate(uint16_t(0), "other"_n, {}), 0u);
   BOOST_CHECK_EQUAL(prim_pool.get(0).code, "other"_n);
   BOOST_CHECK(prim_pool.get(0).prefix.empty());
   BOOST_CHECK_EQUAL(sec_pool.allocate(uint16_t(100), "test"_n, {}), 0u);
}

BOOST_AUTO_TEST_CASE(kv_validate_handle_dispatch) {
   // validate_primary_handle: well-formed primary returns its slot index; wrong-pool tag and reserved bits throw.
   BOOST_CHECK_EQUAL(validate_primary_handle(0u, "op"), 0u);
   BOOST_CHECK_EQUAL(validate_primary_handle(1023u, "op"), 1023u);
   BOOST_CHECK_THROW(validate_primary_handle(kv_make_secondary_handle(5u), "op"), kv_invalid_iterator);
   BOOST_CHECK_THROW(validate_primary_handle(0x00000400u, "op"), kv_invalid_iterator); // bit 10 reserved
   BOOST_CHECK_THROW(validate_primary_handle(0x00020000u, "op"), kv_invalid_iterator); // bit 17 reserved
   BOOST_CHECK_THROW(validate_primary_handle(0x80000000u, "op"), kv_invalid_iterator); // bit 31 reserved (sign)

   // validate_secondary_handle: well-formed secondary returns its slot index; primary handles and reserved bits throw.
   uint32_t sec_handle = kv_make_secondary_handle(5u);
   BOOST_CHECK_EQUAL(validate_secondary_handle(sec_handle, "op"), 5u);
   BOOST_CHECK_THROW(validate_secondary_handle(5u, "op"), kv_invalid_iterator);        // primary
   BOOST_CHECK_THROW(validate_secondary_handle(0u, "op"), kv_invalid_iterator);        // primary, slot 0
   BOOST_CHECK_THROW(validate_secondary_handle(sec_handle | 0x00000400u, "op"),
                     kv_invalid_iterator); // sec + bit 10
   BOOST_CHECK_THROW(validate_secondary_handle(sec_handle | 0x80000000u, "op"),
                     kv_invalid_iterator); // sec + bit 31
}

BOOST_AUTO_TEST_CASE(kv_check_prefix_size_bounds) {
   BOOST_CHECK_NO_THROW(kv_check_prefix_size(0));
   BOOST_CHECK_NO_THROW(kv_check_prefix_size(8));
   BOOST_CHECK_NO_THROW(kv_check_prefix_size(config::max_kv_key_size_limit));
   BOOST_CHECK_THROW(kv_check_prefix_size(config::max_kv_key_size_limit + 1), kv_key_too_large);
   BOOST_CHECK_THROW(kv_check_prefix_size(std::numeric_limits<uint32_t>::max()), kv_key_too_large);
}

BOOST_AUTO_TEST_CASE(kv_iterator_pool_independent_exhaustion) {
   kv_primary_iterator_pool prim_pool;
   kv_secondary_iterator_pool sec_pool;

   // Exhausting one pool must not consume slots in the other.
   for (uint32_t i = 0; i < config::max_kv_iterators; ++i) {
      prim_pool.allocate(uint16_t(0), "test"_n, {});
   }
   BOOST_CHECK_THROW(
      prim_pool.allocate(uint16_t(0), "test"_n, {}),
      kv_iterator_limit_exceeded
   );

   // Secondary pool is still empty -- can allocate the full budget.
   for (uint32_t i = 0; i < config::max_kv_iterators; ++i) {
      sec_pool.allocate(uint16_t(0), "test"_n, {});
   }
   BOOST_CHECK_THROW(
      sec_pool.allocate(uint16_t(0), "test"_n, {}),
      kv_iterator_limit_exceeded
   );

   // Release one slot in each pool and verify reuse.
   prim_pool.release(5);
   BOOST_CHECK_EQUAL(prim_pool.allocate(uint16_t(0), "test"_n, {}), 5u);
   sec_pool.release(7);
   BOOST_CHECK_EQUAL(sec_pool.allocate(uint16_t(0), "test"_n, {}), 7u);
}

BOOST_AUTO_TEST_CASE(kv_iterator_handle_encoding) {
   // Round-trip: tag a slot index, recover it via the helpers.
   for (uint32_t slot : {uint32_t{0}, uint32_t{1}, uint32_t{17}, uint32_t{1023}}) {
      uint32_t handle = kv_make_secondary_handle(slot);
      BOOST_CHECK(kv_handle_is_secondary(handle));
      BOOST_CHECK_EQUAL(kv_handle_slot_index(handle), slot);
      // Reserved bits 10..15 and 17..31 must all be zero in a freshly-encoded handle.
      BOOST_CHECK_EQUAL(handle & ~(kv_handle_slot_index_mask | kv_secondary_handle_tag),
                        0u);
   }

   // Primary handles carry no tag.
   BOOST_CHECK(!kv_handle_is_secondary(0u));
   BOOST_CHECK(!kv_handle_is_secondary(1u));
   BOOST_CHECK(!kv_handle_is_secondary(1023u));

   // Reserved-bit guard: a fabricated handle with any reserved bit set throws.
   BOOST_CHECK_NO_THROW(kv_handle_check_reserved_zero(0u));
   BOOST_CHECK_NO_THROW(kv_handle_check_reserved_zero(1023u));
   BOOST_CHECK_NO_THROW(kv_handle_check_reserved_zero(kv_make_secondary_handle(5u)));
   BOOST_CHECK_THROW(kv_handle_check_reserved_zero(0x00000400u), kv_invalid_iterator); // bit 10
   BOOST_CHECK_THROW(kv_handle_check_reserved_zero(0x00020000u), kv_invalid_iterator); // bit 17
   BOOST_CHECK_THROW(kv_handle_check_reserved_zero(0x40000000u), kv_invalid_iterator); // bit 30
}

// kv_idx_update can move an entry to another sort position. Verify that forget_row stops only the slots on that entry
// from using their cursors, and leaves stored key bytes intact for the slow re-seek path.
BOOST_AUTO_TEST_CASE(kv_iterator_pool_forget_row) {
   kv_secondary_iterator_pool pool;

   const std::string_view sec_key = "alice";
   const std::string_view pri_key("\x00\x01", 2);
   const std::string_view other_pri_key("\x00\x02", 2);
   const kv_index_entry   target{"test"_n, 7, {}, sec_key, pri_key};

   const uint32_t on_target    = pool.allocate(uint16_t(7), "test"_n, {});
   const uint32_t also_target  = pool.allocate(uint16_t(7), "test"_n, {});
   const uint32_t other_table  = pool.allocate(uint16_t(8), "test"_n, {});
   const uint32_t other_code   = pool.allocate(uint16_t(7), "alt"_n, {});
   const uint32_t other_key    = pool.allocate(uint16_t(7), "test"_n, {});

   auto seed = [&](uint32_t handle, std::string_view pri) {
      auto& s = pool.get(handle);
      s.at_end = false;
      s.on_row = true;
      s.current_sec_key.assign(sec_key.begin(), sec_key.end());
      s.current_pri_key.assign(pri.begin(), pri.end());
   };
   seed(on_target, pri_key);
   seed(also_target, pri_key);
   seed(other_table, pri_key);
   seed(other_code, pri_key);
   seed(other_key, other_pri_key);

   pool.forget_row(target);

   // Every slot on the row is cleared, with key bytes and position preserved.
   for (uint32_t handle : {on_target, also_target}) {
      const auto& matched = pool.get(handle);
      BOOST_CHECK(!matched.on_row);
      BOOST_CHECK(!matched.at_end);
      BOOST_CHECK_EQUAL(matched.current_sec_key.size(), 5u);
      BOOST_CHECK_EQUAL(matched.current_pri_key.size(), 2u);
   }
   // A slot at the same key bytes of another table, or at another key of this one, is on a different row.
   for (uint32_t handle : {other_table, other_code, other_key})
      BOOST_CHECK(pool.get(handle).on_row);

   // A slot opened above a closed one is still reached.
   pool.release(on_target);
   pool.get(other_key).current_pri_key.assign(pri_key.begin(), pri_key.end());
   pool.forget_row(target);
   BOOST_CHECK(!pool.get(other_key).on_row);
}

// The pool starts empty and grows on demand, and growth changes neither handle numbering nor what a slot holds.
BOOST_AUTO_TEST_CASE(kv_iterator_pool_grows_on_demand) {
   kv_primary_iterator_pool pool;
   BOOST_CHECK_EQUAL(pool.capacity(), 0u);

   // Short keys stay inline in the slot; a key past the inline size spills to the heap. Both must survive a move.
   const auto key_bytes = [](uint32_t i) {
      return std::string(i % 2 == 0 ? kv_inline_key_bytes : kv_inline_key_bytes + 1, char('a' + i % 26));
   };

   for (uint32_t i = 0; i < config::max_kv_iterators; ++i) {
      const auto key = key_bytes(i);
      BOOST_REQUIRE_EQUAL(pool.allocate(uint16_t(i), "test"_n, key), i);
      auto& slot = pool.get(i);
      slot.current_key.assign(key.data(), key.data() + key.size());
      if (i == 0) BOOST_CHECK_EQUAL(pool.capacity(), kv_initial_iterator_slots);
   }
   BOOST_CHECK_EQUAL(pool.capacity(), config::max_kv_iterators);
   BOOST_CHECK_THROW(pool.allocate(uint16_t(0), "test"_n, {}), kv_iterator_limit_exceeded);

   for (uint32_t i = 0; i < config::max_kv_iterators; ++i) {
      const auto  key  = key_bytes(i);
      const auto& slot = pool.get(i);
      BOOST_REQUIRE_EQUAL(slot.table_id, uint16_t(i));
      BOOST_REQUIRE_EQUAL(std::string_view(slot.prefix.data(), slot.prefix.size()), key);
      BOOST_REQUIRE_EQUAL(std::string_view(slot.current_key.data(), slot.current_key.size()), key);
   }

   // Past the grown array is an invalid handle, as for a slot that was never opened.
   kv_secondary_iterator_pool unused;
   BOOST_CHECK_THROW(unused.get(0), kv_invalid_iterator);
   BOOST_CHECK_THROW(unused.get(config::max_kv_iterators - 1), kv_invalid_iterator);
}

// Closing slots out of order keeps the lowest-free-slot rule that makes handle numbers deterministic.
BOOST_AUTO_TEST_CASE(kv_iterator_pool_lowest_free_slot) {
   kv_primary_iterator_pool pool;
   for (uint32_t i = 0; i < 4; ++i) BOOST_REQUIRE_EQUAL(pool.allocate(uint16_t(0), "test"_n, {}), i);

   // Closing the top slots, then one below them, reuses from the bottom and never skips a free slot.
   pool.release(3);
   pool.release(2);
   pool.release(0);
   BOOST_CHECK_EQUAL(pool.allocate(uint16_t(0), "test"_n, {}), 0u);
   BOOST_CHECK_EQUAL(pool.allocate(uint16_t(0), "test"_n, {}), 2u);
   BOOST_CHECK_EQUAL(pool.allocate(uint16_t(0), "test"_n, {}), 3u);
   BOOST_CHECK_EQUAL(pool.allocate(uint16_t(0), "test"_n, {}), 4u);

   // A closed slot is an invalid handle, also once it has dropped below the highest open slot.
   pool.release(1);
   BOOST_CHECK_THROW(pool.get(1), kv_invalid_iterator);
   BOOST_CHECK_THROW(pool.release(1), kv_invalid_iterator);

   // release_all restarts numbering even after out-of-order closes.
   pool.release_all();
   BOOST_CHECK_THROW(pool.get(4), kv_invalid_iterator);
   BOOST_CHECK_EQUAL(pool.allocate(uint16_t(0), "test"_n, {}), 0u);
}

// A random walk of the pool against a model that hands out the lowest free slot. Half the rounds lean toward allocating
// and never clear the pool, so they fill it and run into the iterator limit.
BOOST_AUTO_TEST_CASE(kv_iterator_pool_matches_a_model) {
   constexpr int      rounds     = 20;
   constexpr int      steps      = 20000;
   constexpr int      percent    = 100;
   constexpr int      release_to = 98;   ///< rolls from the allocate bias up to here release one slot
   constexpr int      clear_roll = 98;   ///< this roll releases everything, and higher ones probe a slot
   constexpr int      filling_bias = 90; ///< net growth of 4 slots in 5 steps reaches the limit well inside a round
   constexpr uint32_t past_limit = config::max_kv_iterators + config::max_kv_iterators / 8;
   constexpr uint64_t seed       = 42;
   const account_name code       = "code"_n;
   constexpr uint16_t table      = 1;
   constexpr std::string_view prefix = "p";

   std::mt19937_64 rng(seed);
   int             limit_hits = 0;
   for (int round = 0; round < rounds; ++round) {
      kv_primary_iterator_pool pool;
      std::set<uint32_t>       open;
      const bool               filling       = round % 2 == 0;
      const int                allocate_bias = filling ? filling_bias : int(rng() % percent);
      for (int step = 0; step < steps; ++step) {
         const int roll = rng() % percent;
         if (roll < allocate_bias || open.empty()) {
            uint32_t lowest_free = 0;
            while (open.contains(lowest_free)) ++lowest_free;
            if (lowest_free >= config::max_kv_iterators) {
               BOOST_REQUIRE_THROW(pool.allocate(table, code, prefix), kv_iterator_limit_exceeded);
               ++limit_hits;
               continue;
            }
            BOOST_REQUIRE_EQUAL(pool.allocate(table, code, prefix), lowest_free);
            open.insert(lowest_free);
         } else if (roll < release_to) {
            const auto slot = std::next(open.begin(), rng() % open.size());
            pool.release(*slot);
            open.erase(slot);
         } else if (roll == clear_roll) {
            if (filling) continue;
            pool.release_all();
            open.clear();
         } else {
            const uint32_t slot = rng() % past_limit;
            if (open.contains(slot)) BOOST_REQUIRE_NO_THROW(pool.get(slot));
            else                     BOOST_REQUIRE_THROW(pool.get(slot), kv_invalid_iterator);
         }
      }
   }
   BOOST_CHECK_GT(limit_hits, 0);
}

// A read copies from an offset, up to the buffer's size, and never past the field.
BOOST_AUTO_TEST_CASE(kv_copy_field_bounds) {
   const std::string_view field = "abcdef";
   std::array<char, 4> buf;
   const auto copied = [&](uint32_t offset, uint32_t size) {
      buf.fill('z');
      kv_copy_field(field, offset, buf.data(), size);
      return std::string(buf.data(), buf.size());
   };

   BOOST_CHECK_EQUAL(copied(0, 3), "abcz");
   BOOST_CHECK_EQUAL(copied(2, 3), "cdez");
   BOOST_CHECK_EQUAL(copied(5, 3), "fzzz");  // less left than the buffer holds
   BOOST_CHECK_EQUAL(copied(6, 3), "zzzz");  // at the end
   BOOST_CHECK_EQUAL(copied(7, 3), "zzzz");  // past the end
   BOOST_CHECK_EQUAL(copied(std::numeric_limits<uint32_t>::max(), 3), "zzzz");
   BOOST_CHECK_EQUAL(copied(0, 0), "zzzz");  // zero-size buffer

   // An empty field and a null destination with no capacity copy nothing.
   kv_copy_field({}, 0, buf.data(), 3);
   kv_copy_field(field, 0, nullptr, 0);
}

BOOST_AUTO_TEST_SUITE_END()
