#include <sysio/testing/kv_map_store.hpp>

#include <chainbase/chainbase.hpp>

#include <boost/throw_exception.hpp>

#include <deque>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace sysio::testing {
namespace {

using chain::account_name;
using chain::kv_index_entry;
using chain::kv_position;
using chain::kv_row;

/// Both kinds of record have the same shape here. Rows keep their key in `first` and leave `second` empty. Entries keep
/// (sec_key, pri_key), and their `data` stays empty.
struct stored_key {
   account_name code;
   uint16_t     table_id = 0;
   std::string  first;
   std::string  second;
};

struct stored_data {
   account_name payer;
   std::string  data;

   bool operator==(const stored_data&) const = default;
};

/// std::string_view orders by unsigned byte with a prefix first, which is the order the seam requires.
auto sort_key(const stored_key& k) {
   return std::tuple(k.code, k.table_id, std::string_view(k.first), std::string_view(k.second));
}
auto sort_key(const kv_position& p) { return std::tuple(p.code, p.table_id, p.first, p.second); }

struct key_less {
   using is_transparent = void;

   template<typename A, typename B>
   bool operator()(const A& a, const B& b) const {
      return sort_key(a) < sort_key(b);
   }
};

/// Ordered records and the undo sessions over them. The newest session is at the back.
class undo_table {
public:
   using records_t = std::map<stored_key, stored_data, key_less>;
   /// What each key held when a session first touched it, with nothing for a key that was free.
   using session_t = std::map<stored_key, std::optional<stored_data>, key_less>;

   const records_t& records() const { return _records; }
   const session_t* top_session() const { return _sessions.empty() ? nullptr : &_sessions.back(); }

   bool insert(stored_key key, stored_data data) {
      if (_records.contains(key)) return false;
      remember(key, std::nullopt);
      _records.emplace(std::move(key), std::move(data));
      return true;
   }

   void replace(records_t::iterator itr, stored_data data) {
      remember(itr->first, itr->second);
      itr->second = std::move(data);
   }

   void erase(records_t::iterator itr) {
      remember(itr->first, itr->second);
      _records.erase(itr);
   }

   records_t::iterator locate(const kv_position& at) { return _records.find(at); }

   void add_session() { _sessions.emplace_back(); }

   void undo() {
      for (auto& [key, before] : _sessions.back()) {
         if (before) _records.insert_or_assign(key, std::move(*before));
         else        _records.erase(key);
      }
      _sessions.pop_back();
   }

   void squash() {
      if (_sessions.size() >= 2) {
         auto top = std::move(_sessions.back());
         _sessions.pop_back();
         _sessions.back().merge(top); // a key the session below already holds keeps that session's older state
      } else {
         _sessions.pop_back();
      }
   }

   void drop_oldest() { _sessions.pop_front(); }
   size_t session_count() const { return _sessions.size(); }

private:
   void remember(const stored_key& key, std::optional<stored_data> before) {
      if (!_sessions.empty()) _sessions.back().try_emplace(key, std::move(before));
   }

   records_t            _records;
   std::deque<session_t> _sessions;
};

kv_row to_row(const stored_key& key, const stored_data& data) {
   return {key.code, key.table_id, data.payer, key.first, data.data};
}

kv_index_entry to_entry(const stored_key& key, const stored_data& data) {
   return {key.code, key.table_id, data.payer, key.first, key.second};
}

template<typename Row>
Row to_view(const stored_key& key, const stored_data& data) {
   if constexpr (std::is_same_v<Row, kv_row>) return to_row(key, data);
   else                                       return to_entry(key, data);
}

template<typename Row>
class map_cursor final : public chain::kv_cursor<Row> {
public:
   explicit map_cursor(const undo_table::records_t& records)
      : _records(records)
      , _position(records.end()) {}

