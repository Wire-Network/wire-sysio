#pragma once

#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/block.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/trace.hpp>

#include <fc/time.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace sysio::chain_apis {

/**
 * Copies of the packed ABIs of the accounts a block or a transaction trace names. They are taken where chain state may
 * be read, so that serializers for them can be looked up and built on a thread that may not read it.
 *
 * Captures share copies: while any capture holds a copy of an account's ABI, a later capture of that account whose ABI
 * is still byte for byte the same takes that copy rather than another. Copying a captured_abis is cheap.
 */
class captured_abis {
public:
   /// A packed ABI, shared by the captures that hold it.
   using packed_abi_ptr = std::shared_ptr<const std::vector<char>>;

   /// Captures the ABI of each account an action of `block` names, context-free actions included.
   static captured_abis capture(const chain::controller& chain, const chain::signed_block& block);

   /// Captures the ABI of each account an action trace of `trace` names.
   static captured_abis capture(const chain::controller& chain, const chain::transaction_trace& trace);

   /// The ABI captured for `account`; null when `account` was not captured or has no ABI.
   const packed_abi_ptr& find(const chain::account_name& account) const;

   /// Accounts captured, those with no ABI included.
   size_t size() const { return _abis.size(); }

private:
   /// Captures `account` unless it already is.
   void add(const chain::controller& chain, const chain::account_name& account);

   std::unordered_map<chain::account_name, packed_abi_ptr> _abis; ///< each account captured, null without an ABI
};

/**
 * Serializers for captured ABIs, for decoding on a thread that may not read chain state. Each is looked up in
 * shared_abi_serializer_cache() on first use and built there on a miss; a build may take `max_build_time`, cut short at
 * `deadline`. Each serializer is remembered by the copy it was looked up for, so a copy several captures share is
 * hashed and looked up once. Use from one thread at a time.
 */
class captured_abi_serializers {
public:
   /// Serializers whose builds may take `max_build_time`, cut short at `deadline`.
   captured_abi_serializers(const fc::microseconds& max_build_time, const fc::time_point& deadline);
   captured_abi_serializers(const captured_abi_serializers&)            = delete;
   captured_abi_serializers& operator=(const captured_abi_serializers&) = delete;

   /// The serializer for `abi`; null when `abi` is null, or its serializer failed to build or was not built in time.
   const chain::abi_serializer* find(const captured_abis::packed_abi_ptr& abi);

   /**
    * A resolver, for abi_serializer::to_variant, from each account `abis` holds to the serializer for its captured ABI.
    * It refers to this and to `abis`, which must outlive it.
    */
   auto resolver_for(const captured_abis& abis) {
      return [this, &abis](const chain::account_name& account)
                -> std::optional<std::reference_wrapper<const chain::abi_serializer>> {
         if (const chain::abi_serializer* serializer = find(abis.find(account)))
            return std::cref(*serializer);
         return std::nullopt;
      };
   }

private:
   const fc::microseconds _max_build_time; ///< time one build may take
   const fc::time_point   _deadline;       ///< builds end here at the latest
   /// The serializer had for each copy looked up, null when it could not be had.
   std::unordered_map<captured_abis::packed_abi_ptr, chain::abi_serializer_ptr> _found;
};

} // namespace sysio::chain_apis
