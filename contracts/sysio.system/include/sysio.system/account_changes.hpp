#pragma once

#include <sysio/action.hpp>
#include <sysio/crypto.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/name.hpp>
#include <sysio/system.hpp>
#include <sysio/time.hpp>

#include <vector>

/// Tables on the `sysio` account recording when an account's ABI or code last changed. `sysio.bios` and
/// `sysio.system` both write them from their `setabi` and `setcode` handlers, so, apart from the `sysio` case below, a
/// chain has a row for every change since genesis. Rows are overwritten, never erased. Each contract aliases the tables
/// inside its class, which puts them in its ABI.
///
/// The rows record what the contract on `sysio` sees. A deploy to `sysio` itself is recorded by the incoming code,
/// since the native `setcode` runs first, so code that does not record (or clearing `sysio`'s code) leaves `sysio`'s
/// row stale and stops every later record. Only `sysio`'s own authority can do that, and it could rewrite these rows
/// anyway; the chain's code hash (`get_code_hash`) is the record nothing can bypass.
namespace sysiosystem {

   using sysio::checksum256;
   using sysio::name;
   using sysio::time_point;

   struct abihash_key {
      uint64_t owner;
      SYSLIB_SERIALIZE(abihash_key, (owner))
   };

   /// The hash of an account's ABI and when it was last set.
   struct [[sysio::table("abihash")]] abi_hash {
      name        owner;      ///< the account the ABI belongs to
      checksum256 hash;       ///< sha256 of the packed ABI `setabi` received
      time_point  updated_at; ///< block time of the last `setabi`
      uint32_t    block_num;  ///< block of the last `setabi`

      SYSLIB_SERIALIZE(abi_hash, (owner)(hash)(updated_at)(block_num))
   };

   using abi_hash_table = sysio::kv::table<"abihash"_n, abihash_key, abi_hash>;

   struct codehash_key {
      uint64_t owner;
      SYSLIB_SERIALIZE(codehash_key, (owner))
   };

   /// The hash of an account's code and when it was last set.
   struct [[sysio::table("codehash")]] code_hash {
      name        owner;      ///< the account the code belongs to
      checksum256 hash;       ///< the chain's code hash: sha256 of the wasm, all zero once cleared (not for `sysio`)
      time_point  updated_at; ///< block time of the last `setcode`
      uint32_t    block_num;  ///< block of the last `setcode`

      SYSLIB_SERIALIZE(code_hash, (owner)(hash)(updated_at)(block_num))
   };

   using code_hash_table = sysio::kv::table<"codehash"_n, codehash_key, code_hash>;

   /// Record `setabi` of `abi` on `account`; the row is billed to `account`, which authorized the action.
   inline void record_abi(name self, name account, const std::vector<char>& abi) {
      const auto hash = sysio::sha256(abi.data(), abi.size());
      abi_hash_table(self).set(account, abihash_key{account.value},
                               abi_hash{account, hash, sysio::current_time_point(), sysio::current_block_number()});
   }

   /// Record `setcode` on `account`; the row is billed to `account`, which authorized the action. The chain applies
   /// `setcode` before the contract's handler runs, so the hash comes from the chain and the handler never needs the
   /// wasm itself.
   inline void record_code(name self, name account) {
      code_hash_table(self).set(account, codehash_key{account.value},
                                code_hash{account, sysio::get_code_hash(account), sysio::current_time_point(),
                                          sysio::current_block_number()});
   }

} // namespace sysiosystem
