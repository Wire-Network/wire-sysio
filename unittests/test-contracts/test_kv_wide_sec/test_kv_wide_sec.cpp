// kv::table secondary indexes on key types outside multi_index's five.
//
// multi_index is limited to what upstream Antelope supports -- uint64_t, uint128_t, double,
// long double, checksum256 -- because it exists to carry a ported contract unchanged, and a
// port cannot have arrived with anything else. kv::table is the Wire-native path and has no
// such ceiling: kv::index keys through sysio::kv::be_key_stream, which encodes the narrow
// integers, the signed ones, name, and composite structs in an order-preserving big-endian
// form.
//
// That only matters if the chain agrees. get_table_rows builds its query bound with
// sysio::chain::be_key_codec, a separate implementation that has to produce the same bytes
// the contract stored -- and until this contract, the only kv::index in the test suite was on
// `name`. Everything else, composite keys included, was unverified end to end.
//
// The rows the test inserts are chosen so a little-endian encoding would order them
// differently, so the query results distinguish the two encodings rather than passing either
// way.
#include <sysio/sysio.hpp>
#include <sysio/kv_table.hpp>

using namespace sysio;

// A composite secondary key. be_key_codec expands a struct key field-by-field in declaration
// order, and be_key_stream encodes each field through the same overloads, so this sorts by
// tier and then by owner.
struct combo_key {
   uint32_t tier;
   name     owner;
   SYSLIB_SERIALIZE(combo_key, (tier)(owner))
};

class [[sysio::contract("test_kv_wide_sec")]] test_kv_wide_sec : public contract {
public:
   using contract::contract;

   struct item_key {
      uint64_t id;
      uint64_t primary_key() const { return id; }
      SYSLIB_SERIALIZE(item_key, (id))
   };

   struct [[sysio::table("items")]] item_val {
      uint32_t  small;
      int64_t   score;
      combo_key combo;
      SYSLIB_SERIALIZE(item_val, (small)(score)(combo))
   };

   using items_table = kv::table<"items"_n, item_key, item_val,
      kv::index<"bysmall"_n, kv::member_data<item_val, uint32_t,  &item_val::small>>,
      kv::index<"byscore"_n, kv::member_data<item_val, int64_t,   &item_val::score>>,
      kv::index<"bycombo"_n, kv::member_data<item_val, combo_key, &item_val::combo>>
   >;

   items_table items{get_self()};

   [[sysio::action]]
   void additem(uint64_t id, uint32_t small, int64_t score, uint32_t tier, name owner) {
      items.emplace(get_self(), {id}, {small, score, {tier, owner}});
   }
};
