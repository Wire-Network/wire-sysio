#pragma once

#include <sysio/chain/kv_context.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/test/unit_test.hpp>
#include <fc/crypto/hex.hpp>
#include <magic_enum/magic_enum.hpp>

#include <ostream>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

/**
 * A contract that makes the one KV call its action data names, for tests that pin what observers see. Written in WAST
 * so that every byte a call produces is chosen by the test, and depends on no CDT output.
 *
 * Action data is a kv_write_header followed by the bytes it sizes. A call that does not return the status the header
 * expects aborts the action, so a test cannot pass by logging nothing. A read also compares what came back with the
 * bytes the test named, so a later action in a transaction can pin what an earlier one wrote.
 */
namespace kv_write_driver {

using namespace sysio::chain;
using namespace sysio::testing;

/// Which intrinsic an action calls. Those from `get` on are reads, and read through a `code` carried in the payer field.
enum class kv_call : uint32_t { set, erase, idx_store, idx_update, idx_erase, get, it_read, idx_find };

/// The fixed part of the action data. The WAST below reads these offsets.
struct kv_write_header {
   uint32_t call;
   uint32_t table_id;
   uint64_t payer;
   uint32_t mode;
   int32_t  expected_status;
   uint32_t first_size;
   uint32_t second_size;
   uint32_t third_size;
   uint32_t reserved = 0; ///< fills the struct: padding would put stray bytes in the action data, and so in block ids
};
static_assert(sizeof(kv_write_header) == 40);
static_assert(std::has_unique_object_representations_v<kv_write_header>);

inline constexpr char kv_write_wast[] = R"=====(
(module
  (import "env" "read_action_data" (func $read_action_data (param i32 i32) (result i32)))
  (import "env" "sysio_assert" (func $sysio_assert (param i32 i32)))
  (import "env" "kv_set" (func $kv_set (param i32 i64 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_erase" (func $kv_erase (param i32 i32 i32) (result i32)))
  (import "env" "kv_idx_store" (func $kv_idx_store (param i32 i64 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_idx_update" (func $kv_idx_update (param i32 i64 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_idx_erase" (func $kv_idx_erase (param i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_get" (func $kv_get (param i32 i64 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_it_create" (func $kv_it_create (param i32 i64 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_it_read" (func $kv_it_read (param i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_it_destroy" (func $kv_it_destroy (param i32)))
  (import "env" "kv_idx_find" (func $kv_idx_find (param i32 i64 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_idx_read" (func $kv_idx_read (param i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "kv_idx_destroy" (func $kv_idx_destroy (param i32)))
  (import "env" "memcmp" (func $memcmp (param i32 i32 i32) (result i32)))
  (memory 2)
  (export "memory" (memory 0))
  (data (i32.const 65000) "unexpected kv status\00")
  ;; True when `size` bytes came back at `got` and they are the `want_size` bytes at `want`.
  (func $same (param $got i32) (param $size i32) (param $want i32) (param $want_size i32) (result i32)
    (if (result i32) (i32.ne (get_local $size) (get_local $want_size))
      (then (i32.const 0))
      (else (i32.eqz (call $memcmp (get_local $got) (get_local $want) (get_local $want_size))))))
  (func (export "apply") (param i64 i64 i64)
    (local $call i32) (local $table i32) (local $payer i64) (local $mode i32) (local $expected i32) (local $status i32)
    (local $first i32) (local $first_size i32)
    (local $second i32) (local $second_size i32)
    (local $third i32) (local $third_size i32)
    (local $handle i32)
    (drop (call $read_action_data (i32.const 0) (i32.const 65000)))
    (set_local $call (i32.load (i32.const 0)))
    (set_local $table (i32.load (i32.const 4)))
    (set_local $payer (i64.load (i32.const 8)))
    (set_local $mode (i32.load (i32.const 16)))
    (set_local $expected (i32.load (i32.const 20)))
    (set_local $first (i32.const 40))
    (set_local $first_size (i32.load (i32.const 24)))
    (set_local $second (i32.add (get_local $first) (get_local $first_size)))
    (set_local $second_size (i32.load (i32.const 28)))
    (set_local $third (i32.add (get_local $second) (get_local $second_size)))
    (set_local $third_size (i32.load (i32.const 32)))
    (if (i32.eq (get_local $call) (i32.const 0))
      (then (set_local $status (call $kv_set (get_local $table) (get_local $payer)
                                 (get_local $first) (get_local $first_size)
                                 (get_local $second) (get_local $second_size) (get_local $mode)))))
    (if (i32.eq (get_local $call) (i32.const 1))
      (then (set_local $status (call $kv_erase (get_local $table) (get_local $first) (get_local $first_size)))))
    (if (i32.eq (get_local $call) (i32.const 2))
      (then (set_local $status (call $kv_idx_store (get_local $table) (get_local $payer)
                                 (get_local $first) (get_local $first_size)
                                 (get_local $second) (get_local $second_size)))))
    (if (i32.eq (get_local $call) (i32.const 3))
      (then (set_local $status (call $kv_idx_update (get_local $table) (get_local $payer)
                                 (get_local $first) (get_local $first_size)
                                 (get_local $second) (get_local $second_size)
                                 (get_local $third) (get_local $third_size)))))
    (if (i32.eq (get_local $call) (i32.const 4))
      (then (set_local $status (call $kv_idx_erase (get_local $table)
                                 (get_local $first) (get_local $first_size)
                                 (get_local $second) (get_local $second_size)))))
    ;; A read fills the two output buffers on the second page. A wrong answer is a status no call returns.
    (if (i32.eq (get_local $call) (i32.const 5))
      (then
        (set_local $handle (call $kv_get (get_local $table) (get_local $payer) (get_local $first) (get_local $first_size)
                             (i32.const 0) (i32.const 65536) (i32.const 32768)))
        (set_local $status (i32.const 2))
        (if (i32.ge_s (get_local $handle) (i32.const 0))
          (then (set_local $status
            (if (result i32) (call $same (i32.const 65536) (get_local $handle) (get_local $second) (get_local $second_size))
              (then (i32.const 0)) (else (i32.const 99))))))))
    (if (i32.eq (get_local $call) (i32.const 6))
      (then
        (set_local $handle (call $kv_it_create (get_local $table) (get_local $payer) (i32.const 0) (i32.const 0)
                             (get_local $first) (get_local $first_size)))
        (set_local $status (call $kv_it_read (get_local $handle) (i32.const 0) (i32.const 65536) (i32.const 32768)
                             (i32.const 98304) (i32.const 32768) (i32.const 65024) (i32.const 65028)))
        (if (i32.eqz (get_local $status))
          (then (set_local $status
            (if (result i32) (i32.and
                  (call $same (i32.const 65536) (i32.load (i32.const 65024)) (get_local $first) (get_local $first_size))
                  (call $same (i32.const 98304) (i32.load (i32.const 65028)) (get_local $second) (get_local $second_size)))
              (then (i32.const 0)) (else (i32.const 99))))))
        (call $kv_it_destroy (get_local $handle))))
    (if (i32.eq (get_local $call) (i32.const 7))
      (then
        (set_local $handle (call $kv_idx_find (get_local $table) (get_local $payer) (i32.const 0) (i32.const 0)
                             (get_local $second) (get_local $second_size)))
        (set_local $status (i32.const 2))
        (if (i32.ge_s (get_local $handle) (i32.const 0))
          (then
            (set_local $status (call $kv_idx_read (get_local $handle) (i32.const 65536) (i32.const 32768)
                                 (i32.const 98304) (i32.const 32768) (i32.const 65024) (i32.const 65028)))
            (if (i32.eqz (get_local $status))
              (then (set_local $status
                (if (result i32) (i32.and
                      (call $same (i32.const 65536) (i32.load (i32.const 65024)) (get_local $second) (get_local $second_size))
                      (call $same (i32.const 98304) (i32.load (i32.const 65028)) (get_local $first) (get_local $first_size)))
                  (then (i32.const 0)) (else (i32.const 99))))))
            (call $kv_idx_destroy (get_local $handle))))))
    (call $sysio_assert (i32.eq (get_local $status) (get_local $expected)) (i32.const 65000))
  )
)
)=====";

/// One KV call. `first` to `third` are the call's key and value arguments, in the order the intrinsic takes them. For a
/// read they are the key and what the read must find there.
struct kv_write {
   kv_call      call;
   uint16_t     table_id = 0;
   account_name payer;
   std::string  first;
   std::string  second;
   std::string  third;
   kv_set_mode  mode     = kv_set_mode::upsert;
   kv_status    expected = kv_status::ok;
};

/// `write`, expected to come back as `status` rather than ok.
inline kv_write expecting(kv_status status, kv_write write) {
   write.expected = status;
   return write;
}

inline kv_write set_row(uint16_t table_id, account_name payer, std::string key, std::string value,
                        kv_set_mode mode = kv_set_mode::upsert) {
   return {kv_call::set, table_id, payer, std::move(key), std::move(value), {}, mode};
}

inline kv_write erase_row(uint16_t table_id, std::string key) {
   return {kv_call::erase, table_id, {}, std::move(key)};
}

inline kv_write store_entry(uint16_t table_id, account_name payer, std::string pri_key, std::string sec_key) {
   return {kv_call::idx_store, table_id, payer, std::move(pri_key), std::move(sec_key)};
}

inline kv_write update_entry(uint16_t table_id, account_name payer, std::string pri_key, std::string old_sec_key,
                             std::string new_sec_key) {
   return {kv_call::idx_update, table_id, payer, std::move(pri_key), std::move(old_sec_key), std::move(new_sec_key)};
}

inline kv_write erase_entry(uint16_t table_id, std::string pri_key, std::string sec_key) {
   return {kv_call::idx_erase, table_id, {}, std::move(pri_key), std::move(sec_key)};
}

/// The value at `key` of `code`'s table must be `value`, and kv_get finds it.
inline kv_write read_row(uint16_t table_id, account_name code, std::string key, std::string value) {
   return {kv_call::get, table_id, code, std::move(key), std::move(value)};
}

/// A seek to `key` must land on that row, with a primary iterator that reads `value` from it. Expect kv_status::end from
/// a seek past the last row.
inline kv_write scan_row(uint16_t table_id, account_name code, std::string key, std::string value) {
   return {kv_call::it_read, table_id, code, std::move(key), std::move(value)};
}

/// kv_idx_find on `sec_key` must land on the entry holding `pri_key`.
inline kv_write find_entry(uint16_t table_id, account_name code, std::string pri_key, std::string sec_key) {
   return {kv_call::idx_find, table_id, code, std::move(pri_key), std::move(sec_key)};
}

/// The action data for `write`.
inline bytes pack_kv_write(const kv_write& write) {
   const kv_write_header header{
      .call            = magic_enum::enum_integer(write.call),
      .table_id        = write.table_id,
      .payer           = write.payer.to_uint64_t(),
      .mode            = magic_enum::enum_integer(write.mode),
      .expected_status = magic_enum::enum_integer(write.expected),
      .first_size      = static_cast<uint32_t>(write.first.size()),
      .second_size     = static_cast<uint32_t>(write.second.size()),
      .third_size      = static_cast<uint32_t>(write.third.size()),
   };
   bytes data(reinterpret_cast<const char*>(&header), reinterpret_cast<const char*>(&header + 1));
   data.insert(data.end(), write.first.begin(), write.first.end());
   data.insert(data.end(), write.second.begin(), write.second.end());
   data.insert(data.end(), write.third.begin(), write.third.end());
   return data;
}

/// Whether a payer named in a call authorizes the action that makes it.
enum class payers { authorize, do_not_authorize };

/// A signed transaction with one action per write, in order. By default a payer other than `driver` authorizes its
/// action. A read names a `code` in that field, which authorizes nothing.
template<typename Tester>
signed_transaction kv_writes_trx(Tester& chain, account_name driver, const std::vector<kv_write>& writes,
                                 payers authorizing = payers::authorize) {
   constexpr auto action_name_for_writes = "write"_n;

   signed_transaction trx;
   std::set<account_name> signers{driver};
   for (const auto& write : writes) {
      std::vector<permission_level> auths;
      const bool pays = authorizing == payers::authorize && write.call < kv_call::get;
      if (pays && write.payer != account_name{} && write.payer != driver) {
         // The authorization manager wants the payer's sysio.payer permission first.
         auths.push_back({write.payer, config::sysio_payer_name});
         auths.push_back({write.payer, config::active_name});
         signers.insert(write.payer);
      }
      auths.push_back({driver, config::active_name});
      trx.actions.emplace_back(std::move(auths), driver, action_name_for_writes, pack_kv_write(write));
   }
   chain.set_transaction_headers(trx);
   for (const auto& signer : signers) {
      trx.sign(chain.get_private_key(signer, "active"), chain.get_chain_id());
   }
   return trx;
}

/// Pushes kv_writes_trx().
template<typename Tester>
transaction_trace_ptr push_kv_writes(Tester& chain, account_name driver, const std::vector<kv_write>& writes,
                                     payers authorizing = payers::authorize) {
   auto trx = kv_writes_trx(chain, driver, writes, authorizing);
   return chain.push_transaction(trx);
}

/// Every field KV holds for some accounts, in index order, and the RAM each of them pays for. Comparing two of these is
/// how a test shows that nothing changed, payer and table_id included.
struct kv_state {
   struct row {
      account_name code;
      uint16_t     table_id = 0;
      account_name payer;
      std::string  key;
      std::string  value;

      bool operator==(const row&) const = default;
      friend std::ostream& operator<<(std::ostream& os, const row& r) {
         return os << r.code << ' ' << r.table_id << ' ' << r.payer << ' ' << fc::to_hex(r.key.data(), r.key.size()) << ' '
                   << fc::to_hex(r.value.data(), r.value.size());
      }
   };

   struct entry {
      account_name code;
      uint16_t     table_id = 0;
      account_name payer;
      std::string  sec_key;
      std::string  pri_key;

      bool operator==(const entry&) const = default;
      friend std::ostream& operator<<(std::ostream& os, const entry& e) {
         return os << e.code << ' ' << e.table_id << ' ' << e.payer << ' ' << fc::to_hex(e.sec_key.data(), e.sec_key.size())
                   << ' ' << fc::to_hex(e.pri_key.data(), e.pri_key.size());
      }
   };

   struct ram {
      account_name account;
      int64_t      usage = 0;

      bool operator==(const ram&) const = default;
      friend std::ostream& operator<<(std::ostream& os, const ram& r) { return os << r.account << ' ' << r.usage; }
   };

   std::vector<row>   rows;
   std::vector<entry> entries;
   std::vector<ram>   usage;

   bool operator==(const kv_state&) const = default;
};

/// What KV holds for `accounts` on `chain` now.
template<typename Tester>
kv_state kv_state_of(const Tester& chain, const std::vector<account_name>& accounts) {
   const auto held = [&](account_name code) { return std::ranges::find(accounts, code) != accounts.end(); };
   const auto& kv  = chain.control->kv();

   kv_state state;
   kv_row row;
   const auto rows = kv.open_row_cursor();
   for (bool more = rows->seek_lower({}, row); more; more = rows->next(row)) {
      if (held(row.code)) {
         state.rows.push_back({row.code, row.table_id, row.payer, std::string(row.key), std::string(row.value)});
      }
   }
   kv_index_entry entry;
   const auto entries = kv.open_index_cursor();
   for (bool more = entries->seek_lower({}, entry); more; more = entries->next(entry)) {
      if (held(entry.code)) {
         state.entries.push_back(
            {entry.code, entry.table_id, entry.payer, std::string(entry.sec_key), std::string(entry.pri_key)});
      }
   }
   for (const auto& account : accounts) {
      state.usage.push_back({account, chain.control->get_resource_limits_manager().get_account_ram_usage(account)});
   }
   return state;
}

/// Reports each field that differs, rather than one failure for the whole state.
inline void check_same_kv_state(const kv_state& actual, const kv_state& expected) {
   BOOST_TEST(actual.rows == expected.rows, boost::test_tools::per_element());
   BOOST_TEST(actual.entries == expected.entries, boost::test_tools::per_element());
   BOOST_TEST(actual.usage == expected.usage, boost::test_tools::per_element());
}

} // namespace kv_write_driver