   bool seek_lower(const kv_position& at, Row& row) override { return land(_records.lower_bound(at), row); }
   bool seek_upper(const kv_position& at, Row& row) override { return land(_records.upper_bound(at), row); }
   bool seek_past_table(account_name code, uint16_t table_id, Row& row) override {
      auto itr = _records.lower_bound(kv_position{code, table_id});
      while (itr != _records.end() && itr->first.code == code && itr->first.table_id == table_id) ++itr;
      return land(itr, row);
   }
   bool next(Row& row) override {
      if (_position == _records.end()) return false;
      return land(std::next(_position), row);
   }
   bool prev(Row& row) override {
      return land(_position == _records.begin() ? _records.end() : std::prev(_position), row);
   }
   bool read(Row& row) const override {
      if (_position == _records.end()) return false;
      row = to_view<Row>(_position->first, _position->second);
      return true;
   }
   std::unique_ptr<chain::kv_cursor<Row>> clone() const override { return std::make_unique<map_cursor>(*this); }

private:
   bool land(undo_table::records_t::const_iterator position, Row& row) {
      _position = position;
      return read(row);
   }

   const undo_table::records_t&          _records;
   undo_table::records_t::const_iterator _position;
};

template<typename Row>
std::vector<chain::kv_delta<Row>> session_deltas(const undo_table& table) {
   std::vector<chain::kv_delta<Row>> deltas;
   const auto* session = table.top_session();
   if (!session) return deltas;
   for (const auto& [key, before] : *session) {
      const auto current = table.records().find(key);
      const bool present = current != table.records().end();
      if (!present && !before) continue;
      if (present && before && current->second == *before) continue;
      deltas.push_back({to_view<Row>(key, present ? current->second : *before), present});
   }
   return deltas;
}

class map_kv_store final : public chain::kv_store {
public:
   void join_undo_sessions(chainbase::database& db) override;

   std::optional<kv_row> find_row(account_name code, uint16_t table_id, std::string_view key) const override {
      const auto itr = _rows.records().find(kv_position{code, table_id, key});
      if (itr == _rows.records().end()) return std::nullopt;
      return to_row(itr->first, itr->second);
   }

   std::optional<kv_row> insert_row(account_name code, uint16_t table_id, account_name payer, std::string_view key,
                                    std::string_view value) override {
      if (!_rows.insert(stored_key{code, table_id, std::string(key), {}}, stored_data{payer, std::string(value)}))
         return std::nullopt;
      return find_row(code, table_id, key);
   }

   kv_row update_row(const kv_row& row, account_name payer, std::string_view value) override {
      const auto itr = _rows.locate({row.code, row.table_id, row.key});
      _rows.replace(itr, stored_data{payer, std::string(value)});
      return to_row(itr->first, itr->second);
   }

   void erase_row(const kv_row& row) override { _rows.erase(_rows.locate({row.code, row.table_id, row.key})); }

   std::unique_ptr<chain::kv_row_cursor> open_row_cursor() const override {
      return std::make_unique<map_cursor<kv_row>>(_rows.records());
   }
   uint64_t row_count() const override { return _rows.records().size(); }
   void reserve_rows(size_t) override {}
   std::vector<chain::kv_delta<kv_row>> row_deltas() const override { return session_deltas<kv_row>(_rows); }

   std::optional<kv_index_entry> find_index_entry(account_name code, uint16_t table_id, std::string_view sec_key,
                                                  std::string_view pri_key) const override {
      const auto itr = _entries.records().find(kv_position{code, table_id, sec_key, pri_key});
      if (itr == _entries.records().end()) return std::nullopt;
      return to_entry(itr->first, itr->second);
   }

   bool insert_index_entry(account_name code, uint16_t table_id, account_name payer, std::string_view sec_key,
                           std::string_view pri_key) override {
      return _entries.insert(stored_key{code, table_id, std::string(sec_key), std::string(pri_key)},
                             stored_data{payer, {}});
   }

   void update_index_entry(const kv_index_entry& entry, account_name payer, std::string_view sec_key) override {
      const auto itr = _entries.locate({entry.code, entry.table_id, entry.sec_key, entry.pri_key});
      if (sec_key == entry.sec_key) { // the entry stays where it is, so a cursor on it stays valid
         _entries.replace(itr, stored_data{payer, {}});
         return;
      }
      stored_key moved{entry.code, entry.table_id, std::string(sec_key), std::string(entry.pri_key)};
      _entries.erase(itr);
      _entries.insert(std::move(moved), stored_data{payer, {}});
   }

