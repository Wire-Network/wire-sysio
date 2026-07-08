/**
 * Unit tests for the runtime-dispatch helpers in fc/crypto/signature_provider.hpp:
 * `from_native_string_to_private_key(chain_key_type_t, ...)`,
 * `from_native_string_to_public_key(chain_key_type_t, ...)`, and
 * `make_local_sign_fn`. These are the pieces shared by the signature-provider
 * plugin's KEY: path and extension providers (e.g. the ssm sub-library) that
 * construct local-key signers; provider-level suites cover them transitively,
 * this file covers them directly.
 */

#include <boost/test/unit_test.hpp>

#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/signature_provider.hpp>
#include <fc/exception/exception.hpp>

#include <array>
#include <string>
#include <utility>

using namespace fc::crypto;

namespace {

/// The chain key types with an implemented native string form, paired with a
/// generator for a key of that type's underlying shim.
const std::array<std::pair<chain_key_type_t, private_key::key_type>, 4> supported_types{{
   {chain_key_type_wire, private_key::key_type::k1},
   {chain_key_type_wire_bls, private_key::key_type::bls},
   {chain_key_type_ethereum, private_key::key_type::em},
   {chain_key_type_solana, private_key::key_type::ed},
}};

} // namespace

BOOST_AUTO_TEST_SUITE(signature_provider_helpers)

BOOST_AUTO_TEST_CASE(private_key_runtime_dispatch_round_trips_all_supported_types) {
   for (const auto& [chain_key_type, shim_type] : supported_types) {
      const auto generated = private_key::generate(shim_type);
      const auto parsed    = from_native_string_to_private_key(chain_key_type, generated.to_string({}));
      // Same key material back: the derived public keys must match.
      BOOST_CHECK_MESSAGE(parsed.get_public_key() == generated.get_public_key(),
                          "round trip changed the key for chain key type "
                             << chain_key_type_reflector::to_string(chain_key_type));
   }
}

BOOST_AUTO_TEST_CASE(public_key_runtime_dispatch_round_trips_all_supported_types) {
   for (const auto& [chain_key_type, shim_type] : supported_types) {
      const auto pub    = private_key::generate(shim_type).get_public_key();
      const auto parsed = from_native_string_to_public_key(chain_key_type, pub.to_string({}));
      BOOST_CHECK_MESSAGE(parsed == pub, "round trip changed the public key for chain key type "
                                            << chain_key_type_reflector::to_string(chain_key_type));
   }
}

BOOST_AUTO_TEST_CASE(runtime_dispatch_rejects_types_without_a_native_form) {
   // sui has no implemented native form; unknown is not a real key type. Both
   // land in the dispatcher's unsupported arm. Callers owning a richer error
   // taxonomy (the signature-provider plugin) pre-check these; the fc-level
   // contract is fc::unsupported_exception.
   BOOST_CHECK_THROW(from_native_string_to_private_key(chain_key_type_sui, "anything"),
                     fc::unsupported_exception);
   BOOST_CHECK_THROW(from_native_string_to_private_key(chain_key_type_unknown, "anything"),
                     fc::unsupported_exception);
   BOOST_CHECK_THROW(from_native_string_to_public_key(chain_key_type_sui, "anything"),
                     fc::unsupported_exception);
   BOOST_CHECK_THROW(from_native_string_to_public_key(chain_key_type_unknown, "anything"),
                     fc::unsupported_exception);
}

BOOST_AUTO_TEST_CASE(runtime_dispatch_propagates_parse_failures) {
   BOOST_CHECK_THROW(from_native_string_to_private_key(chain_key_type_wire, "not-a-key"), fc::exception);
   BOOST_CHECK_THROW(from_native_string_to_private_key(chain_key_type_wire, ""), fc::exception);
}

BOOST_AUTO_TEST_CASE(make_local_sign_fn_signs_with_the_captured_key) {
   const auto digest = fc::sha256::hash(std::string{"fc signature provider helpers"});
   for (const auto& [chain_key_type, shim_type] : supported_types) {
      const auto key    = private_key::generate(shim_type);
      const auto signer = make_local_sign_fn(key);
      BOOST_REQUIRE(static_cast<bool>(signer));
      // fc signing is deterministic for every key type here, so the closure's
      // output must equal signing with the key directly.
      BOOST_CHECK_MESSAGE(signer(digest) == key.sign(digest),
                          "closure signature diverged for chain key type "
                             << chain_key_type_reflector::to_string(chain_key_type));
   }
}

BOOST_AUTO_TEST_SUITE_END()
