#pragma once
/**
 * @file uic_signature_detail.hpp
 * @brief Shared fixed-size recoverable signature policy for underwriter UIC
 *        preflight and construction.
 *
 * The depot accepts the canonical packed K1, R1, EM, and ED variants. These
 * helpers keep the plugin's startup self-test, runtime construction guard, and
 * unit tests on that same non-parsing boundary. Variable-size WebAuthn and
 * unrecoverable BLS signatures are intentionally excluded.
 */

#include <fc/crypto/public_key.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/crypto/signature_provider.hpp>
#include <fc/io/raw.hpp>
#include <sysio/opp/uic_signature_canonical.hpp>

#include <magic_enum/magic_enum.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace sysio::underwriter_detail {

using uic_signature_type = fc::crypto::signature::sig_type;

using uic_signature_storage = fc::crypto::signature::storage_type;
static_assert(std::variant_size_v<uic_signature_storage> == 6);
static_assert(std::is_same_v<std::variant_alternative_t<0, uic_signature_storage>,
                             fc::ecc::signature_shim>);
static_assert(std::is_same_v<std::variant_alternative_t<1, uic_signature_storage>,
                             fc::crypto::r1::signature_shim>);
static_assert(std::is_same_v<std::variant_alternative_t<2, uic_signature_storage>,
                             fc::crypto::webauthn::signature>);
static_assert(std::is_same_v<std::variant_alternative_t<3, uic_signature_storage>,
                             fc::em::signature_shim>);
static_assert(std::is_same_v<std::variant_alternative_t<4, uic_signature_storage>,
                             fc::crypto::ed::signature_shim>);
static_assert(std::is_same_v<std::variant_alternative_t<5, uic_signature_storage>,
                             fc::crypto::bls::signature_shim>);

/// Return the canonical fc-packed size for one concrete signature variant.
template <typename SignatureShim>
inline std::size_t packed_uic_signature_size() {
   static const auto size = fc::raw::pack(fc::crypto::signature{
      fc::crypto::signature::storage_type{std::in_place_type<SignatureShim>}
   }).size();
   return size;
}

/// Return the canonical packed size for a depot-supported signature type.
inline std::size_t canonical_packed_uic_signature_size(uic_signature_type type) {
   switch (type) {
      case uic_signature_type::k1:
         return packed_uic_signature_size<fc::ecc::signature_shim>();
      case uic_signature_type::r1:
         return packed_uic_signature_size<fc::crypto::r1::signature_shim>();
      case uic_signature_type::em:
         return packed_uic_signature_size<fc::em::signature_shim>();
      case uic_signature_type::ed:
         return packed_uic_signature_size<fc::crypto::ed::signature_shim>();
      default:
         return 0;
   }
}

/// Domain-separated message signed by the provider during startup preflight.
inline constexpr std::string_view uic_signature_self_test_domain =
   "wire.underwriter_plugin.signature_self_test.v1";

/**
 * @brief Test whether a configured UIC provider public key is fixed-size and
 *        recoverable by the depot.
 * @param key Provider public key to classify.
 * @return True for K1, R1, EM, or ED.
 */
inline bool is_supported_uic_public_key(const fc::crypto::public_key& key) {
   using key_type = fc::crypto::public_key::key_type;
   return key.contains_type(key_type::k1, key_type::r1, key_type::em, key_type::ed);
}

/**
 * @brief Test whether a provider's UIC signature is fixed-size and recoverable.
 * @param signature Signature to classify.
 * @return True for K1, R1, EM, or ED.
 */
inline bool is_supported_uic_signature(const fc::crypto::signature& signature) {
   using sig_type = fc::crypto::signature::sig_type;
   return signature.contains_type(sig_type::k1, sig_type::r1, sig_type::em, sig_type::ed);
}

