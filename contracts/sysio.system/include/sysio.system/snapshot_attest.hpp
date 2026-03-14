#pragma once

#include <sysio/contract.hpp>
#include <sysio/crypto.hpp>
#include <sysio/multi_index.hpp>
#include <sysio/name.hpp>
#include <sysio/singleton.hpp>

#include <vector>

namespace sysiosystem {

using sysio::checksum256;
using sysio::name;

// Max producer rank eligible to register as snapshot provider
static constexpr uint32_t max_snap_provider_rank = 30;

// -------------------------------------------------------------------------------------------------
// Snapshot attestation configuration (singleton)
// -------------------------------------------------------------------------------------------------
struct [[sysio::table("snapconfig"), sysio::contract("sysio.system")]] snap_config {
   uint32_t min_providers  = 1;
   uint32_t threshold_pct  = 67;

   SYSLIB_SERIALIZE(snap_config, (min_providers)(threshold_pct))
};

using snap_config_singleton = sysio::singleton<"snapconfig"_n, snap_config>;

// -------------------------------------------------------------------------------------------------
// Registered snapshot providers
// -------------------------------------------------------------------------------------------------
struct [[sysio::table("snapprovs"), sysio::contract("sysio.system")]] snap_provider {
   name snap_account;
   name producer;

   uint64_t primary_key() const { return snap_account.value; }
   uint64_t by_producer() const { return producer.value; }

   SYSLIB_SERIALIZE(snap_provider, (snap_account)(producer))
};

using snap_providers_table = sysio::multi_index<
   "snapprovs"_n, snap_provider,
   sysio::indexed_by<"byproducer"_n, sysio::const_mem_fun<snap_provider, uint64_t, &snap_provider::by_producer>>>;

// -------------------------------------------------------------------------------------------------
// Pending snapshot votes (before quorum is reached)
// -------------------------------------------------------------------------------------------------
struct [[sysio::table("snapvotes"), sysio::contract("sysio.system")]] snap_vote {
   uint64_t           id;
   uint32_t           block_num;
   checksum256        block_id;
   checksum256        snapshot_hash;
   std::vector<name>  voters;

   uint64_t primary_key() const { return id; }
   uint64_t by_block_num() const { return static_cast<uint64_t>(block_num); }

   SYSLIB_SERIALIZE(snap_vote, (id)(block_num)(block_id)(snapshot_hash)(voters))
};

using snap_votes_table = sysio::multi_index<
   "snapvotes"_n, snap_vote,
   sysio::indexed_by<"byblocknum"_n, sysio::const_mem_fun<snap_vote, uint64_t, &snap_vote::by_block_num>>>;

// -------------------------------------------------------------------------------------------------
// Attested snapshot records (quorum reached)
// -------------------------------------------------------------------------------------------------
struct [[sysio::table("snaprecords"), sysio::contract("sysio.system")]] snap_record {
   uint64_t    block_num;
   checksum256 block_id;
   checksum256 snapshot_hash;
   uint32_t    attested_at_block;

   uint64_t primary_key() const { return block_num; }

   SYSLIB_SERIALIZE(snap_record, (block_num)(block_id)(snapshot_hash)(attested_at_block))
};

using snap_records_table = sysio::multi_index<"snaprecords"_n, snap_record>;

// -------------------------------------------------------------------------------------------------
// Snapshot attestation sub-contract
// -------------------------------------------------------------------------------------------------
struct [[sysio::contract("sysio.system")]] snapshot_attest : public sysio::contract {

   snapshot_attest(name s, name code, sysio::datastream<const char*> ds)
      : sysio::contract(s, code, ds) {}

   /**
    * Register a snapshot provider account delegated by a producer.
    * The producer must be registered and have rank <= max_snap_provider_rank.
    */
   [[sysio::action]]
   void regsnapprov(name producer, name snap_account);

   /**
    * Unregister a snapshot provider. Can be called by the snap_account itself
    * or looked up by producer via secondary index.
    */
   [[sysio::action]]
   void delsnapprov(name account);

   /**
    * Submit a snapshot hash vote. Accumulates votes; when quorum is reached,
    * creates an attested snap_record and purges older votes.
    */
   [[sysio::action]]
   void votesnaphash(name snap_account, checksum256 block_id, checksum256 snapshot_hash);

   /**
    * Update snapshot attestation configuration. Requires contract authority.
    */
   [[sysio::action]]
   void setsnpcfg(uint32_t min_providers, uint32_t threshold_pct);

   /**
    * Read-only: return the attested snapshot record for a given block number.
    */
   [[sysio::action]]
   snap_record getsnaphash(uint32_t block_num);
};

} // namespace sysiosystem
