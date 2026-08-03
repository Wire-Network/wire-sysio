#pragma once

/// Shared BLS finalizer-key test vectors and helpers for reading the finalizer
/// policy that sysio.system proposes. Used by both sysio.finalizer_key_tests.cpp
/// and sysio.producer_eligibility_tests.cpp so the 23 deterministic keypairs and
/// the `lastpropfins` decode logic live in exactly one place.

#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/kv_table_objects.hpp>

#include <fc/reflect/reflect.hpp>
#include <fc/variant.hpp>

#include <string>
#include <unordered_set>
#include <vector>

namespace sysio_test {

/// A BLS finalizer public key paired with its proof-of-possession, as consumed
/// by sysio.system::regfinkey.
struct key_pair_t {
   std::string pub_key;
   std::string pop;
};

/// Mirror of the finalizer authority as serialized inside the system contract's
/// last-proposed-finalizers row. Used only to unpack that row in tests.
struct finalizer_authority_t {
   std::string       description;
   uint64_t          weight = 0;
   std::vector<char> public_key;
};

/// One (key_id, authority) entry of the last proposed finalizer policy.
struct finalizer_auth_info {
   uint64_t              key_id;
   finalizer_authority_t fin_authority;
};

/// Decoded `lastpropfins` kv::global singleton row.
struct last_prop_finalizers_info {
   std::vector<finalizer_auth_info> last_proposed_finalizers;
};

} // namespace sysio_test

FC_REFLECT(sysio_test::finalizer_authority_t, (description)(weight)(public_key))
FC_REFLECT(sysio_test::finalizer_auth_info, (key_id)(fin_authority))
FC_REFLECT(sysio_test::last_prop_finalizers_info, (last_proposed_finalizers))

namespace sysio_test {

/// 23 deterministic BLS keypairs. Defined once in sysio.finalizer_key_tests.cpp.
extern const std::vector<key_pair_t> key_pairs;

/// Reads sysio.system's `lastpropfins` kv::global singleton -- the finalizer
/// policy most recently proposed by update_ranked_producers -- and returns its
/// entries. Empty when the table has never been written. Because
/// update_ranked_producers builds the producer schedule and this finalizer
/// policy in the same loop, the set of key_ids here reflects exactly which
/// producers were scheduled.
///
/// @param t       any tester whose controller hosts the system contract.
/// @param sys_abi the system contract's ABI serializer.
inline std::vector<finalizer_auth_info>
get_last_prop_finalizers(sysio::testing::base_tester& t, sysio::chain::abi_serializer& sys_abi) {
   using namespace sysio::chain;
   char key_buf[kv_pri_key_size];
   kv_encode_be64(key_buf, "lastpropfins"_n.to_uint64_t());
   std::string_view key_sv(key_buf, kv_pri_key_size);
   const auto& kv_idx = t.control->db().get_index<kv_index, by_code_key>();
   auto it = kv_idx.find(boost::make_tuple(config::system_account_name,
                                           compute_table_id("lastpropfins"_n.to_uint64_t()), key_sv));
   if (it == kv_idx.end()) {
      return {};
   }
   std::vector<char> data(it->value.data(), it->value.data() + it->value.size());
   if (data.empty()) {
      return {};
   }
   fc::variant fins_info = sys_abi.binary_to_variant(
      "last_prop_finalizers_info", data,
      abi_serializer::create_yield_function(sysio::testing::base_tester::abi_serializer_max_time));
   return fins_info["last_proposed_finalizers"].as<std::vector<finalizer_auth_info>>();
}

/// The set of finalizer key_ids in the last proposed finalizer policy.
inline std::unordered_set<uint64_t>
get_last_prop_fin_ids(sysio::testing::base_tester& t, sysio::chain::abi_serializer& sys_abi) {
   std::unordered_set<uint64_t> ids;
   for (const auto& f : get_last_prop_finalizers(t, sys_abi)) {
      ids.insert(f.key_id);
   }
   return ids;
}

} // namespace sysio_test
