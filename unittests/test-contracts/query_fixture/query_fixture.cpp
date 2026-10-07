#include <sysio/sysio.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/binary_extension.hpp>
#include <sysio/crypto.hpp>
#include <sysio/time.hpp>
#include <optional>
#include <variant>

using namespace sysio;

/// Actual WASM writes for query tests; every mutation requires the fixture account.
class [[sysio::contract("query_fixture")]] query_fixture : public contract {
public:
   using contract::contract;
   static constexpr auto positions_name = "positions"_n;
   static constexpr auto composite_name = "composite"_n;
   static constexpr auto wide_name = "wide"_n;
   /// This name hashes to table id 0xFFFF, the highest partition: scans over it cross no successor id.
   static constexpr auto fasp_name = "fasp"_n;
   static constexpr uint32_t maximum_seed_rows = 2048;
   static constexpr int64_t fixture_time_us = 1700000000123456;
   static constexpr auto fixture_symbol = symbol(symbol_code("SYS"), 4);
   static constexpr auto fixture_digest_source = "fixture";
   static constexpr uint32_t fixture_digest_length = 7;

   /// ABI enum with a shared member prefix and a gap in its values, rendered by name in query results.
   enum class position_status : uint8_t {
      POSITION_STATUS_OPEN = 0,
      POSITION_STATUS_CLOSED = 1,
      POSITION_STATUS_ARCHIVED = 5
   };

   /// Primary key with an explicit ABI field name.
   struct position_key {
      uint64_t id;
      SYSLIB_SERIALIZE(position_key, (id))
   };
   /// Nested variable-size values exercise lossless recursive normalization.
   struct nested_value {
      int64_t score;
      std::vector<uint64_t> numbers;
      SYSLIB_SERIALIZE(nested_value, (score)(numbers))
   };
   /// Value fields deliberately cover scalar, null, unit, time, container and enum families.
   struct [[sysio::table("positions")]] position {
      name beneficiary;
      int64_t amount;
      std::optional<int64_t> nullable;
      asset quantity;
      time_point created;
      std::string memo;
      nested_value nested;
      bool enabled;
      position_status status;
      SYSLIB_SERIALIZE(position, (beneficiary)(amount)(nullable)(quantity)(created)(memo)(nested)(enabled)(status))
   };
   /// Composite primary key with independently queryable leading fields.
   struct composite_key {
      name beneficiary;
      uint64_t sequence;
      SYSLIB_SERIALIZE(composite_key, (beneficiary)(sequence))
   };
   /// Wide numbers, opaque floats and a checksum are written by WASM only.
   struct [[sysio::table("wide")]] wide_value {
      int128_t signed_value;
      uint128_t unsigned_value;
      float float_value;
      double double_value;
      long double quad_value;
      checksum256 digest;
      std::variant<uint64_t, std::string> choice;
      binary_extension<uint64_t> extension;
      SYSLIB_SERIALIZE(wide_value, (signed_value)(unsigned_value)(float_value)(double_value)(quad_value)(digest)(choice)(extension))
   };
   using positions = kv::table<positions_name, position_key, position>;
   using composites = kv::table<composite_name, composite_key, position>;
   using wide_table = kv::table<wide_name, position_key, wide_value>;
   using highest_table = kv::table<fasp_name, position_key, position>;

   /// Insert or replace one position with caller-specified nulls, asset units and status.
   [[sysio::action]] void put(uint64_t id, position row) {
      require_auth(get_self());
      positions table(get_self());
      const auto found = table.find(position_key{id});
      if (found == table.end()) table.emplace(get_self(), position_key{id}, row);
      else table.modify(get_self(), found, row);
   }
   /// Remove one primary row to test updates and captured ownership.
   [[sysio::action]] void erase(uint64_t id) {
      require_auth(get_self());
      positions table(get_self());
      auto found = table.find(position_key{id});
      if (found != table.end()) table.erase(found);
   }
   /// Bounded batch insertion supplies data spanning multiple native capture pages.
   [[sysio::action]] void seed(uint64_t first, uint32_t count, name beneficiary, int64_t amount) {
      require_auth(get_self());
      check(count <= maximum_seed_rows, "Too many fixture rows");
      positions table(get_self());
      for (uint32_t i = 0; i < count; ++i)
         table.emplace(get_self(), position_key{first + i},
            position{beneficiary, amount, {}, asset(amount, fixture_symbol), time_point(microseconds(fixture_time_us)),
                     "fixture", {amount, {first + i}}, true, position_status::POSITION_STATUS_OPEN});
   }
   /// Write composite-key rows independently of the positions table.
   [[sysio::action]] void putcomp(name beneficiary, uint64_t sequence, position row) {
      require_auth(get_self());
      composites table(get_self());
      table.emplace(get_self(), composite_key{beneficiary, sequence}, row);
   }
   /// Write one row into the highest-numbered table partition.
   [[sysio::action]] void putfasp(uint64_t id, position row) {
      require_auth(get_self());
      highest_table table(get_self());
      table.emplace(get_self(), position_key{id}, row);
   }
   /// Fixed binary patterns are generated by the contract, never converted through host floating point.
   [[sysio::action]] void putwide(uint64_t id) {
      require_auth(get_self());
      wide_table table(get_self());
      const auto maximum = ~uint128_t{0};
      table.emplace(get_self(), position_key{id}, wide_value{
         -int128_t(maximum >> 1) - 1, maximum, 1.5f, -2.5, 0.0L, sha256(fixture_digest_source, fixture_digest_length),
         uint64_t{9007199254740993ULL}, {}});
   }
};
