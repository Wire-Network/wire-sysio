#pragma once

#include <sysio/chain/types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chainbase { class database; }

namespace sysio::chain {

/**
 * One stored KV row, as an engine hands it out. The views point into the engine and stay valid until the next write to
 * it, so copy what must outlive one.
 */
struct kv_row {
   account_name     code;
   uint16_t         table_id = 0;
   account_name     payer;
   std::string_view key;
   std::string_view value;
   const void*      engine_ref = nullptr; ///< lets the engine that returned this row reach it again without a search
};

/// One stored secondary index entry. The views follow the rule on kv_row.
struct kv_index_entry {
   account_name     code;
   uint16_t         table_id = 0;
   account_name     payer;
   std::string_view sec_key;
   std::string_view pri_key;
   const void*      engine_ref = nullptr; ///< lets the engine that returned this entry reach it again without a search
};

/**
 * A place in an engine's sort order. Rows sort by (code, table_id, key) and entries by (code, table_id, sec_key,
 * pri_key), where keys compare as unsigned bytes and a key sorts before the keys it prefixes. For rows `first` is the
 * key and `second` stays empty. For entries, an empty `second` sits before every entry with that secondary key.
 */
struct kv_position {
   account_name     code;
   uint16_t         table_id = 0;
   std::string_view first;
   std::string_view second;
};

/**
 * A movable place in the ordered rows of one engine, across all codes and tables. A cursor is at a row or at the single
 * end position past the last row, where a new cursor starts. Every move returns true and fills `row` if it lands on a
 * row.
 *
 * - `next` from the last row and `prev` from the first row land on end. `next` at end stays there, and `prev` at end
 *   lands on the last row.
 * - A cursor sees the store as it stands when it moves, including writes made since its last move.
 * - A write that erases the row under a cursor, or changes its sort position, leaves that cursor unusable until its
 *   next seek. Any other write leaves it on its row.
 * - Each thread needs its own cursor. Cursors may read at the same time as each other but not during a write.
 *
 * An engine owns everything else about a cursor, so one over several sources, such as an in-memory overlay on top of
 * data on disk, keeps its merge state here and needs no support from callers.
 */
template<typename Row>
class kv_cursor {
public:
   using row_type = Row;

   virtual ~kv_cursor() = default;

   virtual bool seek_lower(const kv_position& at, Row& row) = 0;                    ///< first row at or after `at`
   virtual bool seek_upper(const kv_position& at, Row& row) = 0;                    ///< first row after `at`
   virtual bool seek_past_table(account_name code, uint16_t table_id, Row& row) = 0; ///< first row past that table
   virtual bool next(Row& row) = 0;
   virtual bool prev(Row& row) = 0;
   virtual bool read(Row& row) const = 0;                                           ///< the row this cursor is on

   /// A second cursor at the same place.
   virtual std::unique_ptr<kv_cursor> clone() const = 0;
};

using kv_row_cursor   = kv_cursor<kv_row>;
using kv_index_cursor = kv_cursor<kv_index_entry>;

/**
 * What one undo session did to one key, in the key order of the rows above. `row` is the row as it now stands, or as it
 * stood before an erase. A key whose row ends the session as it began has no delta, and neither does a row that was
 * both created and erased in it.
 */
template<typename Row>
struct kv_delta {
   using row_type = Row;

   Row  row;
   bool present = true; ///< false when the session erased the row
};

/**
 * @brief The storage engine behind KV rows and secondary index entries.
 *
 * Everything outside the engine reaches KV state through this class, so that an engine can change without touching
 * contract-visible behavior. docs/kv-store-seam.md gives the contract and the reasons for its shape.
 *
 * Undo sessions stay with chainbase::database, because KV writes must roll back together with the rest of the chain
 * state. An engine joins those sessions through join_undo_sessions.
 *
 * Billing is not here. Per-row payer billing needs the old row before every write, so a caller reads with find_row and
 * then calls insert_row or update_row, which is also why there is no separate upsert.
 */
class kv_store {
public:
   virtual ~kv_store() = default;

   /// Makes `db`'s undo sessions drive this engine. Call once, after any snapshot has been loaded.
   virtual void join_undo_sessions(chainbase::database& db) = 0;

   // --- Rows ---

   virtual std::optional<kv_row> find_row(account_name code, uint16_t table_id, std::string_view key) const = 0;
   /// Stores a new row. Returns nothing and changes nothing if the key is taken.
   virtual std::optional<kv_row> insert_row(account_name code, uint16_t table_id, account_name payer,
                                            std::string_view key, std::string_view value) = 0;
   /// Replaces the payer and value of a row from find_row or a cursor.
   virtual kv_row update_row(const kv_row& row, account_name payer, std::string_view value) = 0;
   virtual void   erase_row(const kv_row& row) = 0;
   virtual std::unique_ptr<kv_row_cursor> open_row_cursor() const = 0;
   virtual uint64_t row_count() const = 0;
   virtual void     reserve_rows(size_t count) = 0; ///< a hint before a bulk load
   /// The rows the top undo session changed, in key order.
   virtual std::vector<kv_delta<kv_row>> row_deltas() const = 0;

   // --- Secondary index entries ---

   virtual std::optional<kv_index_entry> find_index_entry(account_name code, uint16_t table_id,
                                                          std::string_view sec_key, std::string_view pri_key) const = 0;
   /// Stores a new entry. Returns false and changes nothing if the (sec_key, pri_key) pair is taken.
   virtual bool insert_index_entry(account_name code, uint16_t table_id, account_name payer,
                                   std::string_view sec_key, std::string_view pri_key) = 0;
   /// Replaces the payer and secondary key of an entry. The new position must be free.
   virtual void update_index_entry(const kv_index_entry& entry, account_name payer, std::string_view sec_key) = 0;
   virtual void erase_index_entry(const kv_index_entry& entry) = 0;
   virtual std::unique_ptr<kv_index_cursor> open_index_cursor() const = 0;
   virtual uint64_t index_entry_count() const = 0;
   virtual void     reserve_index_entries(size_t count) = 0; ///< a hint before a bulk load
   /// The entries the top undo session changed, in key order.
   virtual std::vector<kv_delta<kv_index_entry>> index_entry_deltas() const = 0;
};

/// The first key past every key that starts with `prefix`, or nothing if `prefix` is all 0xff bytes.
std::optional<std::string> kv_prefix_successor(std::string_view prefix);

/// The production engine, over the kv_index and kv_index_index tables of `db`.
std::shared_ptr<kv_store> make_chainbase_kv_store(chainbase::database& db);

} // namespace sysio::chain