/**
 * @brief Select supported UIC signers from providers assigned to the WIRE
 *        target chain.
 *
 * K1/R1 providers use the WIRE native key format, while EM/ED providers use
 * the Ethereum/Solana native key formats. Provider selection must therefore
 * filter by the concrete public-key variant rather than by one native-format
 * `chain_key_type_t` bucket.
 *
 * Automatically generated defaults are useful for ordinary WIRE transaction
 * signing, but they must not compete with the operator's deliberate UIC key.
 * The caller supplies both the manager's operator-configuration predicate and
 * a current direct-permission-key predicate. Their intersection prevents an
 * unrelated operator-configured WIRE provider (for example, a block-signing
 * key) from competing with the underwriter account's UIC key. Both named and
 * anonymous `--signature-provider` entries express operator intent; generated
 * defaults and programmatic registrations do not.
 *
 * @param providers Providers returned by the signature-provider registry.
 * @param is_operator_configured Predicate accepting one provider key name.
 * @param is_permission_authorized Predicate accepting one provider public key.
 * @return Operator-configured WIRE-target providers whose public keys are K1,
 *         R1, EM, or ED and are direct keys of the underwriter account's
 *         current owner or active permission.
 */
template <typename IsOperatorConfigured, typename IsPermissionAuthorized>
inline std::vector<fc::crypto::signature_provider_ptr> select_uic_signature_providers(
   const std::vector<fc::crypto::signature_provider_ptr>& providers,
   IsOperatorConfigured&& is_operator_configured,
   IsPermissionAuthorized&& is_permission_authorized) {
   std::vector<fc::crypto::signature_provider_ptr> selected;
   selected.reserve(providers.size());
   for (const auto& provider : providers) {
      if (provider &&
          is_operator_configured(provider->key_name) &&
          provider->target_chain == fc::crypto::chain_kind_wire &&
          is_supported_uic_public_key(provider->public_key) &&
          is_permission_authorized(provider->public_key)) {
         selected.push_back(provider);
      }
   }
   return selected;
}

/**
 * @brief Test whether the provider key and produced signature use the same
 *        supported variant.
 * @param key Provider public key.
 * @param signature Signature returned by the provider.
 * @return True only when their shared variant tags match.
 */
inline bool uic_signature_type_matches_provider_key(
   const fc::crypto::public_key& key,
   const fc::crypto::signature& signature) {
   using key_type = fc::crypto::public_key::key_type;
   using sig_type = fc::crypto::signature::sig_type;
   static_assert(magic_enum::enum_integer(key_type::k1) ==
                 magic_enum::enum_integer(sig_type::k1));
   static_assert(magic_enum::enum_integer(key_type::r1) ==
                 magic_enum::enum_integer(sig_type::r1));
   static_assert(magic_enum::enum_integer(key_type::em) ==
                 magic_enum::enum_integer(sig_type::em));
   static_assert(magic_enum::enum_integer(key_type::ed) ==
                 magic_enum::enum_integer(sig_type::ed));
   return magic_enum::enum_integer(key.type()) ==
          magic_enum::enum_integer(signature.type());
}

/**
 * @brief Validate the depot's exact packed fixed-size UIC boundary.
 * @param packed Packed fc signature bytes.
 * @return True for canonical K1/R1/EM (66 bytes) or ED (97 bytes).
 */
inline bool is_canonical_packed_uic_signature(std::span<const char> packed) {
   if (packed.empty()) return false;
   const auto type = magic_enum::enum_cast<uic_signature_type>(
      static_cast<uint8_t>(static_cast<unsigned char>(packed.front())));
   if (!type) return false;
   const auto expected_size = canonical_packed_uic_signature_size(*type);
   return expected_size != 0 && packed.size() == expected_size;
}

/**
 * @brief Validate the recovery-byte and scalar policy for one packed UIC signature.
 * @param packed Canonically sized fc-packed signature bytes.
 * @return True for ED, or for K1/R1/EM with their canonical recovery range,
 *         nonzero in-range `r`, and low `s`.
 */
