#pragma once

#include "contract_test_support.hpp"
#include "contracts.hpp"
#include <sysio/opp/opp.hpp>
#include <tuple>

namespace sysio_system::test_support::registry {

/// Registry accounts and tables shared by collateral fixtures.
inline constexpr auto chains = "sysio.chains"_n;
inline constexpr auto tokens = "sysio.tokens"_n;
inline constexpr auto chains_table = "chains"_n;
inline constexpr auto tokens_table = "tokens"_n;
inline constexpr auto bindings_table = "chaintokens"_n;
inline constexpr std::string_view depot_code = "WIRE";
inline constexpr uint32_t token_precision = 9;

/// Deploy a registry only when absent, preserving existing rows and fixture setup.
template <typename Tester>
void deploy(Tester& tester, name account, const std::vector<uint8_t>& wasm, const std::vector<char>& abi) {
   if (!tester.control->find_account(account)) {
      tester.create_accounts({account});
      abi_serializer system_ser;
      load_account_abi(tester, config::system_account_name, system_ser);
      // Full-system fixtures give new sysio.* accounts no NET/CPU. BIOS fixtures
      // have unlimited resources and do not expose these allocation actions.
      if (!system_ser.get_action_type("setacctram"_n).empty()) {
         using mvo = fc::mutable_variant_object;
         for (const auto& [action, field, amount] : {
                 std::tuple{"setacctram"_n, "ram_bytes", int64_t{2'000'000}},
                 std::tuple{"setacctnet"_n, "net_weight", int64_t{1'000'000}},
                 std::tuple{"setacctcpu"_n, "cpu_weight", int64_t{1'000'000}}}) {
            BOOST_REQUIRE_EQUAL(Tester::success(), push_contract_action_and_produce_block(
               tester, config::system_account_name, system_ser, config::system_account_name,
               action, mvo()("account", account)(field, amount)));
         }
      }
   }
   const auto* metadata = tester.control->find_account_metadata(account);
   if (!metadata || metadata->abi.size() == 0) {
      tester.set_code(account, wasm);
      tester.set_abi(account, abi.data());
      tester.set_privileged(account);
      tester.produce_block();
   }
}

/// Read a composite-keyed binding through its ABI rather than assuming a uint64 primary key.
template <typename Tester>
fc::variant binding(Tester& tester, std::string_view chain_code, std::string_view token_code) {
   abi_serializer ser;
   load_account_abi(tester, tokens, ser);
   const auto table = compute_table_id(bindings_table.to_uint64_t());
   const auto& index = tester.control->db().template get_index<kv_index, by_code_key>();
   auto it = index.lower_bound(boost::make_tuple(tokens, table, std::string_view{}));
   for (; it != index.end() && it->code == tokens && it->table_id == table; ++it) {
      std::vector<char> raw(it->value.begin(), it->value.end());
      auto row = ser.binary_to_variant("chain_token_row", raw,
         abi_serializer::create_yield_function(Tester::abi_serializer_max_time));
      if (row["chain_code"].template as<fc::slug_name>() == fc::slug_name{chain_code} &&
          row["token_code"].template as<fc::slug_name>() == fc::slug_name{token_code}) return row;
   }
   return {};
}

/// Register the depot chain and token before a collateral consumer uses them.
/// Existing tokens retain their original metadata (including external shadow origins).
template <typename Tester>
void seed_depot_pair(Tester& tester, std::string_view token_code = depot_code) {
   using mvo = fc::mutable_variant_object;
   using namespace sysio::opp::types;
   deploy(tester, chains, sysio::testing::contracts::chains_wasm(), sysio::testing::contracts::chains_abi());
   deploy(tester, tokens, sysio::testing::contracts::tokens_wasm(), sysio::testing::contracts::tokens_abi());
   abi_serializer chains_ser, tokens_ser;
   load_account_abi(tester, chains, chains_ser);
   load_account_abi(tester, tokens, tokens_ser);
   if (tester.get_row_by_id(chains, chains, chains_table, fc::slug_name{depot_code}.value).empty()) {
      BOOST_REQUIRE_EQUAL(Tester::success(), push_contract_action_and_produce_block(
         tester, chains, chains_ser, chains, "regchain"_n,
         mvo()("kind", ChainKind::CHAIN_KIND_WIRE)("code", depot_code)("external_chain_id", 0)
            ("name", depot_code)("description", "")("outpost", no_outpost_mvo())));
   }
   if (tester.get_row_by_id(tokens, tokens, tokens_table, fc::slug_name{token_code}.value).empty()) {
      BOOST_REQUIRE_EQUAL(Tester::success(), push_contract_action_and_produce_block(
         tester, tokens, tokens_ser, tokens, "regtoken"_n,
         mvo()("kind", token_code == depot_code ? TokenKind::TOKEN_KIND_NATIVE : TokenKind::TOKEN_KIND_LIQ)
            ("code", token_code)("symbol_name", token_code)("description", "")("precision", token_precision)
            ("address", mvo()("kind", ChainKind::CHAIN_KIND_UNKNOWN)("address", ""))));
   }
   if (binding(tester, depot_code, token_code).is_null()) {
      BOOST_REQUIRE_EQUAL(Tester::success(), push_contract_action_and_produce_block(
         tester, tokens, tokens_ser, tokens, "regctok"_n,
         mvo()("chain_code", depot_code)("token_code", token_code)("contract_addr", "")
            ("is_native", token_code == depot_code)));
   }
}

/// Model the inactive rows produced by post-bootstrap registration without running
/// an unrelated epoch schedule. Only the activation fields change; keys stay intact.
template <typename Tester>
void make_inactive(Tester& tester) {
   for (const auto [account, table] : {std::pair{chains, chains_table},
                                      std::pair{tokens, tokens_table}, std::pair{tokens, bindings_table}}) {
      abi_serializer ser;
      load_account_abi(tester, account, ser);
      auto& db = const_cast<chainbase::database&>(tester.control->db());
      const auto id = compute_table_id(table.to_uint64_t());
      const auto& index = db.template get_index<kv_index, by_code_key>();
      auto it = index.lower_bound(boost::make_tuple(account, id, std::string_view{}));
      for (; it != index.end() && it->code == account && it->table_id == id; ++it) {
         std::vector<char> raw(it->value.begin(), it->value.end());
         const auto row_type = ser.get_table_type(table.to_string());
         const auto row = ser.binary_to_variant(row_type, raw,
            abi_serializer::create_yield_function(Tester::abi_serializer_max_time));
         const auto inactive = fc::mutable_variant_object(row.get_object())
            ("active", false)("activated_at_ms", uint64_t{0});
         const auto encoded = ser.variant_to_binary(row_type, inactive,
            abi_serializer::create_yield_function(Tester::abi_serializer_max_time));
         db.modify(*it, [&](auto& object) { object.value.assign(encoded.data(), encoded.size()); });
      }
   }
}

} // namespace sysio_system::test_support::registry
