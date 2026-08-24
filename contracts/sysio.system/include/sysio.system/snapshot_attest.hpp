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

/// Maximum registered snapshot providers and producer rank eligible to register one.
static constexpr uint32_t max_snap_provider_rank = 30;

/// Default quorum percentage used after governance sets a nonzero provider floor.
static constexpr uint32_t default_snap_threshold_pct = 67;

/// Error code for disagreement with an already-attested snapshot record.
static constexpr uint64_t snap_hash_disagreement_error =
   sysio::protocol::snapshot_attestation::disagreement_error_code;

// -------------------------------------------------------------------------------------------------
// Snapshot attestation configuration (singleton)
// -------------------------------------------------------------------------------------------------
/** Governance-controlled quorum configuration. */
struct [[sysio::table("snapconfig"), sysio::contract("sysio.system")]] snap_config {
   /// Zero means governance has not configured the security floor yet, so voting is disabled.
   uint32_t min_providers  = 0;
   /// Percentage of current live registrations required to attest.
   uint32_t threshold_pct  = default_snap_threshold_pct;

   SYSLIB_SERIALIZE(snap_config, (min_providers)(threshold_pct))
};

using snap_config_singleton = sysio::kv::global<"snapconfig"_n, snap_config>;

// -------------------------------------------------------------------------------------------------
// Registered snapshot providers
// -------------------------------------------------------------------------------------------------
/** Provider-account primary key for live snapshot registrations. */
struct snap_provider_key_t {
   /// Raw snapshot-provider account value.
   uint64_t snap_account;
   SYSLIB_SERIALIZE(snap_provider_key_t, (snap_account))
};

/** One live producer-to-snapshot-account delegation. */
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
   sysio::kv::index<"byproducer"_n, sysio::const_mem_fun<snap_provider, uint64_t, &snap_provider::by_producer>>>;

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

/** One pending snapshot tuple and its live producer voters. */
struct [[sysio::table("snapvotes"), sysio::contract("sysio.system")]] snap_vote {
   /// Pending tuple identifier.
   uint64_t           id;
   /// Snapshot height derived from block_id.
   uint32_t           block_num;
   /// Exact irreversible block identifier.
   checksum256        block_id;
   /// Deterministic snapshot root hash.
   checksum256        snapshot_hash;
   /// Delegating producer identities, rather than rotatable snapshot-account identities.
   std::vector<name>  voters;

   /** Return the block-height secondary-index key. */
   uint64_t by_block_num() const { return static_cast<uint64_t>(block_num); }

   SYSLIB_SERIALIZE(snap_vote, (id)(block_num)(block_id)(snapshot_hash)(voters))
};

using snap_votes_table = sysio::kv::table<
   "snapvotes"_n, snap_vote_key_t, snap_vote,
   sysio::kv::index<"byblocknum"_n, sysio::const_mem_fun<snap_vote, uint64_t, &snap_vote::by_block_num>>>;

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
    * The producer must be active and have rank <= max_snap_provider_rank. Stale registrations are
    * removed at every producer-eligibility mutation site and rechecked before each vote.
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
    * Submit a snapshot hash vote from a currently active, rank-eligible provider.
    *
    * Votes aggregate per (block_num, block_id, snapshot_hash). The numerator and denominator both
    * use the same current registration set. A producer carries at most one pending vote across all
    * heights, and retrying the same tuple is idempotent. Removing a registration prunes only that
    * producer's pending weight; additions preserve pending votes. Any tuple made quorate by a
    * removal is finalized synchronously. Rejects with snap_hash_disagreement_error when a final
    * record at the height differs by block id or snapshot hash.
    */
   [[sysio::action]]
   void votesnaphash(name snap_account, checksum256 block_id, checksum256 snapshot_hash);

   /**
    * Update snapshot attestation configuration. Requires contract authority.
    *
    * @param min_providers  minimum voters required to attest (must be >= 1).
    * @param threshold_pct  percentage of currently registered providers required (1..100).
    *
    * No attestation is permitted until this action stores a nonzero min_providers. For N current
    * providers, N must be at least min_providers and the effective quorum is
    *     max( max(min_providers, ceil(N * threshold_pct / 100)), floor(N/3) + 1 )
    * The trailing floor(N/3)+1 is a Byzantine safety floor (see votesnaphash): an attestation
    * must always carry more than N/3 providers so a misconfigured-low threshold cannot let a
    * Byzantine minority attest an arbitrary snapshot. It is a no-op under the default
    * threshold_pct = 67 (ceil(0.67*N) >= floor(N/3)+1 for every N) and only raises the bar when
    * threshold_pct is set below ~33%. Lowering the configured quorum immediately re-evaluates all
    * bounded pending tuples against the new configuration.
    */
   [[sysio::action]]
   void setsnpcfg(uint32_t min_providers, uint32_t threshold_pct);

   /**
    * Read-only: return the attested snapshot record for a given block number.
    */
   [[sysio::action]]
   snap_record getsnaphash(uint32_t block_num);
};

/**
 * Remove registrations whose producers are no longer active or rank-eligible.
 *
 * Every producer lifecycle/rank mutation calls this helper. It removes each departed producer's
 * pending weight and synchronously finalizes tuples that meet the smaller live-set quorum.
 */
void reconcile_snapshot_registrations(name self);

} // namespace sysiosystem
