#include <fc/crypto/hex.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/common.hpp>
#include <fc/exception/exception.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <ranges>
#include <fc/crypto/key_serdes.hpp>

namespace fc { namespace crypto {
   namespace {


   }
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
         FC_THROW_EXCEPTION(fc::exception, "BLS Signatures dont support recovery");
         //return public_key::storage_type(s.recover(_digest, _check_canonical));
      }

      const sha256& _digest;
      bool _check_canonical;
   };

   public_key::public_key( const signature& c, const sha256& digest, bool check_canonical )
   :_storage(std::visit(recovery_visitor(digest, check_canonical), c._storage))
   {
   }

   size_t public_key::which() const {
      return _storage.index();
   }

   public_key::storage_type public_key::parse_base58(const std::string& base58str)
   {
      constexpr auto legacy_prefix = fc::crypto::constants::public_key_legacy_prefix;
      if(prefix_matches(legacy_prefix, base58str) && base58str.find('_') == std::string::npos ) {
         auto sub_str = base58str.substr(const_strlen(legacy_prefix));
         using default_type = typename std::variant_alternative_t<0, public_key::storage_type>; //public_key::storage_type::template type_at<0>;
         using data_type = default_type::data_type;
         using wrapper = checksum_data<data_type>;
         auto bin = fc::from_base58(sub_str);
         FC_ASSERT(bin.size() == sizeof(data_type) + sizeof(uint32_t), "");
         auto wrapped = fc::raw::unpack<wrapper>(bin);
         FC_ASSERT(wrapper::calculate_checksum(wrapped.data) == wrapped.check);
         return public_key::storage_type(default_type(wrapped.data));
      } else {
         constexpr auto prefix = fc::crypto::constants::public_key_base_prefix;

         const auto pivot = base58str.find('_');
         FC_ASSERT(pivot != std::string::npos, "No delimiter in string, cannot determine data type: ${str}", ("str", base58str));

         const auto prefix_str = base58str.substr(0, pivot);
         FC_ASSERT(prefix == prefix_str, "Public Key has invalid prefix: ${str}", ("str", base58str)("prefix_str", prefix_str));

         auto data_str = base58str.substr(pivot + 1);
         FC_ASSERT(!data_str.empty(), "Public Key has no data: ${str}", ("str", base58str));
         return base58_str_parser<public_key::storage_type, fc::crypto::constants::public_key_prefix>::apply(data_str);
      }
   }

   public_key::public_key(const std::string& base58str)
   :_storage(parse_base58(base58str))
   {}

   struct is_valid_visitor : public fc::visitor<bool> {
      template< typename KeyType >
      bool operator()( const KeyType& key )const {
         return key.valid();
      }
   };

   bool public_key::valid()const
   {
      return std::visit(is_valid_visitor(), _storage);
   }

   std::string public_key::to_string(const fc::yield_function_t& yield) const
   {
      auto which = _storage.index();
      std::string data_str;
      // if (which == 3) {
      //
      // } else {
      //    data_str = std::visit(base58str_visitor<storage_type, fc::crypto::constants::public_key_prefix, 0>(yield), _storage);
      // }
      data_str = std::visit(base58str_visitor<storage_type, fc::crypto::constants::public_key_prefix, 0>(yield), _storage);
      if (which == 0) {
         return std::string(fc::crypto::constants::public_key_legacy_prefix) + data_str;
      } else {
         return std::string(fc::crypto::constants::public_key_base_prefix) + "_" + data_str;
      }
   }

   std::string public_key::to_native_string(const fc::yield_function_t& yield) const {
      return std::visit(
         to_native_string_from_public_key_visitor<storage_type, fc::crypto::constants::public_key_prefix,0>(yield),
         _storage);
   }
   chain_key_type_t get_public_key_type(const std::variant<std::string, public_key>& pub_key_var) {
      if (std::holds_alternative<public_key>(pub_key_var)) {
         auto pub_key = std::get<public_key>(pub_key_var);
         if (!pub_key.valid())
            return chain_key_type_unknown;

         return get_public_key_type(pub_key.to_string({}));
      }

      auto pub_key_str = std::get<std::string>(pub_key_var);
      auto pub_key_len = pub_key_str.length();
      public_key pub_key(pub_key_str);

      if (!pub_key.valid())
         return chain_key_type_unknown;

      for (auto& prefix : fc::crypto::constants::public_key_wire_prefixes) {
         if (pub_key_str.starts_with(prefix) && pub_key.which() <= 1)
            return chain_key_type_wire;
      }

      if (pub_key_str.starts_with("0x") || pub_key.which() == 0) {
         return chain_key_type_ethereum;
      }

      if ((pub_key_len == 43 || pub_key_len == 44) && pub_key.which() == 4) {
         return chain_key_type_solana;
      }

      return chain_key_type_unknown;





   }

   std::ostream& operator<<(std::ostream& s, const public_key& k) {
      s << "public_key(" << k.to_string({}) << ')';
      return s;
   }

   bool operator==( const public_key& p1, const public_key& p2) {
      return eq_comparator<public_key::storage_type>::apply(p1._storage, p2._storage);
   }

   bool operator!=( const public_key& p1, const public_key& p2) {
      return !(p1 == p2);
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
      vo = var.to_string(yield);
   }

   void from_variant(const fc::variant& var, fc::crypto::public_key& vo)
   {
      vo = fc::crypto::public_key(var.as_string());
   }
} // fc
