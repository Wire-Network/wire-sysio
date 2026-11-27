#pragma once

#include <sysio/contract.hpp>
#include <sysio/crypto.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/multi_index.hpp> // sysio::const_mem_fun (secondary-index key extractor)
#include <sysio/name.hpp>

#include <sysio.system/snapshot_attest_constants.hpp>

#include <vector>

namespace sysiosystem {

using sysio::checksum256;
using sysio::name;

/// Producers ranked at or above this position may vote snapshot hashes.
static constexpr uint32_t max_snap_provider_rank = 30;

/// Most producers that can vote at once: one per eligible rank position.
static constexpr uint32_t max_snap_providers = max_snap_provider_rank;

/// Error code for disagreement with an already-attested snapshot record.
static constexpr uint64_t snap_hash_disagreement_error = snapshot_attestation::disagreement_error_code;

/** Secondary-index identifiers used by the snapshot-attestation tables. */
namespace snapshot_index {
/// Scan pending snapshot tuples by their scheduled block height.
static constexpr auto by_block_num = "byblocknum"_n;
} // namespace snapshot_index

// -------------------------------------------------------------------------------------------------
// Snapshot attestation configuration (singleton)
// -------------------------------------------------------------------------------------------------
/** Governance-controlled quorum and cadence configuration. */
struct [[sysio::table("snapconfig"), sysio::contract("sysio.system")]] snap_config {
   /// Fixed number of distinct producer votes required; zero disables attestation.
   uint32_t min_providers = 0;
   /// Configurable block interval; nodes must restart to adopt a changed value.
   uint32_t block_spacing = snapshot_attestation::default_block_spacing;

   SYSLIB_SERIALIZE(snap_config, (min_providers)(block_spacing))
};

using snap_config_singleton = sysio::kv::global<"snapconfig"_n, snap_config>;

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

/**
 * One pending snapshot tuple and its monotonic producer voters, billed to sysio. Rows exist only for the
 * last snapshot_attestation::max_pending_vote_heights scheduled heights. A producer votes once per height, so a
 * height holds at most one row per distinct producer that could vote while it was open.
 */
struct [[sysio::table("snapvotes"), sysio::contract("sysio.system")]] snap_vote {
   /// Pending tuple identifier.
   uint64_t           id;
   /// Snapshot height derived from block_id.
   uint32_t           block_num;
   /// Exact irreversible block identifier.
   checksum256        block_id;
   /// Deterministic snapshot root hash.
   checksum256        snapshot_hash;
   /// Producers that voted this tuple.
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
    * Submit a producer's snapshot hash vote for a scheduled snapshot height.
    *
    * Any producer holding one of the first max_snap_provider_rank positions among schedulable producers may vote.
    * A provider node normally signs with the producer's snapshot_attestation::permission_snapshot permission,
    * linked to this action only.
    *
    * Votes aggregate per (block_num, block_id, snapshot_hash) and finalize when the current fixed
    * min_providers value is reached. Votes are monotonic, producer equivocation is rejected per
    * height, and retrying the same tuple is idempotent while its height remains eligible under the
    * current cadence. Snapshot heights must be exact multiples of the configured block_spacing.
    * Rejects with snap_hash_disagreement_error when a final record at
    * the height differs by block id or snapshot hash. A height without its own final record cannot
    * be reopened below the latest attested height after pending rows have been purged.
    *
    * Only the last max_pending_vote_heights scheduled heights at or below the current block take
    * votes, retries included; each accepted vote purges rows below that window. A matching or
    * disagreeing vote on a final record is answered as above at any height.
    *
    * Every vote, including a no-op vote on a final tuple, requires that rank. A new vote, or a retry
    * while the tuple is pending, also requires the producer's bond to clear the live collateral
    * minimum, bootstrapped exempt. Votes accepted earlier keep counting.
    */
   [[sysio::action]]
   void votesnaphash(name producer, checksum256 block_id, checksum256 snapshot_hash);

   /**
    * Update snapshot attestation configuration. Requires contract authority.
    *
    * @param min_providers fixed number of distinct producer voters required to attest (1..30).
    * @param block_spacing Positive block interval; small values are for testing. Nodes restart to adopt changes.
    *
    * No attestation is permitted until this action stores a nonzero min_providers. Governance is
    * responsible for choosing K with the desired security and liveness tradeoff. K changes apply to
    * pending heights; an exact vote retry, from a producer that still has operator standing,
    * finalizes a tuple that already meets a newly lowered K. Cadence may change at any time;
    * operators restart nodes to adopt it. Small intervals increase snapshot overhead and shorten
    * verification grace; production should use much larger values. Grace is capped at 12,500 blocks.
    */
   [[sysio::action]]
   void setsnpcfg(uint32_t min_providers, uint32_t block_spacing);

   /**
    * Read-only: return the attested snapshot record for a given block number.
    */
   [[sysio::action]]
   snap_record getsnaphash(uint32_t block_num);
};

} // namespace sysiosystem
