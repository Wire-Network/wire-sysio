#pragma once
#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>

namespace fc::test {
using namespace fc::crypto;

constexpr auto keygen_ethereum_script = "keygen_ethereum.py";
constexpr auto keygen_solana_script   = "keygen_solana.py";
constexpr auto keygen_wire_script   = "keygen_wire.py";

constexpr auto keygen_ethereum_name = "ethereum";
constexpr auto keygen_solana_name   = "solana";
constexpr auto keygen_wire_name   = "wire";

constexpr std::array<std::string_view, 3> keygen_scripts = {keygen_ethereum_script, keygen_solana_script, keygen_wire_script};
constexpr std::array<std::string_view, 3> keygen_names = {keygen_ethereum_name, keygen_solana_name, keygen_wire_name};

constexpr bool is_keygen_name(const std::string_view& name) {
   return std::ranges::find(keygen_names, name) != keygen_names.end();
}

constexpr bool is_keygen_script(const std::string_view& script) {
   return std::ranges::find(keygen_scripts, script) != keygen_scripts.end();
}

struct keygen_result {
   std::string                  key_name;
   /** ONLY APPLICABLE TO WIRE, `address` is the name encoded version of `account_name` */
   std::string                  account_name;
   fc::crypto::chain_kind_t     chain_type;
   fc::crypto::chain_key_type_t chain_key_type;
   /** ONLY APPLICABLE TO WIRE, genesis `chain_id` (relevant to the signature?) */
   std::string                  chain_id;
   std::string                  public_key;
   std::string                  private_key;
   std::string                  address;
   std::string                  signature;
   std::string                  payload;
};

keygen_result generate_external_test_key_and_sig(const std::string& keygen_script);

keygen_result load_keygen_fixture(const std::string& keygen_name, std::uint32_t id);

std::string to_private_key_spec(const std::string& priv);

std::string keygen_fixture_to_spec(const std::string& keygen_name, std::uint32_t id);

std::vector<std::string> keygen_fixtures_to_specs(const std::string& keygen_name);
}

FC_REFLECT(fc::test::keygen_result, (key_name)(account_name)(chain_type)(chain_key_type)(public_key)(private_key)(address)(signature)(payload))