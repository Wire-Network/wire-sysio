#include <fc-lite/crypto/chain_types.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/common.hpp>
#include <fc/crypto/key_serdes.hpp>
#include <fc/exception/exception.hpp>
#include <fc/static_variant.hpp>

#include <variant>


namespace fc { namespace crypto {
   namespace {
      struct recovery_visitor : fc::visitor<public_key::storage_type> {
         recovery_visitor(const sha256& digest, bool check_canonical)
         :_digest(digest)
         ,_check_canonical(check_canonical)
         {}

         template<typename SignatureType>
         public_key::storage_type operator()(const SignatureType& s) const {
            return public_key::storage_type(s.recover(_digest, _check_canonical));
         }

         public_key::storage_type operator()(const bls::signature_shim& s) const {
            FC_THROW_EXCEPTION(fc::unsupported_exception, "BLS Signatures dont support recovery");
         }

         const sha256& _digest;
         bool _check_canonical;
      };

   } // anonymous namespace

   public_key::public_key( const signature& c, const sha256& digest, bool check_canonical )
   :_storage(std::visit(recovery_visitor(digest, check_canonical), c._storage))
   {
   }

   public_key public_key::from_string(const std::string& str, key_type type) {
      switch (type) {
      case key_type::k1:
      case key_type::r1:
      case key_type::wa:
      case key_type::bls: {
         public_key k(parse_unknown_wire_public_key_str(str));
         FC_ASSERT( k.type() == type, "Parsed type ${pt} does not match specified type ${t} for ${k}",
                    ("pt", k.type())("t", type)("k", str));
         return k;
      }
      case key_type::em: {
         auto [base_prefix, type_prefix, data_str] = parse_base_prefixes(str);
         const auto& key = base_prefix.empty() ? str : data_str;
         FC_ASSERT(type_prefix.empty() || type_prefix == key_prefix(key_type::em), "Invalid public key prefixes: ${k}", ("k", str));
         return from_native_string_to_public_key<chain_key_type_t::chain_key_type_ethereum>(key);
      }
      case key_type::ed: {
         auto [base_prefix, type_prefix, data_str] = parse_base_prefixes(str);
         const auto& key = base_prefix.empty() ? str : data_str;
         FC_ASSERT(type_prefix.empty() || type_prefix == key_prefix(key_type::ed), "Invalid public key prefixes: ${k}", ("k", str));
         return from_native_string_to_public_key<chain_key_type_t::chain_key_type_solana>(key);
      }
      case key_type::unknown: {
         if (is_legacy_public_key_str(str))
            return public_key(parse_unknown_wire_public_key_str(str));

         auto [base_prefix, type_prefix, data_str] = parse_base_prefixes(str);
         FC_ASSERT(base_prefix == constants::public_key_base_prefix, "Invalid prefix to parse key type: ${k}", ("k", str));
         if (type_prefix == key_prefix(key_type::em)) {
            return from_native_string_to_public_key<chain_key_type_t::chain_key_type_ethereum>(data_str);
         } else if (type_prefix == key_prefix(key_type::ed)) {
            return from_native_string_to_public_key<chain_key_type_t::chain_key_type_solana>(data_str);
         }
         return public_key(parse_unknown_wire_public_key_str(str));
      }

      default:
         FC_ASSERT(false, "Unknown key type: ${type}", ("type", type));
      };
   }

   bool public_key::valid()const {
      return std::visit([](const auto& key) { return key.valid(); }, _storage);
   }

   std::string public_key::to_string(const fc::yield_function_t& yield, bool include_prefix) const {
      switch (type()) {
      case key_type::k1:
      case key_type::r1:
      case key_type::wa: {
         auto visit = [&](int default_pos) -> std::string {
            if (default_pos == 0)
               return std::visit(base58str_visitor<storage_type, fc::crypto::constants::public_key_prefix, 0>(yield), _storage);
            return std::visit(base58str_visitor<storage_type, fc::crypto::constants::public_key_prefix, -1>(yield), _storage);
         };
         if (type() == key_type::k1 && !include_prefix) {
            return std::string(fc::crypto::constants::public_key_legacy_prefix) + visit(0);
         }
         return std::string(fc::crypto::constants::public_key_base_prefix) + "_" + visit(-1);
      }
      case key_type::em: {
         std::string prefix = include_prefix
                                 ? std::string(constants::public_key_base_prefix) + "_" + key_prefix(key_type::em) + "_"
                                 : "";
         return prefix + get<em::public_key_shim>().to_string();
      }
      case key_type::ed: {
         std::string prefix = include_prefix
                                 ? std::string(constants::public_key_base_prefix) + "_" + key_prefix(key_type::ed) + "_"
                                 : "";
         FC_THROW_EXCEPTION(fc::unsupported_exception, "Solana ED keys are not implemented yet");
      }
      case key_type::bls: {
         // bls to string includes prefix
         return get<bls::public_key_shim>().to_string();
      }
      case key_type::unknown:
         break;
      }

      FC_ASSERT(false, "public_key unknown key type ${t}", ("t", type()));
   }

   std::ostream& operator<<(std::ostream& s, const public_key& k) {
      s << "public_key(" << k.to_string({}, true) << ')';
      return s;
   }

   bool operator==( const public_key& p1, const public_key& p2) {
      return eq_comparator<public_key::storage_type>::apply(p1._storage, p2._storage);
   }

   bool operator<( const public_key& p1, const public_key& p2)
   {
      return less_comparator<public_key::storage_type>::apply(p1._storage, p2._storage);
   }
} } // fc::crypto

namespace fc
{
   using namespace std;
   void to_variant(const fc::crypto::public_key& var, fc::variant& vo, const fc::yield_function_t& yield)
   {
      vo = var.to_string(yield, true);
   }

   void from_variant(const fc::variant& var, fc::crypto::public_key& vo)
   {
      vo = fc::crypto::public_key::from_string(var.as_string());
   }
} // fc
