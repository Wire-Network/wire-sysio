#include <sysio/chain/kv_iterator_steps.hpp>
#include <sysio/chain/kv_store.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/state_history/create_deltas.hpp>
#include <sysio/state_history/serialization.hpp>
#include <sysio/testing/kv_map_store.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/test/unit_test.hpp>

#include <fc/crypto/hex.hpp>
#include <fc/io/json.hpp>
#include <test_contracts.hpp>

#include <magic_enum/magic_enum.hpp>

#include "snapshot_suites.hpp"
#include "snapshot_tester.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

// The kv_store contract, checked on both engines. docs/kv-store-seam.md gives the rules. The KV suites also run with
// the std::map engine behind a whole chain: see unittests/CMakeLists.txt.

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

namespace {

constexpr uint64_t state_size_bytes = 128 * 1024 * 1024;

/// Both engines under one database, so that one undo session drives both and they stay at the same revision.
struct engine_pair {
   fc::temp_directory        dir;
   chainbase::database       db;
   std::shared_ptr<kv_store> chainbase_engine;
   std::shared_ptr<kv_store> map_engine;

   engine_pair()
      : db(dir.path(), chainbase::database::read_write, state_size_bytes) {
      db.add_index<kv_index>();
      db.add_index<kv_index_index>();
      chainbase_engine = make_chainbase_kv_store(db);
      map_engine       = make_map_kv_store();
      chainbase_engine->join_undo_sessions(db);
      map_engine->join_undo_sessions(db);
   }

   std::array<kv_store*, 2> engines() { return {chainbase_engine.get(), map_engine.get()}; }
};

std::string hex(std::string_view bytes) { return fc::to_hex(bytes.data(), bytes.size()); }

std::string render(const kv_row& row) {
   return std::format("{}/{}/{}={}@{}", row.code.to_string(), row.table_id, hex(row.key), hex(row.value),
                      row.payer.to_string());
}

std::string render(const kv_index_entry& entry) {
   return std::format("{}/{}/{}/{}@{}", entry.code.to_string(), entry.table_id, hex(entry.sec_key), hex(entry.pri_key),
                      entry.payer.to_string());
}

template<typename Row>
std::string render(const kv_delta<Row>& delta) {
   return (delta.present ? "+" : "-") + render(delta.row);
}

template<typename Row>
std::string render(bool found, const Row& row) {
   return found ? render(row) : std::string("end");
}

template<typename Cursor>
std::vector<std::string> walk_forward(Cursor&& cursor) {
   std::vector<std::string> rows;
   typename std::remove_cvref_t<decltype(*cursor)>::row_type row;
   for (bool more = cursor->seek_lower({}, row); more; more = cursor->next(row))
      rows.push_back(render(row));
   return rows;
}

template<typename Cursor>
std::vector<std::string> walk_backward(Cursor&& cursor) {
   std::vector<std::string> rows;
   typename std::remove_cvref_t<decltype(*cursor)>::row_type row;
   for (bool more = cursor->prev(row); more; more = cursor->prev(row)) // a new cursor is at end
      rows.push_back(render(row));
   std::reverse(rows.begin(), rows.end());
   return rows;
}

template<typename Delta>
std::vector<std::string> rendered(const std::vector<Delta>& deltas) {
   std::vector<std::string> out;
   for (const auto& delta : deltas)
      out.push_back(render(delta));
   return out;
}

/// Keys whose sizes straddle kv_key_blob's inline capacity and its integer comparison paths, with shared prefixes so
/// that ordering, seeks and collisions all get exercised.
std::vector<std::string> key_pool() {
   std::vector<std::string> keys;
   for (size_t size : {1, 2, 7, 8, 9, 15, 16, 17, 23, 24, 25, 29, 30, 31, 32, 33, 64, 256, 511, 512}) {
      for (char fill : {'\x00', '\x01', '\x7f', '\x80', '\xff'}) {
         std::string key(size, fill);
         keys.push_back(key);
         key.back() = '\x41';
         keys.push_back(std::move(key));
      }
   }
   return keys;
}

/// Applies one random sequence of operations to both engines, and fails on the first difference between them.
class differential_run {
public:
   explicit differential_run(uint64_t seed)
      : _rng(seed) {}

   void run(uint32_t operations) {
      for (_operation = 0; _operation < operations; ++_operation) {
         step();
         BOOST_REQUIRE_EQUAL(a().row_count(), b().row_count());
         BOOST_REQUIRE_EQUAL(a().index_entry_count(), b().index_entry_count());
         if (_operation % full_check_interval == 0) check_everything();
      }
      check_everything();
      while (!_sessions.empty()) undo_session();
      check_everything();
   }

private:
   static constexpr uint32_t full_check_interval = 64;
   static constexpr size_t   max_open_sessions   = 6;
   static constexpr int      walk_steps          = 12;
   static constexpr int      operation_kinds     = 100;

   kv_store& a() { return *_engines.chainbase_engine; }
   kv_store& b() { return *_engines.map_engine; }

   uint64_t pick(uint64_t count) { return _rng() % count; }
   account_name code() { return std::array{"alpha"_n, "beta"_n, "gamma"_n}[pick(3)]; }
   uint16_t table() { return std::array<uint16_t, 3>{0, 7, 0xFFFF}[pick(3)]; }
   account_name payer() { return std::array{"payera"_n, "payerb"_n}[pick(2)]; }
   const std::string& key() { return _keys[pick(_keys.size())]; }
   std::string value() { return std::string(pick(48), static_cast<char>(pick(256))); }

