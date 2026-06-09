
#include <sysio.system/sysio.system.hpp>
#include <sysio.system/snapshot_attest.hpp>
#include <sysio.system/block_utils.hpp>

#include <sysio/sysio.hpp>

#include <utility>

namespace sysiosystem {

// -------------------------------------------------------------------------------------------------
void snapshot_attest::regsnapprov(name producer, name snap_account) {
   require_auth(producer);

   // Validate producer is registered and rank <= max_snap_provider_rank
   producers_table producers(get_self());
   auto prod_itr = producers.require_find(producer_key_t{producer.value}, "producer is not registered");
   check(prod_itr->rank <= max_snap_provider_rank,
         "producer rank exceeds maximum for snapshot providers");

   // Ensure snap_account is not already registered
   snap_providers_table provs(get_self());
   check(!provs.contains(snap_provider_key_t{snap_account.value}),
         "snap_account is already registered as a provider");

   // Ensure this producer doesn't already have a provider registered
   auto by_prod = provs.get_index<"byproducer"_n>();
   check(by_prod.find(producer.value) == by_prod.end(),
         "producer already has a registered snapshot provider");

   provs.emplace(producer, snap_provider_key_t{snap_account.value}, [&](auto& row) {
      row.snap_account = snap_account;
      row.producer     = producer;
   });
}

// -------------------------------------------------------------------------------------------------
// The `account` parameter is overloaded: it can be either a snap_account (primary key lookup)
// or a producer (secondary index lookup). The primary key path takes precedence.
// This means a producer can unregister their own provider, and a snap_account can unregister itself.
// regsnapprov() enforces uniqueness of both snap_account and producer, so collisions cannot occur.
void snapshot_attest::delsnapprov(name account) {
   require_auth(account);

   snap_providers_table provs(get_self());

   // First try lookup as snap_account (primary key)
   auto prov_itr = provs.find(snap_provider_key_t{account.value});
   if (prov_itr != provs.end()) {
      require_auth(prov_itr->snap_account);
      provs.erase(std::move(prov_itr));
      return;
   }

   // Then try lookup as producer (secondary index)
   auto by_prod = provs.get_index<"byproducer"_n>();
   auto prod_itr = by_prod.find(account.value);
   check(prod_itr != by_prod.end(), "account is not registered as a snapshot provider or producer");
   by_prod.erase(std::move(prod_itr));
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::votesnaphash(name snap_account, checksum256 block_id, checksum256 snapshot_hash) {
   require_auth(snap_account);

   // Validate snap_account is a registered provider
   snap_providers_table provs(get_self());
   check(provs.contains(snap_provider_key_t{snap_account.value}),
         "snap_account is not a registered snapshot provider");

   uint32_t block_num = block_info::block_height_from_id(block_id);
   check(block_num > 0, "invalid block_id");

   // Check for disagreement against any already-attested record.
   // Uses snap_hash_disagreement_error code so nodeop can detect this specific failure
   // without fragile string matching (see producer_plugin.cpp::submit_snapshot_vote).
   snap_records_table records(get_self());
   snap_record_key_t  rec_key{static_cast<uint64_t>(block_num)};
   auto rec_itr = records.find(rec_key);
   if (rec_itr != records.end()) {
      check(rec_itr->snapshot_hash == snapshot_hash, snap_hash_disagreement_error);
   }

   // Find or create the vote entry for this block_num + snapshot_hash
   snap_votes_table votes(get_self());
   auto by_bn = votes.get_index<"byblocknum"_n>();

   uint64_t vote_id     = 0;
   uint32_t voter_count = 0;
   bool     found       = false;
   for (auto itr = by_bn.lower_bound(static_cast<uint64_t>(block_num));
        itr != by_bn.end() && itr->block_num == block_num; ++itr) {
      if (itr->snapshot_hash == snapshot_hash) {
         // Check voter hasn't already voted
         for (const auto& v : itr->voters) {
            check(v != snap_account, "snap_account has already voted for this snapshot");
         }
         vote_id     = itr->id;
         voter_count = static_cast<uint32_t>(itr->voters.size()) + 1;
         found       = true;
         break;
      }
   }

   if (found) {
      votes.modify(same_payer, snap_vote_key_t{vote_id}, [&](auto& row) {
         row.voters.push_back(snap_account);
      });
   } else {
      uint64_t new_id = votes.available_primary_key();
      votes.emplace(snap_account, snap_vote_key_t{new_id}, [&](auto& row) {
         row.id            = new_id;
         row.block_num     = block_num;
         row.block_id      = block_id;
         row.snapshot_hash = snapshot_hash;
         row.voters        = {snap_account};
      });
      voter_count = 1;
   }

   // Check quorum
   snap_config_singleton cfg_singleton(get_self());
   snap_config cfg = cfg_singleton.get_or_default(snap_config{});

   // Count total registered providers.
   // O(n) iteration is acceptable here: max_snap_provider_rank (30) bounds the table size.
   uint32_t provider_count = 0;
   for (auto itr = provs.begin(); itr != provs.end(); ++itr) {
      ++provider_count;
   }

   uint32_t quorum = std::max(cfg.min_providers, (provider_count * cfg.threshold_pct + 99) / 100);

   if (voter_count >= quorum) {
      // Attestation reached -- create the record if it does not already exist.
      // Re-check existence here (rather than reusing the iterator from above) so the
      // decision does not depend on iterator liveness across the intervening votes writes.
      if (!records.contains(rec_key)) {
         uint32_t current_block = static_cast<uint32_t>(sysio::current_block_number());
         records.emplace(get_self(), rec_key, [&](auto& row) {
            row.block_num         = block_num;
            row.block_id          = block_id;
            row.snapshot_hash     = snapshot_hash;
            row.attested_at_block = current_block;
         });
      }

      // Purge old votes with block_num <= attested block_num
      auto purge_itr = by_bn.begin();
      while (purge_itr != by_bn.end() && purge_itr->block_num <= block_num) {
         purge_itr = by_bn.erase(std::move(purge_itr));
      }
   }
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::setsnpcfg(uint32_t min_providers, uint32_t threshold_pct) {
   require_auth(get_self());

   check(threshold_pct > 0 && threshold_pct <= 100, "threshold_pct must be between 1 and 100");
   check(min_providers > 0, "min_providers must be at least 1");

   snap_config_singleton cfg_singleton(get_self());
   cfg_singleton.set(snap_config{min_providers, threshold_pct}, get_self());
}

// -------------------------------------------------------------------------------------------------
snap_record snapshot_attest::getsnaphash(uint32_t block_num) {
   snap_records_table records(get_self());
   auto rec_itr = records.require_find(snap_record_key_t{static_cast<uint64_t>(block_num)},
                                       "no attested snapshot record for this block number");
   return *rec_itr;
}

} // namespace sysiosystem
