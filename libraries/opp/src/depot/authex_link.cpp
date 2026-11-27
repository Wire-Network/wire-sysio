#include <sysio/opp/depot/authex_link.hpp>

#include <fc/crypto/base58.hpp>
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/exception/exception.hpp>

#include <magic_enum/magic_enum.hpp>

#include <array>
#include <span>

namespace sysio::opp::depot::authex_link {

namespace {

using key_type = fc::crypto::public_key::key_type;

/// Refuse a key createlink cannot link, naming its type.
[[noreturn]] void throw_unsupported_key(const fc::crypto::public_key& key) {
   FC_THROW_EXCEPTION(fc::invalid_arg_exception,
                      "createlink links only an EM (Ethereum) or ED (Solana) key, not a {} key",
                      std::string{magic_enum::enum_name(key.type())});
}

} // namespace

types::ChainKind chain_kind_for(const fc::crypto::public_key& key) {
   switch (key.type()) {
      case key_type::em: return types::CHAIN_KIND_EVM;
      case key_type::ed: return types::CHAIN_KIND_SVM;
      default:           throw_unsupported_key(key);
   }
}

std::string message_public_key(const fc::crypto::public_key& key) {
   switch (key.type()) {
      case key_type::em: {
         const auto compressed = key.get<fc::em::public_key_shim>().serialize();
         return std::string{em_key_prefix} +
                fc::to_hex(reinterpret_cast<const char*>(compressed.data()), compressed.size());
      }
      case key_type::ed: {
         const auto raw = key.get<fc::crypto::ed::public_key_shim>().serialize();
         return std::string{ed_key_prefix} +
                fc::to_base58(reinterpret_cast<const char*>(raw.data()), raw.size(), fc::yield_function_t{});
      }
      default: throw_unsupported_key(key);
   }
}

std::string build_message(const fc::crypto::public_key& key, std::string_view account, types::ChainKind chain_kind,
                          uint64_t nonce) {
   std::string message = message_public_key(key);
   message += field_separator;
   message += account;
   message += field_separator;
   message += std::to_string(magic_enum::enum_integer(chain_kind));
   message += field_separator;
   message += std::to_string(nonce);
   message += field_separator;
   message += auth_suffix;
   return message;
}

fc::sha256 signing_digest(std::string_view message, const fc::crypto::public_key& key) {
   switch (key.type()) {
      case key_type::em: {
         const auto hash = fc::crypto::keccak256::hash(
            std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(message.data()), message.size()});
         return fc::sha256{reinterpret_cast<const char*>(hash.data()), hash.data_size()};
      }
      case key_type::ed: {
         const auto hash = fc::sha256::hash(message.data(), static_cast<uint32_t>(message.size()));
         std::array<char, fc::sha256::byte_size> mapped{};
         for (size_t i = 0; i < mapped.size(); ++i) {
            const auto byte = static_cast<unsigned char>(hash.data()[i]);
            mapped[i]       = static_cast<char>(byte % ed_digest_printable_range + ed_digest_printable_min);
         }
         return fc::sha256{mapped.data(), mapped.size()};
      }
      default: throw_unsupported_key(key);
   }
}

fc::crypto::signature sign(const fc::crypto::private_key& key, std::string_view account, types::ChainKind chain_kind,
                           uint64_t nonce) {
   const auto public_key = key.get_public_key();
   return key.sign(signing_digest(build_message(public_key, account, chain_kind, nonce), public_key));
}

fc::crypto::signature sign(const fc::crypto::private_key& key, std::string_view account, uint64_t nonce) {
   return sign(key, account, chain_kind_for(key.get_public_key()), nonce);
}

} // namespace sysio::opp::depot::authex_link