   void step() {
      const int kind = static_cast<int>(pick(operation_kinds));
      if (kind < 25) return insert_row();
      if (kind < 40) return update_row();
      if (kind < 50) return erase_row();
      if (kind < 65) return insert_entry();
      if (kind < 72) return update_entry();
      if (kind < 80) return erase_entry();
      if (kind < 85) return open_session();
      if (kind < 88) return undo_session();
      if (kind < 91) return squash_session();
      if (kind < 94) return push_session();
      if (kind < 96) return commit_sessions();
      return probe_cursors();
   }

   void insert_row() {
      const auto c = code();
      const auto t = table();
      const auto p = payer();
      const auto& k = key();
      const auto v = value();
      BOOST_REQUIRE_EQUAL(a().insert_row(c, t, p, k, v).has_value(), b().insert_row(c, t, p, k, v).has_value());
   }

   void update_row() {
      const auto c = code();
      const auto t = table();
      const auto& k = key();
      const auto row_a = a().find_row(c, t, k);
      const auto row_b = b().find_row(c, t, k);
      BOOST_REQUIRE_EQUAL(row_a.has_value(), row_b.has_value());
      if (!row_a) return;
      BOOST_REQUIRE_EQUAL(render(*row_a), render(*row_b));
      const auto p = payer();
      const auto v = value();
      BOOST_REQUIRE_EQUAL(render(a().update_row(*row_a, p, v)), render(b().update_row(*row_b, p, v)));
   }

   void erase_row() {
      const auto c = code();
      const auto t = table();
      const auto& k = key();
      const auto row_a = a().find_row(c, t, k);
      const auto row_b = b().find_row(c, t, k);
      BOOST_REQUIRE_EQUAL(row_a.has_value(), row_b.has_value());
      if (!row_a) return;
      a().erase_row(*row_a);
      b().erase_row(*row_b);
   }

   void insert_entry() {
      const auto c = code();
      const auto t = table();
      const auto p = payer();
      const auto& s = key();
      const auto& k = key();
      BOOST_REQUIRE_EQUAL(a().insert_index_entry(c, t, p, s, k), b().insert_index_entry(c, t, p, s, k));
   }

   void update_entry() {
      const auto c = code();
      const auto t = table();
      const auto& s = key();
      const auto& k = key();
      const auto& moved_to = key();
      const auto entry_a = a().find_index_entry(c, t, s, k);
      const auto entry_b = b().find_index_entry(c, t, s, k);
      BOOST_REQUIRE_EQUAL(entry_a.has_value(), entry_b.has_value());
      if (!entry_a) return;
      BOOST_REQUIRE_EQUAL(render(*entry_a), render(*entry_b));
      if (moved_to != s && a().find_index_entry(c, t, moved_to, k)) return; // the caller rules out a collision
      const auto p = payer();
      a().update_index_entry(*entry_a, p, moved_to);
      b().update_index_entry(*entry_b, p, moved_to);
   }

   void erase_entry() {
      const auto c = code();
      const auto t = table();
      const auto& s = key();
      const auto& k = key();
      const auto entry_a = a().find_index_entry(c, t, s, k);
      const auto entry_b = b().find_index_entry(c, t, s, k);
      BOOST_REQUIRE_EQUAL(entry_a.has_value(), entry_b.has_value());
      if (!entry_a) return;
      a().erase_index_entry(*entry_a);
      b().erase_index_entry(*entry_b);
   }

   void open_session() {
      if (_sessions.size() < max_open_sessions) _sessions.push_back(_engines.db.start_undo_session(true));
   }

   void undo_session() {
      if (!_sessions.empty()) {
         _sessions.back().undo();
         _sessions.pop_back();
      } else if (_pushed > 0) {
         _engines.db.undo();
         --_pushed;
      }
   }

   void squash_session() {
      if (!_sessions.empty()) {
         _sessions.back().squash();
         _sessions.pop_back();
      } else if (_pushed > 0) {
         _engines.db.squash();
         --_pushed;
      }
   }

   void push_session() {
      if (_sessions.size() == 1) {
         _sessions.back().push();
         _sessions.pop_back();
         ++_pushed;
      }
   }

   void commit_sessions() {
      if (!_sessions.empty() || _pushed == 0) return;
      const uint64_t committed = 1 + pick(_pushed);
      _engines.db.commit(_engines.db.revision() - (_pushed - committed));
      _pushed -= committed;
   }

   template<typename Cursor>
   void compare_walk(Cursor&& cursor_a, Cursor&& cursor_b, const kv_position& at) {
      typename std::remove_cvref_t<decltype(*cursor_a)>::row_type row_a, row_b;
      const auto compare = [&](bool found_a, bool found_b) {
         BOOST_REQUIRE_EQUAL(found_a, found_b);
         if (found_a) BOOST_REQUIRE_EQUAL(render(row_a), render(row_b));
      };
      switch (pick(3)) {
      case 0: compare(cursor_a->seek_lower(at, row_a), cursor_b->seek_lower(at, row_b)); break;
      case 1: compare(cursor_a->seek_upper(at, row_a), cursor_b->seek_upper(at, row_b)); break;
      default:
         compare(cursor_a->seek_past_table(at.code, at.table_id, row_a),
                 cursor_b->seek_past_table(at.code, at.table_id, row_b));
      }
      for (int walked = 0; walked < walk_steps; ++walked) {
         if (pick(2) == 0) compare(cursor_a->next(row_a), cursor_b->next(row_b));
         else              compare(cursor_a->prev(row_a), cursor_b->prev(row_b));
         compare(cursor_a->read(row_a), cursor_b->read(row_b));
         compare(cursor_a->clone()->read(row_a), cursor_b->clone()->read(row_b));
      }
   }

