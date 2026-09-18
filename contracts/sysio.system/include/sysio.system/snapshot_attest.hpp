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
   /// Fixed number of distinct producer votes required; zero disables attestation.
   uint32_t min_providers = 0;

   SYSLIB_SERIALIZE(snap_config, (min_providers))
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
   /// Delegating producer, which was eligible when it entered the provider set but need not be now.
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
   /// Delegating producer identities, rather than rotatable snapshot-account identities.
   std::vector<name>  voters;

   /** Return the block-height secondary-index key. */
   uint64_t by_block_num() const { return static_cast<uint64_t>(block_num); }

   SYSLIB_SERIALIZE(snap_vote, (id)(block_num)(block_id)(snapshot_hash)(voters))
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
    * A producer that currently holds no mapping must be active and hold a rank position <=
    * max_snap_provider_rank. That walk tests `is_schedulable`, so operator-registry status and an
    * active finalizer key are both consulted through it, and a rejection names which of the three
    * conditions failed. When such a gated registration finds the table full, mappings whose
    * producers would now fail the gate are pruned before the capacity limit is enforced; a
    * producer evicted that way holds no mapping, so registering again is gated.
    *
    * A producer that already holds a mapping rotates it to the new snapshot account without retracting votes
    * already recorded under the producer identity, and rotation is deliberately NOT eligibility-gated. `delsnapprov`
    * does not replace it: leaving is one-way, since registering again is gated, so an ineligible producer that
    * rotates keeps a delegation it can still serve from and carry back into eligibility.
    */
   [[sysio::action]]
   void regsnapprov(name producer, name snap_account);

   /**
    * Retire the caller's snapshot-provider delegation, freeing its registration slot.
    *
    * Not eligibility-gated, for the reason rotation is not: a producer that has become ineligible
    * is exactly the one that needs to stop, and the capacity prune is out of reach below
    * max_snap_providers. Votes already accepted are keyed by producer identity and are not
    * retracted, and the producer keeps the attestation credit it earned in the open pay period.
    */
   [[sysio::action]]
   void delsnapprov(name producer);

   /**
    * Submit a snapshot hash vote from a registered provider for a scheduled snapshot height.
    *
    * Votes aggregate per (block_num, block_id, snapshot_hash) and finalize when the current fixed
    * min_providers value is reached. Votes are monotonic, producer equivocation is rejected per
    * height, and retrying the same tuple is idempotent. Snapshot heights must be exact multiples of
    * protocol::snapshot_attestation::block_spacing. Rejects with snap_hash_disagreement_error when
    * a final record at the height differs by block id or snapshot hash. A height without its own
    * final record cannot be reopened below the latest attested height after pending rows have been
    * purged.
    */
   [[sysio::action]]
   void votesnaphash(name snap_account, checksum256 block_id, checksum256 snapshot_hash);

   /**
    * Update snapshot attestation configuration. Requires contract authority.
    *
    * @param min_providers fixed number of distinct producer voters required to attest (1..30).
    *
    * No attestation is permitted until this action stores a nonzero min_providers. Governance is
    * responsible for choosing K with the desired security and liveness tradeoff. Configuration
    * changes apply to pending heights; an exact vote retry finalizes a tuple that already meets a
    * newly lowered K.
    */
   [[sysio::action]]
   void setsnpcfg(uint32_t min_providers);

   /**
    * Read-only: return the attested snapshot record for a given block number.
    */
   [[sysio::action]]
   snap_record getsnaphash(uint32_t block_num);
};

} // namespace sysiosystem