   void erase_index_entry(const kv_index_entry& entry) override {
      _entries.erase(_entries.locate({entry.code, entry.table_id, entry.sec_key, entry.pri_key}));
   }

   std::unique_ptr<chain::kv_index_cursor> open_index_cursor() const override {
      return std::make_unique<map_cursor<kv_index_entry>>(_entries.records());
   }
   uint64_t index_entry_count() const override { return _entries.records().size(); }
   void reserve_index_entries(size_t) override {}
   std::vector<chain::kv_delta<kv_index_entry>> index_entry_deltas() const override {
      return session_deltas<kv_index_entry>(_entries);
   }

   // --- Undo, driven by map_undo_participant ---

   void add_session() {
      _rows.add_session();
      _entries.add_session();
      ++_revision;
   }

   void undo() {
      if (_rows.session_count() == 0) return;
      _rows.undo();
      _entries.undo();
      --_revision;
   }

   void squash() {
      if (_rows.session_count() == 0) return;
      _rows.squash();
      _entries.squash();
      --_revision;
   }

   void commit(int64_t revision) {
      while (_rows.session_count() > 0 && oldest_session_revision() <= revision) {
         _rows.drop_oldest();
         _entries.drop_oldest();
      }
   }

   void undo_all() {
      while (_rows.session_count() > 0)
         undo();
   }

   void set_revision(uint64_t revision) {
      if (_rows.session_count() != 0)
         BOOST_THROW_EXCEPTION(std::logic_error("cannot set revision while there is an existing undo stack"));
      if (static_cast<int64_t>(revision) < _revision)
         BOOST_THROW_EXCEPTION(std::logic_error("revision cannot decrease"));
      _revision = static_cast<int64_t>(revision);
   }

   int64_t revision() const { return _revision; }

   std::pair<uint64_t, uint64_t> revision_range() const {
      return {_revision - _rows.session_count(), _revision};
   }

private:
   int64_t oldest_session_revision() const { return _revision - _rows.session_count() + 1; }

   undo_table _rows;
   undo_table _entries;
   int64_t    _revision = 0;
};

/// Forwards the database's session events to the store, the way dedup_undo_index does for the transaction dedup.
class map_undo_participant final : public chainbase::abstract_index {
public:
   static constexpr uint16_t reserved_type_id = 0xFFFD; ///< next to dedup_undo_index, above any chainbase object type

   explicit map_undo_participant(map_kv_store& store)
      : chainbase::abstract_index(&store)
      , _store(store) {}

   void     set_revision(uint64_t revision) override { _store.set_revision(revision); }
   void     add_undo_session() override { _store.add_session(); }
   int64_t  revision() const override { return _store.revision(); }
   void     undo() const override { _store.undo(); }
   void     squash() const override { _store.squash(); }
   void     commit(int64_t revision) const override { _store.commit(revision); }
   void     undo_all() const override { _store.undo_all(); }
   uint32_t type_id() const override { return reserved_type_id; }
   uint64_t row_count() const override { return _store.row_count() + _store.index_entry_count(); }
   size_t   freelist_memory_usage() const override { return 0; }
   const std::string& type_name() const override { return _type_name; }
   std::pair<uint64_t, uint64_t> undo_stack_revision_range() const override { return _store.revision_range(); }
   void     remove_object(int64_t) override {}

private:
   map_kv_store& _store;
   std::string   _type_name = "kv_map_store";
};

void map_kv_store::join_undo_sessions(chainbase::database& db) {
   const auto range = revision_range();
   const auto db_range = db.undo_stack_revision_range();
   if (range != db_range && _rows.session_count() == 0)
      set_revision(db_range.second);
   db.add_undo_participant(std::make_unique<map_undo_participant>(*this));
}

} // namespace

std::shared_ptr<chain::kv_store> make_map_kv_store() { return std::make_shared<map_kv_store>(); }

} // namespace sysio::testing