   void probe_cursors() {
      const auto c = code();
      const auto t = table();
      compare_walk(a().open_row_cursor(), b().open_row_cursor(), {c, t, key()});
      compare_walk(a().open_index_cursor(), b().open_index_cursor(), {c, t, key(), key()});
   }

   void check_everything() {
      BOOST_REQUIRE(walk_forward(a().open_row_cursor()) == walk_forward(b().open_row_cursor()));
      BOOST_REQUIRE(walk_forward(a().open_index_cursor()) == walk_forward(b().open_index_cursor()));
      for (kv_store* engine : {&a(), &b()}) {
         BOOST_REQUIRE(walk_forward(engine->open_row_cursor()) == walk_backward(engine->open_row_cursor()));
         BOOST_REQUIRE(walk_forward(engine->open_index_cursor()) == walk_backward(engine->open_index_cursor()));
      }
      BOOST_REQUIRE(rendered(a().row_deltas()) == rendered(b().row_deltas()));
      BOOST_REQUIRE(rendered(a().index_entry_deltas()) == rendered(b().index_entry_deltas()));
   }

   engine_pair                                _engines;
   std::mt19937_64                            _rng;
   std::vector<std::string>                   _keys = key_pool();
   std::vector<chainbase::database::session>  _sessions;
   uint64_t                                   _pushed = 0;
   uint32_t                                   _operation = 0;
};

constexpr auto     code_a  = "alice"_n;
constexpr auto     code_b  = "bob"_n;
constexpr auto     payer_a = "carol"_n;
constexpr auto     payer_b = "dave"_n;
constexpr uint16_t table_a = 10;
constexpr uint16_t table_b = 11;

/// One KV table's history deltas in printable form, `full` for the whole table or else for the last block.
std::string kv_section(const controller& control, bool full, const char* table_name) {
   namespace bio = boost::iostreams;
   std::vector<char> buffer;
   bio::filtering_ostreambuf out;
   out.push(bio::back_inserter(buffer));
   state_history::pack_deltas(out, control.db(), control.kv(), full);

   fc::datastream<const char*> in{buffer.data(), buffer.size()};
   fc::unsigned_int table_count;
   fc::raw::unpack(in, table_count);
   for (uint32_t table = 0; table < table_count.value; ++table) {
      fc::unsigned_int version, row_count;
      std::string      name;
      fc::raw::unpack(in, version);
      fc::raw::unpack(in, name);
      fc::raw::unpack(in, row_count);
      std::string section;
      for (uint32_t row = 0; row < row_count.value; ++row) {
         bool              present = false;
         std::vector<char> bytes;
         fc::raw::unpack(in, present);
         fc::raw::unpack(in, bytes);
         section += (present ? "+" : "-") + fc::to_hex(bytes.data(), bytes.size()) + "\n";
      }
      if (name == table_name) return section;
   }
   return {};
}

fc::mutable_variant_object bench_args(uint32_t begin, uint32_t count, uint32_t total) {
   return fc::mutable_variant_object()("begin", begin)("count", count)("total", total);
}

// --- The iterator steps over each engine, against a model that holds its position by key ---

using model_key = std::pair<std::string, std::string>; ///< (key, "") for a row, (sec_key, pri_key) for an entry

/// An iterator as the intrinsics promise it: a key position that any write to the rows around it leaves in place.
class iterator_model {
public:
   iterator_model(const std::set<model_key>& rows, std::string prefix)
      : _rows(rows)
      , _prefix(std::move(prefix)) {}

   void seek(const std::string& to) { land(_rows.lower_bound({std::max(to, _prefix), {}})); }
   void next() {
      if (_position) land(_rows.upper_bound(*_position));
   }
   void prev() {
      const auto after = _position ? _rows.lower_bound(*_position) : past_last_in_prefix();
      if (after == _rows.begin()) _position.reset();
      else                        land(std::prev(after));
   }
   kv_status read() const {
      if (!_position) return kv_status::end;
      return _rows.contains(*_position) ? kv_status::ok : kv_status::no_row;
   }
   const std::optional<model_key>& position() const { return _position; }

private:
   using rows_iterator = std::set<model_key>::const_iterator;

   void land(rows_iterator itr) {
      if (itr != _rows.end() && itr->first.starts_with(_prefix)) _position = *itr;
      else                                                       _position.reset();
   }
   rows_iterator past_last_in_prefix() const {
      for (auto itr = _rows.end(); itr != _rows.begin(); --itr)
         if (std::prev(itr)->first.starts_with(_prefix)) return itr;
      return _rows.begin();
   }

   const std::set<model_key>& _rows;
   std::string                _prefix;
   std::optional<model_key>   _position;
};

constexpr account_name     model_code        = code_a;
constexpr uint16_t         model_table       = 7; ///< the table under test
constexpr std::string_view model_value       = "v";
constexpr std::string_view model_new_value   = "changed";
constexpr uint32_t         model_buffer_size = 8;

/// The writes on rows that the iterator intrinsics have to survive, in the order kv_set and kv_erase make them.
struct primary_model_ops {
   using family = kv_primary_family;
   using pool_t = kv_primary_iterator_pool;

