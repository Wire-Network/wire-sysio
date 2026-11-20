#include <sysio/chain_plugin/captured_abis.hpp>

#include <sysio/chain/account_object.hpp>
#include <sysio/chain_plugin/abi_serializer_cache.hpp>

#include <algorithm>
#include <mutex>
#include <span>

namespace sysio::chain_apis {

using chain::abi_serializer;

namespace {

/**
 * The copy of each account's packed ABI that captures took last, held weakly, so that captures of an ABI that has not
 * changed share one copy while any of them holds it. Safe to call from any thread.
 */
class shared_abi_copies {
public:
   /// A copy of `current`, the packed ABI `account` has now: the copy captures hold when its bytes are the same.
   captured_abis::packed_abi_ptr copy_of(const chain::account_name& account, std::span<const char> current) {
      captured_abis::packed_abi_ptr held;
      {
         std::lock_guard lock(_mtx);
         if (const auto itr = _copies.find(account); itr != _copies.end())
            held = itr->second.lock();
      }
      if (held && std::ranges::equal(*held, current))
         return held;

      captured_abis::packed_abi_ptr fresh = std::make_shared<const std::vector<char>>(current.begin(), current.end());
      std::lock_guard lock(_mtx);
      _copies.insert_or_assign(account, fresh);
      if (_copies.size() >= _sweep_at) {
         std::erase_if(_copies, [](const auto& entry) { return entry.second.expired(); });
         _sweep_at = std::max(min_sweep_size, sweep_growth * _copies.size());
      }
      return fresh;
   }

private:
   /// Entries held before copies no capture holds any longer are first dropped.
   static constexpr size_t min_sweep_size = 1024;
   /// After a sweep, the next one comes once the entries left have grown this many times over.
   static constexpr size_t sweep_growth = 2;

   std::mutex _mtx; ///< guards every member below
   /// The copy each account's captures took last.
   std::unordered_map<chain::account_name, std::weak_ptr<const std::vector<char>>> _copies;
   /// Entries at which copies no capture holds are dropped next.
   size_t _sweep_at = min_sweep_size;
};

/// The copies every capture shares.
shared_abi_copies& abi_copies() {
   static shared_abi_copies copies;
   return copies;
}

} // namespace

captured_abis captured_abis::capture(const chain::controller& chain, const chain::signed_block& block) {
   captured_abis captured;
   for (const chain::transaction_receipt& receipt : block.transactions) {
      const chain::transaction& trx = receipt.trx.get_transaction();
      for (const chain::action& act : trx.actions)
         captured.add(chain, act.account);
      for (const chain::action& act : trx.context_free_actions)
         captured.add(chain, act.account);
   }
   return captured;
}

captured_abis captured_abis::capture(const chain::controller& chain, const chain::transaction_trace& trace) {
   captured_abis captured;
   for (const chain::action_trace& act_trace : trace.action_traces)
      captured.add(chain, act_trace.act.account);
   return captured;
}

const captured_abis::packed_abi_ptr& captured_abis::find(const chain::account_name& account) const {
   static const packed_abi_ptr none;
   const auto itr = _abis.find(account);
   return itr == _abis.end() ? none : itr->second;
}

void captured_abis::add(const chain::controller& chain, const chain::account_name& account) {
   const auto [itr, inserted] = _abis.try_emplace(account);
   if (!inserted || !account.good())
      return;
   const chain::account_metadata_object* metadata = chain.find_account_metadata(account);
   if (metadata == nullptr || abi_serializer::is_empty_abi(metadata->abi))
      return;
   itr->second = abi_copies().copy_of(account, std::span<const char>(metadata->abi.data(), metadata->abi.size()));
}

captured_abi_serializers::captured_abi_serializers(const fc::microseconds& max_build_time,
                                                   const fc::time_point&   deadline)
   : _max_build_time(max_build_time)
   , _deadline(deadline) {}

const abi_serializer* captured_abi_serializers::find(const captured_abis::packed_abi_ptr& abi) {
   if (!abi)
      return nullptr;
   const auto [itr, inserted] = _found.try_emplace(abi);
   if (inserted) {
      try {
         itr->second = shared_abi_serializer_cache().get(*abi, _max_build_time, _deadline);
      } catch (...) {
         // Remembered as none: the data of the account's actions stays hex.
      }
   }
   return itr->second.get();
}

} // namespace sysio::chain_apis
