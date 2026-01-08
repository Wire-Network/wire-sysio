#include <fc/crypto/signature.hpp>
#include <fc/crypto/common.hpp>
#include <fc/crypto/key_serdes.hpp>
#include <fc/exception/exception.hpp>

namespace fc { namespace crypto {
   struct hash_visitor : public fc::visitor<size_t> {
      template<typename SigType>
      size_t operator()(const SigType& sig) const {
         static_assert(sizeof(sig._data) == 65, "sig size is expected to be 65");
         //signatures are two bignums: r & s. Just add up least significant digits of the two
         return *(size_t*)&sig._data[32-sizeof(size_t)] + *(size_t*)&sig._data[64-sizeof(size_t)];
      }

      size_t operator()(const bls::signature_shim& sig) const {
         return sig.unwrapped().get_hash();
      }

      size_t operator()(const webauthn::signature& sig) const {
         return sig.get_hash();
      }
   };

   signature::storage_type signature::sig_parse_base58(const std::string& base58str)
   { try {
      constexpr auto prefix = fc::crypto::constants::signature_base_prefix;

      const auto pivot = base58str.find('_');
      FC_ASSERT(pivot != std::string::npos, "No delimiter in string, cannot determine type: ${str}", ("str", base58str));

      const auto prefix_str = base58str.substr(0, pivot);
      FC_ASSERT(prefix == prefix_str, "Signature Key has invalid prefix: ${str}", ("str", base58str)("prefix_str", prefix_str));

      auto data_str = base58str.substr(pivot + 1);
      FC_ASSERT(!data_str.empty(), "Signature has no data: ${str}", ("str", base58str));
      return base58_str_parser<signature::storage_type, fc::crypto::constants::signature_prefix>::apply(data_str);
   } FC_RETHROW_EXCEPTIONS( warn, "error parsing signature", ("str", base58str ) ) }

   signature::signature(const std::string& base58str)
      :_storage(sig_parse_base58(base58str))
   {}

   size_t signature::which() const {
      return _storage.index();
   }

   template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
   template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

   size_t signature::variable_size() const {
      return std::visit(overloaded {
         [&](const auto& k1r1) {
            return static_cast<size_t>(0);
         },
         [&](const webauthn::signature& wa) {
            return static_cast<size_t>(wa.variable_size());
         }
      }, _storage);
   }

   std::string signature::to_native_string(const fc::yield_function_t& yield) const {
      return std::visit(to_native_string_from_signature_visitor<storage_type, fc::crypto::constants::signature_prefix>(yield), _storage);
   }

   std::string signature::to_string(const fc::yield_function_t& yield) const
   {
      auto data_str = std::visit(base58str_visitor<storage_type, fc::crypto::constants::signature_prefix>(yield), _storage);
      yield();
      return std::string(fc::crypto::constants::signature_base_prefix) + "_" + data_str;
   }

   std::ostream& operator<<(std::ostream& s, const signature& k) {
      s << "signature(" << k.to_string() << ')';
      return s;
   }

   bool operator == ( const signature& p1, const signature& p2) {
      return eq_comparator<signature::storage_type>::apply(p1._storage, p2._storage);
   }

   bool operator != ( const signature& p1, const signature& p2) {
      return !eq_comparator<signature::storage_type>::apply(p1._storage, p2._storage);
   }

   bool operator < ( const signature& p1, const signature& p2)
   {
      return less_comparator<signature::storage_type>::apply(p1._storage, p2._storage);
   }

   size_t hash_value(const signature& b) {
       return std::visit(hash_visitor(), b._storage);
   }
} } // sysio::blockchain

namespace fc
{
   void to_variant(const fc::crypto::signature& var, fc::variant& vo, const fc::yield_function_t& yield)
   {
      vo = var.to_string(yield);
   }

   void from_variant(const fc::variant& var, fc::crypto::signature& vo)
   {
      vo = fc::crypto::signature(var.as_string());
   }
} // fc