inline bool has_canonical_uic_signature_body(std::span<const char> packed) {
   if (!is_canonical_packed_uic_signature(packed)) return false;
   const auto type = magic_enum::enum_cast<uic_signature_type>(
      static_cast<uint8_t>(static_cast<unsigned char>(packed.front())));
   if (!type) return false;
   const auto body = packed.subspan(1);
   switch (*type) {
      case uic_signature_type::k1:
         return sysio::opp::is_canonical_uic_ecdsa_signature(
            body, sysio::opp::uic_ecdsa_curve::secp256k1,
            sysio::opp::uic_ecdsa_recovery_position::prefix,
            sysio::opp::uic_ecdsa_recovery_encoding::compact);
      case uic_signature_type::r1:
         return sysio::opp::is_canonical_uic_ecdsa_signature(
            body, sysio::opp::uic_ecdsa_curve::p256,
            sysio::opp::uic_ecdsa_recovery_position::prefix,
            sysio::opp::uic_ecdsa_recovery_encoding::compact);
      case uic_signature_type::em:
         return sysio::opp::is_canonical_uic_ecdsa_signature(
            body, sysio::opp::uic_ecdsa_curve::secp256k1,
            sysio::opp::uic_ecdsa_recovery_position::suffix,
            sysio::opp::uic_ecdsa_recovery_encoding::ethereum);
      case uic_signature_type::ed:
         return true;
      default:
         return false;
   }
}

/// Result of the signature-shape portion of underwriter startup preflight.
enum class uic_signature_provider_result {
   compatible,                     ///< Provider satisfies the complete fixed-shape policy.
   unsupported_public_key_type,    ///< Configured provider key is WA or BLS.
   unsupported_signature_type,     ///< Provider returned WA or BLS.
   signature_type_mismatch,        ///< Provider key and signature variants differ.
   non_canonical_packed_signature, ///< Packed result violates the depot boundary.
   non_canonical_signature,        ///< ECDSA recovery/scalar body is noncanonical.
};

/// Provider-policy result and the recovered key available on success.
struct uic_signature_provider_check {
   uic_signature_provider_result result; ///< Shape-policy classification.
   std::optional<fc::crypto::public_key> recovered_key; ///< Present only when compatible.
};

/**
 * @brief Run the signature-shape portion of underwriter startup preflight.
 *
 * Account-permission authorization remains a chain-state check in
 * `run_preflight`; this shared policy prevents its implementation and unit
 * regression from drifting apart.
 *
 * @param provider Configured WIRE provider to exercise.
 * @param digest Fixed self-test digest the provider must sign.
 * @return Shape classification and, on success, the recovered public key.
 * @throws fc exceptions propagated by provider signing, packing, or recovery.
 */
inline uic_signature_provider_check check_uic_signature_provider(
   const fc::crypto::signature_provider_t& provider,
   const fc::sha256& digest) {
   if (!is_supported_uic_public_key(provider.public_key)) {
      return {uic_signature_provider_result::unsupported_public_key_type, std::nullopt};
   }

   const auto signature = provider.sign(digest);
   if (!is_supported_uic_signature(signature)) {
      return {uic_signature_provider_result::unsupported_signature_type, std::nullopt};
   }
   if (!uic_signature_type_matches_provider_key(provider.public_key, signature)) {
      return {uic_signature_provider_result::signature_type_mismatch, std::nullopt};
   }

   const auto packed = fc::raw::pack(signature);
   if (!is_canonical_packed_uic_signature(packed)) {
      return {
         uic_signature_provider_result::non_canonical_packed_signature,
         std::nullopt,
      };
   }
   if (!has_canonical_uic_signature_body(packed)) {
      return {
         uic_signature_provider_result::non_canonical_signature,
         std::nullopt,
      };
   }

   return {
      uic_signature_provider_result::compatible,
      fc::crypto::public_key::recover(signature, digest),
   };
}

} // namespace sysio::underwriter_detail