   static bool insert(kv_store& store, account_name code, uint16_t table, const model_key& key) {
      return store.insert_row(code, table, code, key.first, model_value).has_value();
   }
   static void erase(kv_store& store, pool_t& pool, const model_key& key) {
      const auto row = *store.find_row(model_code, model_table, key.first);
      pool.forget_row(row);
      store.erase_row(row);
   }
   static void change(kv_store& store, pool_t&, const model_key& key, std::string) {
      store.update_row(*store.find_row(model_code, model_table, key.first), payer_b, model_new_value);
   }
   static model_key make_key(std::string first, std::string) { return {std::move(first), {}}; }
   static model_key landed(const kv_row_buffers& out) { return {{out.first, out.first_size}, {}}; }
   static bool same_rest(const kv_row_buffers& out, std::string_view value) {
      return std::string_view(out.second, out.second_size) == value;
   }
};

/// The same for index entries, where a change moves the entry, as kv_idx_store, kv_idx_erase and kv_idx_update do.
struct secondary_model_ops {
   using family = kv_secondary_family;
   using pool_t = kv_secondary_iterator_pool;

   static bool insert(kv_store& store, account_name code, uint16_t table, const model_key& key) {
      return store.insert_index_entry(code, table, code, key.first, key.second);
   }
   static void erase(kv_store& store, pool_t& pool, const model_key& key) {
      const auto entry = *store.find_index_entry(model_code, model_table, key.first, key.second);
      pool.forget_row(entry);
      store.erase_index_entry(entry);
   }
   static void change(kv_store& store, pool_t& pool, const model_key& key, std::string new_sec_key) {
      if (store.find_index_entry(model_code, model_table, new_sec_key, key.second)) return;
      const auto entry = *store.find_index_entry(model_code, model_table, key.first, key.second);
      pool.forget_row(entry);
      store.update_index_entry(entry, model_code, new_sec_key);
   }
   static model_key make_key(std::string first, std::string second) { return {std::move(first), std::move(second)}; }
   static model_key landed(const kv_row_buffers& out) {
      return {{out.first, out.first_size}, {out.second, out.second_size}};
   }
   static bool same_rest(const kv_row_buffers&, std::string_view) { return true; } ///< an entry has only its keys
};

/// Keys are short and drawn from the bytes that matter to the bounds, so positions collide, prefixes end on 0xff, and a
/// row can sit exactly at a prefix's successor.
std::string pick_model_key(std::mt19937_64& rng) {
   constexpr std::array<char, 3> alphabet{'\x00', '\x01', '\xff'};
   constexpr size_t              max_length = 3;
   std::string                   key(1 + rng() % max_length, '\x00');
   for (auto& byte : key)
      byte = alphabet[rng() % alphabet.size()];
   return key;
}

/// One round: a few iterators walk a small table while writes land around them, and each move is checked. Other codes
/// hold rows in the same table id on both sides. Neighboring tables appear in half the rounds, because without them
/// the first row past either end of the table belongs to another code.
template<typename Ops>
void run_iterator_model_round(kv_store& store, std::mt19937_64& rng) {
   using family = typename Ops::family;
   constexpr int rows_per_table = 12;
   constexpr int iterators      = 3;
   constexpr int steps          = 60;
   const std::array<std::string, 6> prefixes{"",           std::string(1, '\x00'), "\x01",
                                             "\xff",       "\xff\xff",             "\x01\xff"};
   const account_name code_below(model_code.to_uint64_t() - 1);
   const account_name code_above(model_code.to_uint64_t() + 1);
   const bool         neighbor_tables = rng() % 2 == 0;

   std::set<model_key>                rows;
   std::map<model_key, std::string_view> values;
   const auto insert = [&](account_name code, uint16_t table) {
      const auto key = Ops::make_key(pick_model_key(rng), pick_model_key(rng));
      if (Ops::insert(store, code, table, key) && code == model_code && table == model_table) {
         rows.insert(key);
         values[key] = model_value;
      }
   };
   for (int i = 0; i < rows_per_table; ++i) {
      insert(model_code, model_table);
      insert(code_below, model_table);
      insert(code_above, model_table);
      if (neighbor_tables) {
         insert(model_code, model_table - 1);
         insert(model_code, model_table + 1);
      }
   }
   const auto any_row = [&] { return *std::next(rows.begin(), rng() % rows.size()); };

   typename Ops::pool_t        pool;
   std::vector<uint32_t>       handles;
   std::vector<iterator_model> models;
   for (int i = 0; i < iterators; ++i) {
      const auto& prefix = prefixes[rng() % prefixes.size()];
      handles.push_back(pool.allocate(model_table, model_code, prefix));
      models.emplace_back(rows, prefix);
      auto& slot = pool.get(handles.back());
      kv_prepare_cursor<family>(store, slot);
      typename family::row_type row;
      kv_seek<family>(slot, {}, row);
      models.back().seek({});
   }

   char first[model_buffer_size];
   char second[model_buffer_size];
   const auto check_row = [&](const iterator_model& model, const kv_row_buffers& out) {
      BOOST_REQUIRE(Ops::landed(out) == *model.position());
      BOOST_REQUIRE(Ops::same_rest(out, values.at(*model.position())));
   };
   const auto check_landing = [&](kv_status status, const iterator_model& model, const kv_row_buffers& out) {
      BOOST_REQUIRE_EQUAL(status == kv_status::end, !model.position());
      if (model.position()) check_row(model, out);
   };

   enum class step { next, prev, seek, read, insert, erase, change };
   for (int done = 0; done < steps; ++done) {
      const size_t which = rng() % iterators;
      auto&        slot  = pool.get(handles[which]);
      auto&        model = models[which];
      kv_row_buffers out{first, model_buffer_size, second, model_buffer_size};
      typename family::row_type row;
      switch (magic_enum::enum_value<step>(rng() % magic_enum::enum_count<step>())) {
      case step::next:
         kv_step_next<family>(slot, row);
         model.next();
         check_landing(kv_report_landing<family>(slot, row, out), model, out);
         break;
      case step::prev:
         kv_step_prev<family>(slot, row);
         model.prev();
         check_landing(kv_report_landing<family>(slot, row, out), model, out);
         break;
      case step::seek: {
         const auto to = pick_model_key(rng);
         kv_seek<family>(slot, to, row);
         model.seek(to);
         check_landing(kv_report_landing<family>(slot, row, out), model, out);
         break;
      }
      case step::read: {
         const kv_status status = kv_report_position<family>(slot, 0, out);
         BOOST_REQUIRE_EQUAL(magic_enum::enum_name(status), magic_enum::enum_name(model.read()));
         if (status == kv_status::ok) check_row(model, out);
         break;
      }
      case step::insert:
         insert(model_code, model_table);
         break;
      case step::erase:
         if (rows.empty()) break;
         {
            const auto key = any_row();
            Ops::erase(store, pool, key);
            rows.erase(key);
            values.erase(key);
         }
         break;
      case step::change:
         if (rows.empty()) break;
         {
            const auto key     = any_row();
            const auto new_sec = pick_model_key(rng);
            Ops::change(store, pool, key, new_sec);
            if constexpr (std::is_same_v<Ops, secondary_model_ops>) {
               if (!rows.contains({new_sec, key.second})) {
                  rows.erase(key);
                  values.erase(key);
                  rows.insert({new_sec, key.second});
                  values[{new_sec, key.second}] = model_value;
               }
            } else {
               values[key] = model_new_value;
            }
         }
         break;
      }
   }
}

} // namespace

