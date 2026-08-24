#pragma once

#include <sysio/contract.hpp>
#include <sysio/crypto.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/multi_index.hpp> // sysio::const_mem_fun (secondary-index key extractor)
#include <sysio/name.hpp>
#include <sysio/protocol/snapshot_attestation.hpp>

#include <vector>

namespace sysiosystem {

using sysio::checksum256;
using sysio::name;

/// Maximum producer rank eligible to register a snapshot provider.
static constexpr uint32_t max_snap_provider_rank = 30;

/// Maximum number of producer-to-snapshot-account delegations retained at once.
static constexpr uint32_t max_snap_providers = max_snap_provider_rank;

/// Default quorum percentage used after governance sets a nonzero provider floor.
static constexpr uint32_t default_snap_threshold_pct = 67;

/// Error code for disagreement with an already-attested snapshot record.
static constexpr uint64_t snap_hash_disagreement_error =
   sysio::protocol::snapshot_attestation::disagreement_error_code;

/** Secondary-index identifiers used by the snapshot-attestation tables. */
namespace snapshot_index {
/// Lookup snapshot-provider mappings by their delegating producer.
static constexpr auto by_producer = "byproducer"_n;
/// Scan pending snapshot tuples by their scheduled block height.
static constexpr auto by_block_num = "byblocknum"_n;
} // namespace snapshot_index

// -------------------------------------------------------------------------------------------------
// Snapshot attestation configuration (singleton)
// -------------------------------------------------------------------------------------------------
/** Governance-controlled quorum configuration. */
struct [[sysio::table("snapconfig"), sysio::contract("sysio.system")]] snap_config {
   /// Zero means governance has not configured the security floor yet, so voting is disabled.
   uint32_t min_providers  = 0;
   /// Percentage of registrations used to freeze a height's quorum on its first vote.
   uint32_t threshold_pct  = default_snap_threshold_pct;

   SYSLIB_SERIALIZE(snap_config, (min_providers)(threshold_pct))
};

using snap_config_singleton = sysio::kv::global<"snapconfig"_n, snap_config>;

// -------------------------------------------------------------------------------------------------
// Registered snapshot providers
// -------------------------------------------------------------------------------------------------
/** Provider-account primary key for snapshot registrations. */
struct snap_provider_key_t {
   /// Raw snapshot-provider account value.
   uint64_t snap_account;
   SYSLIB_SERIALIZE(snap_provider_key_t, (snap_account))
};

/** One producer-to-snapshot-account delegation. */
struct [[sysio::table("snapprovs"), sysio::contract("sysio.system")]] snap_provider {
   /// Account authorized to submit snapshot votes.
   name snap_account;
   /// Active, rank-eligible producer represented by this account.
   name producer;

   /** Return the producer secondary-index key. */
   uint64_t by_producer() const { return producer.value; }

   SYSLIB_SERIALIZE(snap_provider, (snap_account)(producer))
};

using snap_providers_table = sysio::kv::table<
   "snapprovs"_n, snap_provider_key_t, snap_provider,
   sysio::kv::index<snapshot_index::by_producer,
                    sysio::const_mem_fun<snap_provider, uint64_t, &snap_provider::by_producer>>>;

// -------------------------------------------------------------------------------------------------
// Pending snapshot votes (before quorum is reached)
// -------------------------------------------------------------------------------------------------
/** Auto-incrementing primary key for pending snapshot tuples. */
struct snap_vote_key_t {
   /// Pending tuple identifier.
   uint64_t id;
   /// Lets snap_votes_table.available_primary_key() allocate the next vote id.
   uint64_t primary_key() const { return id; }
   SYSLIB_SERIALIZE(snap_vote_key_t, (id))
};

/** One pending snapshot tuple and its monotonic producer voters. */
struct [[sysio::table("snapvotes"), sysio::contract("sysio.system")]] snap_vote {
   /// Pending tuple identifier.
   uint64_t           id;
   /// Snapshot height derived from block_id.
   uint32_t           block_num;
   /// Exact irreversible block identifier.
   checksum256        block_id;
   /// Deterministic snapshot root hash.
   checksum256        snapshot_hash;
   /// Quorum frozen from the registration set when the first vote at this height was cast.
   uint32_t           quorum;
   /// Delegating producer identities, rather than rotatable snapshot-account identities.
   std::vector<name>  voters;

