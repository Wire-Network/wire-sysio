#include <sysio/chain/kv_store.hpp>
#include <sysio/chain/kv_table_objects.hpp>

#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace sysio::chain {
namespace {

/// What differs between rows and index entries in the chainbase engine.
struct row_family {
   using object_type = kv_object;
   using index_type  = kv_index;
   using order_type  = by_code_key;
   using row_type    = kv_row;

   static kv_row view(const kv_object& o) {
      return {o.code, o.table_id, o.payer, o.key_view(), o.value_view(), &o};
   }
   static auto search_key(const kv_position& at) { return boost::make_tuple(at.code, at.table_id, at.first); }
   static auto sort_position(const kv_object& o) { return std::tuple(o.code, o.table_id, o.key_view()); }
   static bool same_content(const kv_object& a, const kv_object& b) {
      return a.payer == b.payer && a.value_view() == b.value_view();
   }
};

struct index_family {
   using object_type = kv_index_object;
   using index_type  = kv_index_index;
   using order_type  = by_code_table_id_seckey;
   using row_type    = kv_index_entry;

   static kv_index_entry view(const kv_index_object& o) {
      return {o.code, o.table_id, o.payer, o.sec_key_view(), o.pri_key_view(), &o};
   }
   static auto search_key(const kv_position& at) {
      return boost::make_tuple(at.code, at.table_id, at.first, at.second);
   }
   static auto sort_position(const kv_index_object& o) {
      return std::tuple(o.code, o.table_id, o.sec_key_view(), o.pri_key_view());
   }
   static bool same_content(const kv_index_object& a, const kv_index_object& b) { return a.payer == b.payer; }
};

/// The index that gives a family its sort order, which every cursor and lookup walks.
template<typename Family>
const auto& ordered_index(const chainbase::database& db) {
   return db.get_index<typename Family::index_type, typename Family::order_type>();
}

/// A kv_cursor is one iterator of the family's ordered index.
template<typename Family>
class chainbase_cursor final : public kv_cursor<typename Family::row_type> {
public:
   using row_type = typename Family::row_type;

   explicit chainbase_cursor(const chainbase::database& db)
      : _index(ordered_index<Family>(db))
      , _position(_index.end()) {}

   bool seek_lower(const kv_position& at, row_type& row) override {
      return land(_index.lower_bound(Family::search_key(at)), row);
   }
   bool seek_upper(const kv_position& at, row_type& row) override {
      return land(_index.upper_bound(Family::search_key(at)), row);
   }
   bool seek_past_table(account_name code, uint16_t table_id, row_type& row) override {
      return land(_index.upper_bound(boost::make_tuple(code, table_id)), row);
   }
   bool next(row_type& row) override {
      if (_position == _index.end()) return false;
      return land(std::next(_position), row);
   }
   bool prev(row_type& row) override {
      return land(_position == _index.begin() ? _index.end() : std::prev(_position), row);
   }
   bool read(row_type& row) const override {
      if (_position == _index.end()) return false;
      row = Family::view(*_position);
      return true;
   }
   std::unique_ptr<kv_cursor<row_type>> clone() const override { return std::make_unique<chainbase_cursor>(*this); }

private:
   using index_t    = std::remove_cvref_t<decltype(ordered_index<Family>(std::declval<const chainbase::database&>()))>;
   using iterator_t = decltype(std::declval<const index_t&>().begin());

   bool land(iterator_t position, row_type& row) {
      _position = position;
      return read(row);
   }

   const index_t& _index;
   iterator_t     _position;
};

/// Builds the delta list of the top undo session. Chainbase records it by object, so a row erased and stored again
/// under one key appears twice and an index entry that moved appears once, under its new key. A delta is per key.
template<typename Family>
std::vector<kv_delta<typename Family::row_type>> session_deltas(const chainbase::database& db) {
   using object_type = typename Family::object_type;
   struct change {
      const object_type* before = nullptr;
      const object_type* after  = nullptr;

      const object_type& any() const { return before ? *before : *after; }
   };

   std::vector<change> changes;
   const auto& index = db.get_index<typename Family::index_type>();
   const auto undo = index.last_undo_session();
   for (const auto& old : undo.old_values) {
      const auto& current = index.get(old.id);
      if (Family::sort_position(old) == Family::sort_position(current)) {
         changes.push_back({&old, &current});
      } else {
         changes.push_back({&old, nullptr});
         changes.push_back({nullptr, &current});
      }
   }
   for (const auto& old : undo.removed_values)
      changes.push_back({&old, nullptr});
   for (const auto& row : undo.new_values)
      changes.push_back({nullptr, &row});

   std::sort(changes.begin(), changes.end(), [](const change& a, const change& b) {
      return Family::sort_position(a.any()) < Family::sort_position(b.any());
   });

   std::vector<kv_delta<typename Family::row_type>> deltas;
   deltas.reserve(changes.size());
   for (auto itr = changes.begin(); itr != changes.end();) {
      change merged = *itr++;
      if (itr != changes.end() && Family::sort_position(merged.any()) == Family::sort_position(itr->any())) {
         merged.before = merged.before ? merged.before : itr->before;
         merged.after  = merged.after ? merged.after : itr->after;
         ++itr;
      }
      if (merged.before && merged.after && Family::same_content(*merged.before, *merged.after))
         continue;
      deltas.push_back({Family::view(merged.after ? *merged.after : *merged.before), merged.after != nullptr});
   }
   return deltas;
}

/// A new cursor at the end position of a family's index.
template<typename Family>
std::unique_ptr<chainbase_cursor<Family>> open_cursor(const chainbase::database& db) {
   return std::make_unique<chainbase_cursor<Family>>(db);
}

class chainbase_kv_store final : public kv_store {
public:
   explicit chainbase_kv_store(chainbase::database& db)
      : _db(db) {}