BOOST_AUTO_TEST_SUITE(kv_store_tests)

BOOST_AUTO_TEST_CASE(random_operations_match) {
   constexpr uint32_t operations_per_seed = 4'000;
   for (uint64_t seed : {1u, 2u, 3u, 42u, 1729u, 65537u}) {
      BOOST_TEST_CONTEXT("seed " << seed) {
         differential_run(seed).run(operations_per_seed);
      }
   }
}

BOOST_AUTO_TEST_CASE(keys_sort_as_unsigned_bytes_with_a_prefix_first) {
   engine_pair pair;
   // Sorted already. 0x80 and 0xff must sort above 0x7f, and each key below the keys it prefixes.
   const std::vector<std::string> keys{{"\x00", 1}, {"\x00\x00", 2}, "\x01", "\x7f", "\x80", "\x80\x01", "\xff"};
   for (auto* engine : pair.engines()) {
      for (auto key = keys.rbegin(); key != keys.rend(); ++key)
         BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, *key, "v"));
      auto cursor = engine->open_row_cursor();
      kv_row row;
      bool on_row = cursor->seek_lower({}, row);
      for (const auto& key : keys) {
         BOOST_REQUIRE(on_row);
         BOOST_CHECK_EQUAL(hex(row.key), hex(key));
         on_row = cursor->next(row);
      }
      BOOST_CHECK(!on_row);
   }
}

BOOST_AUTO_TEST_CASE(index_entries_sort_by_secondary_then_primary_key) {
   engine_pair pair;
   for (auto* engine : pair.engines()) {
      // ("a", "z") sorts before ("ab", "a"), although joining the bytes would put them the other way round.
      BOOST_REQUIRE(engine->insert_index_entry(code_a, table_a, payer_a, "ab", "a"));
      BOOST_REQUIRE(engine->insert_index_entry(code_a, table_a, payer_a, "a", "z"));
      BOOST_REQUIRE(engine->insert_index_entry(code_a, table_a, payer_a, "a", "b"));
      auto cursor = engine->open_index_cursor();
      kv_index_entry entry;
      BOOST_REQUIRE(cursor->seek_lower({code_a, table_a, "a"}, entry));
      BOOST_CHECK_EQUAL(entry.pri_key, "b");
      BOOST_REQUIRE(cursor->next(entry));
      BOOST_CHECK_EQUAL(entry.pri_key, "z");
      BOOST_REQUIRE(cursor->next(entry));
      BOOST_CHECK_EQUAL(entry.sec_key, "ab");
      BOOST_CHECK(!cursor->next(entry));
   }
}

BOOST_AUTO_TEST_CASE(cursor_edges) {
   engine_pair pair;
   for (auto* engine : pair.engines()) {
      kv_row row;
      auto cursor = engine->open_row_cursor();
      // An empty store has only the end position.
      BOOST_CHECK(!cursor->seek_lower({}, row));
      BOOST_CHECK(!cursor->read(row));
      BOOST_CHECK(!cursor->next(row));
      BOOST_CHECK(!cursor->prev(row));

      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, "a", "1"));
      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, "b", "2"));
      BOOST_REQUIRE(engine->insert_row(code_a, table_b, payer_a, "a", "3"));

      // Off the top and the bottom lands on end, and prev from end lands on the last row.
      BOOST_REQUIRE(cursor->seek_lower({code_a, table_b, "a"}, row));
      BOOST_CHECK(!cursor->next(row));
      BOOST_CHECK(!cursor->next(row));
      BOOST_REQUIRE(cursor->prev(row));
      BOOST_CHECK_EQUAL(row.table_id, table_b);
      BOOST_REQUIRE(cursor->seek_lower({code_a, table_a, "a"}, row));
      BOOST_CHECK(!cursor->prev(row));
      BOOST_REQUIRE(cursor->prev(row));
      BOOST_CHECK_EQUAL(row.table_id, table_b);

      // A seek crosses tables, because a cursor walks the whole store.
      BOOST_REQUIRE(cursor->seek_upper({code_a, table_a, "b"}, row));
      BOOST_CHECK_EQUAL(row.table_id, table_b);
      BOOST_REQUIRE(cursor->seek_past_table(code_a, table_a, row));
      BOOST_CHECK_EQUAL(row.table_id, table_b);
      BOOST_CHECK(!cursor->seek_past_table(code_a, table_b, row));
      BOOST_CHECK(!cursor->seek_past_table(code_b, table_a, row));
   }
}