   /** Return the block-height secondary-index key. */
   uint64_t by_block_num() const { return static_cast<uint64_t>(block_num); }

   SYSLIB_SERIALIZE(snap_vote, (id)(block_num)(block_id)(snapshot_hash)(quorum)(voters))
};

using snap_votes_table = sysio::kv::table<
   "snapvotes"_n, snap_vote_key_t, snap_vote,
   sysio::kv::index<snapshot_index::by_block_num,
                    sysio::const_mem_fun<snap_vote, uint64_t, &snap_vote::by_block_num>>>;

// -------------------------------------------------------------------------------------------------
// Attested snapshot records (quorum reached)
// -------------------------------------------------------------------------------------------------
/** Block-height primary key for final snapshot attestations. */
struct snap_record_key_t {
   /// Attested snapshot block height.
   uint64_t block_num;
   SYSLIB_SERIALIZE(snap_record_key_t, (block_num))
};

/** Permanent on-chain snapshot attestation. */
struct [[sysio::table("snaprecords"), sysio::contract("sysio.system")]] snap_record {
   /// Attested snapshot block height.
   uint32_t    block_num;
   /// Exact irreversible block identifier.
   checksum256 block_id;
   /// Deterministic snapshot root hash.
   checksum256 snapshot_hash;
   /// Chain block that created this record.
   uint32_t    attested_at_block;

   SYSLIB_SERIALIZE(snap_record, (block_num)(block_id)(snapshot_hash)(attested_at_block))
};

using snap_records_table = sysio::kv::table<"snaprecords"_n, snap_record_key_t, snap_record>;

// -------------------------------------------------------------------------------------------------
// Snapshot attestation sub-contract
// -------------------------------------------------------------------------------------------------
struct [[sysio::contract("sysio.system")]] snapshot_attest : public sysio::contract {

   /** Construct the snapshot-attestation sub-contract dispatcher. */
   snapshot_attest(name s, name code, sysio::datastream<const char*> ds)
      : sysio::contract(s, code, ds) {}

   /**
    * Register a snapshot provider account delegated by a producer.
    *
    * The producer must be active and have rank <= max_snap_provider_rank at registration time.
    * Operator-registry status is deliberately not consulted: producer-table eligibility is the
    * attestation trust root. When the table is full, stale producer mappings are pruned lazily
    * before enforcing the capacity limit.
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
    * Submit a snapshot hash vote from a registered provider for a scheduled snapshot height.
    *
    * Votes aggregate per (block_num, block_id, snapshot_hash). The first vote at a height freezes
    * one quorum shared by every competing tuple at that height. Votes are monotonic, producer
    * equivocation is rejected per height, and retrying the same tuple is idempotent. Snapshot
    * heights must be exact multiples of protocol::snapshot_attestation::block_spacing. Rejects
    * with snap_hash_disagreement_error when a final record at the height differs by block id or
    * snapshot hash. A height without its own final record cannot be reopened below the latest
    * attested height after pending rows have been purged.
    */
   [[sysio::action]]
   void votesnaphash(name snap_account, checksum256 block_id, checksum256 snapshot_hash);

   /**
    * Update snapshot attestation configuration. Requires contract authority.
    *
    * @param min_providers  minimum voters required to attest (must be >= 1).
    * @param threshold_pct  percentage of currently registered providers required (1..100).
    *
    * No attestation is permitted until this action stores a nonzero min_providers. When the first
    * vote arrives at a height, N registered providers must be at least min_providers and the
    * height's immutable quorum is
    *     max( max(min_providers, ceil(N * threshold_pct / 100)), floor(N/3) + 1 )
    * The trailing floor(N/3)+1 is a Byzantine safety floor (see votesnaphash): an attestation
    * must always carry more than N/3 providers so a misconfigured-low threshold cannot let a
    * Byzantine minority attest an arbitrary snapshot. It is a no-op under the default
    * threshold_pct = 67 (ceil(0.67*N) >= floor(N/3)+1 for every N) and only raises the bar when
    * threshold_pct is set below ~33%. Configuration changes apply only to heights that have not
    * received their first vote yet.
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
