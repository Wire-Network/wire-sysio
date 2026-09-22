#pragma once

#include <sysio/chain/controller.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/chain/name.hpp>
#include <sysio/chain/symbol.hpp>
#include <sysio/opp/types/types.pb.h>

#include <magic_enum/magic_enum.hpp>

#include <string>
#include <string_view>
#include <vector>

/// Test support for sysio.liq's kv tables, shared by the liq and dispatch suites.
namespace sysio_liq::test_support {

/// The kv key of a `sysio.liq::parked` row: the symbol code and the chain kind as
/// big-endian words, then the pubkey NUL-escaped and NUL-NUL terminated (the kv key
/// encoding of the contract's `parked_key`).
inline std::string parked_key(sysio::chain::symbol_code sym, sysio::opp::types::ChainKind kind,
                              const std::vector<char>& pubkey) {
   std::string key;
   for (uint64_t word : { sym.value, static_cast<uint64_t>(magic_enum::enum_integer(kind)) })
      for (int shift = 56; shift >= 0; shift -= 8) key.push_back(char((word >> shift) & 0xff));
   for (char c : pubkey) {
      key.push_back(c);
      if (c == '\0') key.push_back('\x01');
   }
   key.push_back('\0');
   key.push_back('\0');
   return key;
}

/// The serialized `parked` row stored under `key` by `liq_account`, empty when absent.
inline std::vector<char> parked_row_bytes(const sysio::chain::controller& control, sysio::chain::name liq_account,
                                          const std::string& key) {
   const auto& kv_idx = control.db().get_index<sysio::chain::kv_index, sysio::chain::by_code_key>();
   const auto  itr    = kv_idx.find(boost::make_tuple(
      liq_account, sysio::chain::compute_table_id(sysio::chain::name{ "parked" }.to_uint64_t()), std::string_view(key)));
   if (itr == kv_idx.end()) return {};
   return std::vector<char>(itr->value.data(), itr->value.data() + itr->value.size());
}

} // namespace sysio_liq::test_support