BOOST_AUTO_TEST_CASE(cursor_sees_the_store_as_it_stands) {
   engine_pair pair;
   for (auto* engine : pair.engines()) {
      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, "b", "1"));
      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, "d", "2"));
      kv_row row;
      auto cursor = engine->open_row_cursor();
      BOOST_REQUIRE(cursor->seek_lower({code_a, table_a, "b"}, row));

      // A row stored after the cursor moved shows up, and a value update reads back through the cursor.
      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, "c", "3"));
      const auto updated = engine->update_row(*engine->find_row(code_a, table_a, "b"), payer_b, "changed");
      BOOST_CHECK_EQUAL(updated.value, "changed");
      BOOST_REQUIRE(cursor->read(row));
      BOOST_CHECK_EQUAL(row.value, "changed");
      BOOST_CHECK_EQUAL(row.payer, payer_b);
      BOOST_REQUIRE(cursor->next(row));
      BOOST_CHECK_EQUAL(row.key, "c");

      // Erasing a row ahead of the cursor leaves it out of the walk.
      engine->erase_row(*engine->find_row(code_a, table_a, "d"));
      BOOST_CHECK(!cursor->next(row));
   }
}

BOOST_AUTO_TEST_CASE(a_payer_update_leaves_a_cursor_on_its_entry) {
   engine_pair pair;
   for (auto* engine : pair.engines()) {
      BOOST_REQUIRE(engine->insert_index_entry(code_a, table_a, payer_a, "s", "p"));
      BOOST_REQUIRE(engine->insert_index_entry(code_a, table_a, payer_a, "t", "q"));
      kv_index_entry entry;
      auto cursor = engine->open_index_cursor();
      BOOST_REQUIRE(cursor->seek_lower({code_a, table_a, "s", "p"}, entry));

      engine->update_index_entry(*engine->find_index_entry(code_a, table_a, "s", "p"), payer_b, "s");
      BOOST_REQUIRE(cursor->read(entry));
      BOOST_CHECK_EQUAL(entry.payer, payer_b);
      BOOST_REQUIRE(cursor->next(entry));
      BOOST_CHECK_EQUAL(entry.sec_key, "t");
   }
}

// A key that crosses kv_key_blob's inline limit moves between the row and a blob, in both directions and through undo.
BOOST_AUTO_TEST_CASE(keys_cross_the_inline_limit) {
   const std::string fits(kv_key_blob::inline_capacity, 's');
   const std::string spills(kv_key_blob::inline_capacity + 1, 's');
   const std::string pri_key(kv_key_blob::inline_capacity + 1, 'p');

   engine_pair pair;
   for (auto* engine : pair.engines()) {
      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, fits, "1"));
      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, spills, "2"));
      BOOST_CHECK_EQUAL(engine->find_row(code_a, table_a, fits)->value, "1");
      BOOST_CHECK_EQUAL(engine->find_row(code_a, table_a, spills)->value, "2");

      BOOST_REQUIRE(engine->insert_index_entry(code_a, table_a, payer_a, fits, pri_key));
      {
         auto session = pair.db.start_undo_session(true);
         engine->update_index_entry(*engine->find_index_entry(code_a, table_a, fits, pri_key), payer_a, spills);
         BOOST_CHECK(!engine->find_index_entry(code_a, table_a, fits, pri_key));
         BOOST_REQUIRE(engine->find_index_entry(code_a, table_a, spills, pri_key));

         engine->update_index_entry(*engine->find_index_entry(code_a, table_a, spills, pri_key), payer_b, fits);
         BOOST_CHECK(!engine->find_index_entry(code_a, table_a, spills, pri_key));
         BOOST_CHECK_EQUAL(engine->find_index_entry(code_a, table_a, fits, pri_key)->payer, payer_b);

         engine->update_index_entry(*engine->find_index_entry(code_a, table_a, fits, pri_key), payer_b, spills);
         session.undo();
      }
      BOOST_CHECK(!engine->find_index_entry(code_a, table_a, spills, pri_key));
      BOOST_CHECK_EQUAL(engine->find_index_entry(code_a, table_a, fits, pri_key)->payer, payer_a);

      engine->erase_index_entry(*engine->find_index_entry(code_a, table_a, fits, pri_key));
      engine->erase_row(*engine->find_row(code_a, table_a, fits));
      engine->erase_row(*engine->find_row(code_a, table_a, spills));
   }
}

