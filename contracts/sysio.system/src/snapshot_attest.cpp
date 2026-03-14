
#include <sysio.system/sysio.system.hpp>
#include <sysio.system/snapshot_attest.hpp>
#include <sysio.system/block_utils.hpp>

#include <sysio/sysio.hpp>

namespace sysiosystem {

// -------------------------------------------------------------------------------------------------
void snapshot_attest::regsnapprov(name producer, name snap_account) {
   require_auth(producer);

   // Validate producer is registered and rank <= max_snap_provider_rank
   producers_table producers(get_self(), get_self().value);
   auto prod_itr = producers.find(producer.value);
   check(prod_itr != producers.end(), "producer is not registered");
   check(prod_itr->rank <= max_snap_provider_rank,
         "producer rank exceeds maximum for snapshot providers");

   // Ensure snap_account is not already registered
   snap_providers_table provs(get_self(), get_self().value);
   auto prov_itr = provs.find(snap_account.value);
   check(prov_itr == provs.end(), "snap_account is already registered as a provider");

   // Ensure this producer doesn't already have a provider registered
   auto by_prod = provs.get_index<"byproducer"_n>();
   auto prod_prov = by_prod.find(producer.value);
   check(prod_prov == by_prod.end(), "producer already has a registered snapshot provider");

   provs.emplace(producer, [&](auto& row) {
      row.snap_account = snap_account;
      row.producer     = producer;
   });
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::delsnapprov(name account) {
   require_auth(account);

   snap_providers_table provs(get_self(), get_self().value);

   // First try lookup as snap_account (primary key)
   auto prov_itr = provs.find(account.value);
   if (prov_itr != provs.end()) {
      require_auth(prov_itr->snap_account);
      provs.erase(prov_itr);
      return;
   }

   // Then try lookup as producer (secondary index)
   auto by_prod = provs.get_index<"byproducer"_n>();
   auto prod_itr = by_prod.find(account.value);
   check(prod_itr != by_prod.end(), "account is not registered as a snapshot provider or producer");
   by_prod.erase(prod_itr);
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::votesnaphash(name snap_account, checksum256 block_id, checksum256 snapshot_hash) {
   require_auth(snap_account);

   // Validate snap_account is a registered provider
   snap_providers_table provs(get_self(), get_self().value);
   auto prov_itr = provs.find(snap_account.value);
   check(prov_itr != provs.end(), "snap_account is not a registered snapshot provider");

   uint32_t block_num = block_info::block_height_from_id(block_id);
   check(block_num > 0, "invalid block_id");

   // Check for disagreement against attested records
   snap_records_table records(get_self(), get_self().value);
   auto rec_itr = records.find(static_cast<uint64_t>(block_num));
   if (rec_itr != records.end()) {
      check(rec_itr->snapshot_hash == snapshot_hash,
            "snapshot hash disagrees with attested record for this block");
   }

   // Find or create vote entry for this block_num + snapshot_hash
   snap_votes_table votes(get_self(), get_self().value);
   auto by_bn = votes.get_index<"byblocknum"_n>();

   // Search for existing vote with matching block_num and snapshot_hash
   uint64_t vote_id = 0;
   bool found = false;
   for (auto itr = by_bn.lower_bound(static_cast<uint64_t>(block_num));
        itr != by_bn.end() && itr->block_num == block_num; ++itr) {
      if (itr->snapshot_hash == snapshot_hash) {
         // Check voter hasn't already voted
         for (const auto& v : itr->voters) {
            check(v != snap_account, "snap_account has already voted for this snapshot");
         }
         vote_id = itr->id;
         found = true;
         break;
      }
   }

   uint32_t voter_count;
   if (found) {
      auto vote_itr = votes.find(vote_id);
      votes.modify(vote_itr, same_payer, [&](auto& row) {
         row.voters.push_back(snap_account);
      });
      voter_count = static_cast<uint32_t>(vote_itr->voters.size());
   } else {
      votes.emplace(snap_account, [&](auto& row) {
         row.id            = votes.available_primary_key();
         row.block_num     = block_num;
         row.block_id      = block_id;
         row.snapshot_hash = snapshot_hash;
         row.voters        = {snap_account};
      });
      voter_count = 1;
   }

   // Check quorum
   snap_config_singleton cfg_singleton(get_self(), get_self().value);
   snap_config cfg = cfg_singleton.get_or_default(snap_config{});

   // Count total registered providers
   uint32_t provider_count = 0;
   for (auto itr = provs.begin(); itr != provs.end(); ++itr) {
      ++provider_count;
   }

   uint32_t quorum = std::max(cfg.min_providers, (provider_count * cfg.threshold_pct + 99) / 100);

   if (voter_count >= quorum) {
      // Attestation reached — create record
      uint32_t current_block = static_cast<uint32_t>(sysio::current_block_number());

      if (rec_itr == records.end()) {
         records.emplace(get_self(), [&](auto& row) {
            row.block_num        = static_cast<uint64_t>(block_num);
            row.block_id         = block_id;
            row.snapshot_hash    = snapshot_hash;
            row.attested_at_block = current_block;
         });
      }

      // Purge old votes with block_num <= attested block_num
      auto purge_itr = by_bn.begin();
      while (purge_itr != by_bn.end() && purge_itr->block_num <= block_num) {
         purge_itr = by_bn.erase(purge_itr);
      }
   }
}

// -------------------------------------------------------------------------------------------------
void snapshot_attest::setsnpcfg(uint32_t min_providers, uint32_t threshold_pct) {
   require_auth(get_self());

   check(threshold_pct > 0 && threshold_pct <= 100, "threshold_pct must be between 1 and 100");
   check(min_providers > 0, "min_providers must be at least 1");

   snap_config_singleton cfg_singleton(get_self(), get_self().value);
   cfg_singleton.set(snap_config{min_providers, threshold_pct}, get_self());
}

// -------------------------------------------------------------------------------------------------
snap_record snapshot_attest::getsnaphash(uint32_t block_num) {
   snap_records_table records(get_self(), get_self().value);
   auto rec_itr = records.find(static_cast<uint64_t>(block_num));
   check(rec_itr != records.end(), "no attested snapshot record for this block number");
   return *rec_itr;
}

} // namespace sysiosystem