   /// kv_index and kv_index_index are tables of `db`, so its sessions already cover them.
   void join_undo_sessions(chainbase::database&) override {}

   std::optional<kv_row> find_row(account_name code, uint16_t table_id, std::string_view key) const override {
      const auto& index = ordered_index<row_family>(_db);
      const auto itr = index.find(boost::make_tuple(code, table_id, key));
      if (itr == index.end()) return std::nullopt;
      return row_family::view(*itr);
   }

   std::optional<kv_row> insert_row(account_name code, uint16_t table_id, account_name payer, std::string_view key,
                                    std::string_view value) override {
      // Create searches once, and the descent that places the row is the one that finds a duplicate. It leaves the
      // database as it was when the uniqueness constraint fails.
      try {
         return row_family::view(_db.create<kv_object>([&](auto& o) {
            o.code     = code;
            o.payer    = payer;
            o.table_id = table_id;
            o.set_key(key.data(), key.size());
            o.set_value(value);
         }));
      } catch (const chainbase::uniqueness_violation&) {
         return std::nullopt;
      }
   }

   kv_row update_row(const kv_row& row, account_name payer, std::string_view value) override {
      const auto& object = *static_cast<const kv_object*>(row.engine_ref);
      _db.modify_unindexed(object, [&](auto& o) {
         o.payer = payer;
         o.set_value(value);
      });
      return row_family::view(object);
   }

   void erase_row(const kv_row& row) override { _db.remove(*static_cast<const kv_object*>(row.engine_ref)); }

   std::unique_ptr<kv_row_cursor> open_row_cursor() const override { return open_cursor<row_family>(_db); }
   uint64_t row_count() const override { return _db.get_index<kv_index>().indices().size(); }
   void reserve_rows(size_t count) override { _db.preallocate<kv_object>(count); }
   std::vector<kv_delta<kv_row>> row_deltas() const override { return session_deltas<row_family>(_db); }

   std::optional<kv_index_entry> find_index_entry(account_name code, uint16_t table_id, std::string_view sec_key,
                                                  std::string_view pri_key) const override {
      const auto& index = ordered_index<index_family>(_db);
      const auto itr = index.find(boost::make_tuple(code, table_id, sec_key, pri_key));
      if (itr == index.end()) return std::nullopt;
      return index_family::view(*itr);
   }

   bool insert_index_entry(account_name code, uint16_t table_id, account_name payer, std::string_view sec_key,
                           std::string_view pri_key) override {
      // Same search count and failure behavior as insert_row.
      try {
         _db.create<kv_index_object>([&](auto& o) {
            o.code     = code;
            o.payer    = payer;
            o.table_id = table_id;
            o.set_sec_key(sec_key.data(), sec_key.size());
            o.set_pri_key(pri_key.data(), pri_key.size());
         });
         return true;
      } catch (const chainbase::uniqueness_violation&) {
         return false;
      }
   }

   void update_index_entry(const kv_index_entry& entry, account_name payer, std::string_view sec_key) override {
      const auto& object = *static_cast<const kv_index_object*>(entry.engine_ref);
      if (sec_key == entry.sec_key) {
         _db.modify_unindexed(object, [&](auto& o) { o.payer = payer; });
         return;
      }
      // Modify moves the object to its new sort position without reallocating it. The caller has ruled out a collision.
      _db.modify(object, [&](auto& o) {
         o.payer = payer;
         o.set_sec_key(sec_key.data(), sec_key.size());
      });
   }

   void erase_index_entry(const kv_index_entry& entry) override {
      _db.remove(*static_cast<const kv_index_object*>(entry.engine_ref));
   }

   std::unique_ptr<kv_index_cursor> open_index_cursor() const override { return open_cursor<index_family>(_db); }
   uint64_t index_entry_count() const override { return _db.get_index<kv_index_index>().indices().size(); }
   void reserve_index_entries(size_t count) override { _db.preallocate<kv_index_object>(count); }
   std::vector<kv_delta<kv_index_entry>> index_entry_deltas() const override {
      return session_deltas<index_family>(_db);
   }

private:
   chainbase::database& _db;
};

} // namespace

std::optional<std::string> kv_prefix_successor(std::string_view prefix) {
   constexpr unsigned char last_byte = 0xff;
   while (!prefix.empty() && static_cast<unsigned char>(prefix.back()) == last_byte)
      prefix.remove_suffix(1);
   if (prefix.empty())
      return std::nullopt;
   std::string successor(prefix);
   ++successor.back();
   return successor;
}

std::shared_ptr<kv_store> make_chainbase_kv_store(chainbase::database& db) {
   return std::make_shared<chainbase_kv_store>(db);
}

} // namespace sysio::chain
