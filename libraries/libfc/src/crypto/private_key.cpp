#include <fc/crypto/private_key.hpp>
#include <fc/utility.hpp>
#include <fc/crypto/key_serdes.hpp>
#include <fc/exception/exception.hpp>

namespace fc { namespace crypto {
   using namespace std;

   public_key private_key::get_public_key() const {
      return public_key(std::visit([](const auto& key) {
         return public_key::storage_type(key.get_public_key());
      }, _storage));
   }

   signature private_key::sign( const sha256& digest, bool require_canonical ) const {
      return signature(std::visit([&digest, require_canonical](const auto& key) {
         return signature::storage_type(key.sign(digest, require_canonical));
      }, _storage));
   }

   sha512 private_key::generate_shared_secret( const public_key& pub ) const {
      return std::visit([&_pub_storage = pub._storage]<typename T>(const T& key) {
         return key.generate_shared_secret(std::get<typename T::public_key_type>(_pub_storage));
      }, _storage);
   }

   private_key private_key::from_string(const std::string& str, key_type type) {
      switch (type) {
      case key_type::k1:
      case key_type::r1:
      case key_type::bls: {
         private_key k(parse_unknown_wire_private_key_str(str));
         FC_ASSERT( k.type() == type, "Parsed type ${pt} does not match specified type ${t} for ${k}",
                    ("pt", k.type())("t", type)("k", str));
         return k;
      }
      case key_type::em: {
         auto [base_prefix, type_prefix, data_str] = parse_base_prefixes(str);
         const auto& key = base_prefix.empty() ? str : data_str;
         FC_ASSERT(type_prefix.empty() || type_prefix == key_prefix(key_type::em), "Invalid private key prefixes: ${k}", ("k", str));
         return from_native_string_to_private_key<chain_key_type_t::chain_key_type_ethereum>(key);
      }
      case key_type::ed: {
         auto [base_prefix, type_prefix, data_str] = parse_base_prefixes(str);
         const auto& key = base_prefix.empty() ? str : data_str;
         FC_ASSERT(type_prefix.empty() || type_prefix == key_prefix(key_type::ed), "Invalid private key prefixes: ${k}", ("k", str));
         return from_native_string_to_private_key<chain_key_type_t::chain_key_type_solana>(key);
      }
      case key_type::unknown: {
         if (str.find('_') == std::string::npos)
            return private_key(parse_unknown_wire_private_key_str(str));

         auto [base_prefix, type_prefix, data_str] = parse_base_prefixes(str);
         FC_ASSERT(base_prefix == constants::private_key_base_prefix, "Invalid prefix to parse key type: ${k}", ("k", str));
         if (type_prefix == key_prefix(key_type::em)) {
            return from_native_string_to_private_key<chain_key_type_t::chain_key_type_ethereum>(data_str);
         } else if (type_prefix == key_prefix(key_type::ed)) {
            return from_native_string_to_private_key<chain_key_type_t::chain_key_type_solana>(data_str);
         }
         return private_key(parse_unknown_wire_private_key_str(str));
      }

      default:
         FC_ASSERT(false, "Unknown key type: ${type}", ("type", type));
      };

   }

   std::string private_key::to_string(const fc::yield_function_t& yield, bool include_prefix) const {
      if (type() == key_type::k1 && !include_prefix) {
         return to_wif(std::get<0>(_storage), yield);
      }

      switch (type()) {
      case key_type::k1:
      case key_type::r1: {
         auto data_str = std::visit(base58str_visitor<storage_type, fc::crypto::constants::private_key_prefix>(yield), _storage);
         return std::string(fc::crypto::constants::private_key_base_prefix) + "_" + data_str;
      }
      case key_type::em: {
         std::string prefix = include_prefix
                                 ? std::string(constants::private_key_base_prefix) + "_" + key_prefix(key_type::em) + "_"
                                 : "";
         return prefix + get<em::private_key_shim>().to_string();
      }
      case key_type::ed: {
         std::string prefix = include_prefix
                                 ? std::string(constants::private_key_base_prefix) + "_" + key_prefix(key_type::ed) + "_"
                                 : "";
         FC_THROW_EXCEPTION(fc::unsupported_exception, "Solana ED keys are not implemented yet");
      }
      case key_type::bls: {
         return std::get<bls::private_key_shim>(_storage).to_string();
      }
      case key_type::unknown:
         break;
      }

      FC_ASSERT(false, "private_key unknown key type ${t}", ("t", type()));
   }

   bool operator==( const private_key& p1, const private_key& p2 ) {
      return eq_comparator<private_key::storage_type>::apply(p1._storage, p2._storage);
   }

   bool operator<( const private_key& p1, const private_key& p2 )
   {
      return less_comparator<private_key::storage_type>::apply(p1._storage, p2._storage);
   }
} } // fc::crypto

namespace fc {
void to_variant(const fc::crypto::private_key& var, variant& vo, const fc::yield_function_t& yield) {
   vo = var.to_string(yield, true);
}

void from_variant(const variant& var, fc::crypto::private_key& vo) {
   vo = fc::crypto::private_key::from_string(var.as_string());
}

} // fc