// What docs/kv-ram-billing.md says a row and an index entry store on chainbase. Bytes per row are measured over many
// rows, so that allocator batches do not show.
BOOST_AUTO_TEST_CASE(stored_size_matches_the_billing_doc) {
   constexpr uint32_t row_count = 20000;
   struct shape {
      uint32_t first_size;
      uint32_t second_size;
      uint32_t stored;
   };
   constexpr std::array<shape, 11> entries{{{24, 24, 123}, {16, 16, 123}, {8, 8, 123}, {4, 4, 123}, {32, 8, 164},
                                            {64, 64, 268}, {31, 31, 204}, {30, 30, 204}, {512, 512, 1174},
                                            {25, 25, 204}, {33, 33, 220}}};
   constexpr std::array<shape, 8> rows{{{24, 1, 123}, {16, 64, 180}, {8, 8, 123}, {8, 1, 123}, {4, 1, 123},
                                        {31, 1, 164}, {25, 1, 164}, {33, 1, 172}}};

   // Holds `number`, so a size has to be at least sizeof(number).
   const auto key = [](uint32_t size, uint32_t number) {
      std::string bytes(size, 'k');
      std::memcpy(bytes.data(), &number, sizeof(number));
      return bytes;
   };
   const auto bytes_per_row = [&](auto& pair, auto&& store) {
      const size_t before = pair.db.get_free_memory();
      for (uint32_t number = 0; number < row_count; ++number) store(number);
      return (before - pair.db.get_free_memory()) / double(row_count);
   };
   constexpr double allowed_error = 2.0;

   for (const auto& entry : entries) {
      BOOST_TEST_CONTEXT("index entry " << entry.first_size << " + " << entry.second_size) {
         engine_pair pair;
         const double stored = bytes_per_row(pair, [&](uint32_t number) {
            pair.chainbase_engine->insert_index_entry(code_a, table_a, payer_a, key(entry.first_size, number),
                                                      key(entry.second_size, number));
         });
         BOOST_CHECK_SMALL(stored - entry.stored, allowed_error);
      }
   }
   for (const auto& row : rows) {
      BOOST_TEST_CONTEXT("row " << row.first_size << " + " << row.second_size) {
         engine_pair pair;
         const std::string value(row.second_size, 'v');
         const double stored = bytes_per_row(pair, [&](uint32_t number) {
            pair.chainbase_engine->insert_row(code_a, table_a, payer_a, key(row.first_size, number), value);
         });
         BOOST_CHECK_SMALL(stored - row.stored, allowed_error);
      }
   }
}

// A taken key is the one create failure that means "exists". A database refusing the write is not, and has to get out.
BOOST_AUTO_TEST_CASE(only_a_taken_key_counts_as_taken) {
   engine_pair pair;
   BOOST_REQUIRE(pair.chainbase_engine->insert_row(code_a, table_a, payer_a, "k", "v"));
   BOOST_REQUIRE(pair.chainbase_engine->insert_index_entry(code_a, table_a, payer_a, "s", "p"));

   pair.db.set_read_only_mode();
   const auto refused = [](const std::logic_error& e) {
      return !dynamic_cast<const chainbase::uniqueness_violation*>(&e);
   };
   BOOST_CHECK_EXCEPTION(pair.chainbase_engine->insert_row(code_a, table_a, payer_a, "free", "v"), std::logic_error,
                         refused);
   BOOST_CHECK_EXCEPTION(pair.chainbase_engine->insert_index_entry(code_a, table_a, payer_a, "free", "p"),
                         std::logic_error, refused);
   pair.db.unset_read_only_mode();
}

BOOST_AUTO_TEST_CASE(a_taken_key_changes_nothing) {
   engine_pair pair;
   for (auto* engine : pair.engines()) {
      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, "k", "first"));
      BOOST_CHECK(!engine->insert_row(code_a, table_a, payer_b, "k", "second"));
      BOOST_CHECK_EQUAL(engine->find_row(code_a, table_a, "k")->value, "first");
      BOOST_CHECK_EQUAL(engine->find_row(code_a, table_a, "k")->payer, payer_a);

      BOOST_REQUIRE(engine->insert_index_entry(code_a, table_a, payer_a, "s", "p"));
      BOOST_CHECK(!engine->insert_index_entry(code_a, table_a, payer_b, "s", "p"));
      BOOST_CHECK_EQUAL(engine->find_index_entry(code_a, table_a, "s", "p")->payer, payer_a);
      BOOST_CHECK_EQUAL(engine->index_entry_count(), 1u);
   }
}

BOOST_AUTO_TEST_CASE(deltas_are_per_key_and_in_key_order) {
   engine_pair pair;
   for (auto* engine : pair.engines()) {
      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, "gone", "old"));
      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, "same", "old"));
      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, "swap", "old"));
      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, "undo", "old"));
      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, "zero", "old"));
      BOOST_REQUIRE(engine->insert_index_entry(code_a, table_a, payer_a, "from", "p"));
   }

   auto session = pair.db.start_undo_session(true);
   for (auto* engine : pair.engines()) {
      // The only outcomes that count are the ones the session ends with.
      engine->erase_row(*engine->find_row(code_a, table_a, "gone"));
      engine->update_row(*engine->find_row(code_a, table_a, "same"), payer_b, "changed");
      engine->update_row(*engine->find_row(code_a, table_a, "same"), payer_a, "old");
      engine->erase_row(*engine->find_row(code_a, table_a, "swap"));
      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, "swap", "new"));
      engine->erase_row(*engine->find_row(code_a, table_a, "undo"));
      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, "undo", "old"));
      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, "brief", "new"));
      engine->erase_row(*engine->find_row(code_a, table_a, "brief"));
      BOOST_REQUIRE(engine->insert_row(code_a, table_a, payer_a, "added", "new"));
      engine->update_row(*engine->find_row(code_a, table_a, "zero"), payer_b, "changed");
      engine->update_index_entry(*engine->find_index_entry(code_a, table_a, "from", "p"), payer_a, "to");
   }

   const std::vector<std::string> expected_rows{
      "+" + render(kv_row{code_a, table_a, payer_a, "added", "new"}),
      "-" + render(kv_row{code_a, table_a, payer_a, "gone", "old"}),
      "+" + render(kv_row{code_a, table_a, payer_a, "swap", "new"}),
      "+" + render(kv_row{code_a, table_a, payer_b, "zero", "changed"}),
   };
   const std::vector<std::string> expected_entries{
      "-" + render(kv_index_entry{code_a, table_a, payer_a, "from", "p"}),
      "+" + render(kv_index_entry{code_a, table_a, payer_a, "to", "p"}),
   };
   for (auto* engine : pair.engines()) {
      BOOST_TEST(rendered(engine->row_deltas()) == expected_rows, boost::test_tools::per_element());
      BOOST_TEST(rendered(engine->index_entry_deltas()) == expected_entries, boost::test_tools::per_element());
   }

   session.undo();
   for (auto* engine : pair.engines()) {
      BOOST_CHECK(engine->row_deltas().empty());
      BOOST_CHECK_EQUAL(engine->find_row(code_a, table_a, "swap")->value, "old");
      BOOST_CHECK(!engine->find_row(code_a, table_a, "added"));
      BOOST_CHECK(engine->find_index_entry(code_a, table_a, "from", "p"));
      BOOST_CHECK(!engine->find_index_entry(code_a, table_a, "to", "p"));
   }
}

// Two chains, one on each engine, one validating the other's blocks. A chain state that drifted would fail the push,
// and the hashes, history bytes and snapshots below check what block validation does not cover.
BOOST_AUTO_TEST_CASE(chains_on_both_engines_match) {
   constexpr uint32_t rows         = 3'000;
   constexpr uint32_t batch        = 500;
   constexpr auto     contract     = "benchkvops"_n;
   constexpr auto     table_section = "contract_row_kv";
   constexpr auto     index_section = "contract_index_kv";

   tester on_chainbase;
   tester on_map([](controller& control) { control.testing_set_kv_store(make_map_kv_store()); }, setup_policy::none);

   /// Hands every block `on_map` lacks to it.
   const auto sync = [&] {
      while (on_map.control->head().block_num() < on_chainbase.control->head().block_num())
         on_map.push_block(on_chainbase.control->fetch_block_by_number(on_map.control->head().block_num() + 1));
   };

   on_chainbase.create_accounts({contract}, false, true, false, false);
   on_chainbase.set_code(contract, test_contracts::bench_kv_ops_wasm());
   on_chainbase.set_abi(contract, test_contracts::bench_kv_ops_abi());
   on_chainbase.produce_block();
   sync();

   const auto compare = [&](bool full) {
      BOOST_REQUIRE_EQUAL(on_chainbase.control->head().id(), on_map.control->head().id());
      for (const char* section : {table_section, index_section}) {
         BOOST_REQUIRE_EQUAL(kv_section(*on_chainbase.control, full, section),
                             kv_section(*on_map.control, full, section));
      }
   };
   const auto apply = [&](action_name action, const fc::variant_object& args) {
      on_chainbase.push_action(contract, action, contract, args);
      on_map.push_block(on_chainbase.produce_block());
      on_chainbase.control->abort_block(); // or the top undo session would be the next block's
      compare(false);
   };

   for (uint32_t begin = 0; begin < rows; begin += batch)
      apply("populate"_n, bench_args(begin, batch, rows));
   compare(true);
   apply("opfindmod"_n, bench_args(0, batch, rows));
   apply("opemplace"_n, bench_args(0, batch, rows));
   apply("operase"_n, bench_args(0, batch, rows));
   compare(true);

   BOOST_REQUIRE_EQUAL(on_chainbase.control->calculate_integrity_hash().str(),
                       on_map.control->calculate_integrity_hash().str());

   using suite = variant_snapshot_suite;
   auto chainbase_writer = suite::get_writer();
   on_chainbase.control->write_snapshot(chainbase_writer);
   const auto chainbase_snapshot = suite::finalize(chainbase_writer);
   auto map_writer = suite::get_writer();
   on_map.control->write_snapshot(map_writer);
   const auto map_snapshot = suite::finalize(map_writer);
   BOOST_REQUIRE(fc::json::to_string(chainbase_snapshot, fc::time_point::maximum()) ==
                 fc::json::to_string(map_snapshot, fc::time_point::maximum()));

   // Each engine loads what the other wrote and hashes it the same.
   snapshotted_tester chainbase_from_map(on_chainbase.get_config(), suite::get_reader(map_snapshot), 1,
                                         snapshotted_tester::dont_copy_config_files, {}, kv_engine::chainbase);
   snapshotted_tester map_from_chainbase(on_chainbase.get_config(), suite::get_reader(chainbase_snapshot), 2,
                                         snapshotted_tester::dont_copy_config_files, {}, kv_engine::map);
   const auto expected = on_chainbase.control->calculate_integrity_hash().str();
   BOOST_REQUIRE_EQUAL(chainbase_from_map.control->calculate_integrity_hash().str(), expected);
   BOOST_REQUIRE_EQUAL(map_from_chainbase.control->calculate_integrity_hash().str(), expected);
}

BOOST_AUTO_TEST_CASE(iterators_match_a_model_by_key) {
   constexpr int      rounds = 3000;
   constexpr uint64_t seed   = 7;
   engine_pair pair;
   for (auto* engine : pair.engines()) {
      std::mt19937_64 rng(seed);
      for (int round = 0; round < rounds; ++round) {
         auto session = pair.db.start_undo_session(true);
         run_iterator_model_round<primary_model_ops>(*engine, rng);
         run_iterator_model_round<secondary_model_ops>(*engine, rng);
         session.undo();
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
