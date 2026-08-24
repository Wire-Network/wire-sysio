#pragma once

#include <sysio/contract.hpp>
#include <sysio/crypto.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/multi_index.hpp> // sysio::const_mem_fun (secondary-index key extractor)
#include <sysio/name.hpp>
#include <sysio/protocol/snapshot_attestation.hpp>

#include <limits>
#include <vector>

namespace sysiosystem {

using sysio::checksum256;
using sysio::name;

/// Maximum producer rank eligible for snapshot-provider registration.
static constexpr uint32_t max_snap_provider_rank = 30;

/// Maximum number of snapshot registrations and active roster members.
static constexpr uint32_t max_snap_roster_size = 30;

/// Maximum obsolete pending-vote rows removed by one snapshot action.
static constexpr uint32_t max_snap_vote_cleanup_rows = 30;

/// Error code for a tuple that differs from the already-attested record; nodeop fails closed on it.
static constexpr uint64_t snap_hash_disagreement_error =
   sysio::protocol::snapshot_attestation::disagreement_error_code;

// -------------------------------------------------------------------------------------------------
// Snapshot attestation configuration (singleton)
// -------------------------------------------------------------------------------------------------
/** Governance-controlled snapshot quorum configuration. */
struct [[sysio::table("snapconfig"), sysio::contract("sysio.system")]] snap_config {
   /// Hard minimum number of weighted voters required for attestation.
   uint32_t min_providers  = 1;
   /// Percentage of the stable active roster required for attestation.
   uint32_t threshold_pct  = 67;

   SYSLIB_SERIALIZE(snap_config, (min_providers)(threshold_pct))
};

using snap_config_singleton = sysio::kv::global<"snapconfig"_n, snap_config>;

// -------------------------------------------------------------------------------------------------
// Snapshot-provider registration candidates
// -------------------------------------------------------------------------------------------------
/** Primary key for provider-account keyed snapshot tables. */
struct snap_account_key_t {
   /// Raw snapshot-provider account value.
   uint64_t snap_account;
   SYSLIB_SERIALIZE(snap_account_key_t, (snap_account))
};

/** Currently eligible producer-to-provider registration candidate. */
struct [[sysio::table("snapregs"), sysio::contract("sysio.system")]] snap_registration {
   /// Account authorized to sign snapshot attestations.
   name snap_account;
   /// Producer that delegated authority to the provider account.
   name producer;

   /** Return the producer secondary-index key. */
   uint64_t by_producer() const { return producer.value; }

   SYSLIB_SERIALIZE(snap_registration, (snap_account)(producer))
};

using snap_registrations_table = sysio::kv::table<
   "snapregs"_n, snap_account_key_t, snap_registration,
   sysio::kv::index<"byproducer"_n,
                    sysio::const_mem_fun<snap_registration, uint64_t, &snap_registration::by_producer>>>;

// -------------------------------------------------------------------------------------------------
// Stable active snapshot-provider roster
// -------------------------------------------------------------------------------------------------
/** One producer/provider pair in the stable active voting roster. */
struct [[sysio::table("snaproster"), sysio::contract("sysio.system")]] snap_roster_member {
   /// Account authorized to sign snapshot attestations.
   name snap_account;
   /// Producer whose voting weight the provider represents.
   name producer;

   /** Return the producer secondary-index key. */
   uint64_t by_producer() const { return producer.value; }

   SYSLIB_SERIALIZE(snap_roster_member, (snap_account)(producer))
};

using snap_roster_table = sysio::kv::table<
   "snaproster"_n, snap_account_key_t, snap_roster_member,
   sysio::kv::index<"byproducer"_n,
                    sysio::const_mem_fun<snap_roster_member, uint64_t, &snap_roster_member::by_producer>>>;

/** Versioned digests and cleanup progress for the active and candidate rosters. */
struct [[sysio::table("snaprstate"), sysio::contract("sysio.system")]] snap_roster_state {
   /// Monotonic active-roster version.
   uint64_t    active_version       = 0;
   /// Block at which the current roster became active.
   uint32_t    activated_at_block   = 0;
   /// Stable active-roster denominator.
   uint32_t    active_count         = 0;
   /// Digest of the canonical active roster.
   checksum256 active_digest;
   /// Number of currently eligible registration candidates.
   uint32_t    candidate_count      = 0;
   /// Digest of the canonical candidate pool.
   checksum256 candidate_digest;
   /// Inclusive primary-id cursor for the next bounded pending-vote cleanup page.
   uint64_t    cleanup_cursor       = 0;
   /// Highest finalized snapshot height applied by all cleanup-cursor continuation pages.
   uint32_t    cleanup_finalized_height = 0;

   SYSLIB_SERIALIZE(snap_roster_state,
                    (active_version)(activated_at_block)(active_count)(active_digest)
                    (candidate_count)(candidate_digest)(cleanup_cursor)(cleanup_finalized_height))
};

using snap_roster_state_singleton = sysio::kv::global<"snaprstate"_n, snap_roster_state>;

/** Governance-approved active-roster shrink staged for a snapshot boundary. */
struct [[sysio::table("snaprprop"), sysio::contract("sysio.system")]] snap_roster_proposal {
   /// Active version that the proposal is authorized to replace.
   uint64_t                        expected_active_version = 0;
   /// Exact ordered member set approved by governance.
   std::vector<snap_roster_member> members;
   /// Digest of the canonical proposed member set.
   checksum256                     digest;

   SYSLIB_SERIALIZE(snap_roster_proposal, (expected_active_version)(members)(digest))
};

using snap_roster_proposal_singleton = sysio::kv::global<"snaprprop"_n, snap_roster_proposal>;

// -------------------------------------------------------------------------------------------------
// Pending snapshot votes (before quorum is reached)
// -------------------------------------------------------------------------------------------------
/** Auto-incrementing primary key for pending snapshot tuples. */
struct snap_vote_key_t {
   /// Pending-vote identifier.
   uint64_t id;
   // primary_key() lets snap_votes_table.available_primary_key() allocate the next vote id.
   uint64_t primary_key() const { return id; }
   SYSLIB_SERIALIZE(snap_vote_key_t, (id))
};

/** Build the exact secondary-index key for one block height and roster version. */
constexpr uint128_t make_snap_vote_block_roster_key(uint32_t block_num, uint64_t roster_version) {
   return (uint128_t{block_num} << std::numeric_limits<uint64_t>::digits) | roster_version;
}

/** One version-bound pending snapshot tuple and its producer voters. */
struct [[sysio::table("snapvotes"), sysio::contract("sysio.system")]] snap_vote {
   /// Pending-vote identifier.
   uint64_t           id;
   /// Active-roster version under which votes were cast.
   uint64_t           roster_version;
   /// Snapshot block height derived from block_id.
   uint32_t           block_num;
   /// Irreversible block identifier bound to the snapshot.
   checksum256        block_id;
   /// Deterministic snapshot root hash.
   checksum256        snapshot_hash;
   // Delegating PRODUCER identities that have voted -- NOT snap_accounts. Counting by the
   // stable producer prevents a producer from inflating the count (and clearing the Byzantine
   // quorum floor) by rotating snap_accounts. See snapshot_attest::votesnaphash.
   std::vector<name>  voters;

   /** Return the block-height secondary-index key. */
   uint64_t by_block_num() const { return static_cast<uint64_t>(block_num); }

   /** Return the exact block-height and roster-version composite index key. */
   uint128_t by_block_roster() const {
      return make_snap_vote_block_roster_key(block_num, roster_version);
   }

   SYSLIB_SERIALIZE(snap_vote, (id)(roster_version)(block_num)(block_id)(snapshot_hash)(voters))
};

using snap_votes_table = sysio::kv::table<
   "snapvotes"_n, snap_vote_key_t, snap_vote,
   sysio::kv::index<"byblocknum"_n, sysio::const_mem_fun<snap_vote, uint64_t, &snap_vote::by_block_num>>,
   sysio::kv::index<"byblkroster"_n,
                    sysio::const_mem_fun<snap_vote, uint128_t, &snap_vote::by_block_roster>>>;

// -------------------------------------------------------------------------------------------------
// Attested snapshot records (quorum reached)
// -------------------------------------------------------------------------------------------------
/** Block-height primary key for final snapshot records. */
struct snap_record_key_t {
   /// Finalized snapshot block height.
   uint64_t block_num;
   SYSLIB_SERIALIZE(snap_record_key_t, (block_num))
};

/** Permanent final snapshot attestation and its exact roster provenance. */
struct [[sysio::table("snaprecords"), sysio::contract("sysio.system")]] snap_record {
   /// Finalized snapshot block height.
   uint32_t    block_num;
   /// Irreversible block identifier bound to the snapshot.
   checksum256 block_id;
   /// Deterministic snapshot root hash.
   checksum256 snapshot_hash;
   /// Chain block at which quorum finalized the record.
   uint32_t    attested_at_block;
   /// Active-roster version that reached quorum.
   uint64_t    roster_version;
   /// Digest of the exact roster that reached quorum.
   checksum256 roster_digest;

   SYSLIB_SERIALIZE(snap_record,
                    (block_num)(block_id)(snapshot_hash)(attested_at_block)(roster_version)(roster_digest))
};

using snap_records_table = sysio::kv::table<"snaprecords"_n, snap_record_key_t, snap_record>;

// -------------------------------------------------------------------------------------------------
// Snapshot attestation sub-contract
// -------------------------------------------------------------------------------------------------
struct [[sysio::contract("sysio.system")]] snapshot_attest : public sysio::contract {

   snapshot_attest(name s, name code, sysio::datastream<const char*> ds)
      : sysio::contract(s, code, ds) {}

   /**
    * Register a snapshot provider candidate delegated by a producer.
    *
    * The producer must be active and have rank <= max_snap_provider_rank. A retained provider
    * registration does not preserve eligibility: producer lifecycle changes remove ineligible
    * candidates without mutating the stable active roster. At capacity, deterministic rank/name
    * ordering retains only the best max_snap_roster_size candidates.
    */
   [[sysio::action]]
   void regsnapprov(name producer, name snap_account);

   /**
    * Unregister a snapshot-provider candidate. Can be called by the snap_account itself or looked
    * up by producer via secondary index. Active roster membership remains unchanged until a later
    * atomic roster activation.
    */
   [[sysio::action]]
   void delsnapprov(name account);

   /**
    * Submit a snapshot hash vote from a currently registered, eligible active-roster member.
    *
    * Votes aggregate per (roster_version, block_num, block_id, snapshot_hash). The first vote for a
    * height may atomically activate a complete non-shrinking candidate roster. Retrying the same
    * tuple is idempotent. Rejects with snap_hash_disagreement_error when an attested record already
    * exists for the height and either the snapshot hash or the block id differs from it.
    */
   [[sysio::action]]
   void votesnaphash(name snap_account, checksum256 block_id, checksum256 snapshot_hash);

   /**
    * Permissionlessly re-evaluate one exact pending tuple against its bound active roster version.
    *
    * The supplied tuple and version must exactly match vote_id. Exact obsolete rows are accepted as
    * bounded cleanup work. The action adds no vote and is idempotent after a matching final record.
    */
   [[sysio::action]]
   void evalsnapvote(uint64_t vote_id,
                     checksum256 expected_block_id,
                     checksum256 expected_snapshot_hash,
                     uint64_t expected_roster_version);

   /**
    * Stage an exact governance-approved active-roster shrink for the next snapshot boundary.
    *
    * The proposal must target the current active version, contain fewer members than the current
    * roster, remain at or above min_providers, and contain only currently eligible registrations.
    */
   [[sysio::action]]
   void propsnaprost(std::vector<snap_roster_member> members, uint64_t expected_active_version);

   /** Cancel the currently staged governance roster proposal. */
   [[sysio::action]]
   void cancsnaprost();

   /**
    * Update snapshot attestation configuration. Requires contract authority.
    *
    * @param min_providers  minimum voters required to attest (must be >= 1).
    * @param threshold_pct  percentage of active-roster members required (1..100).
    *
    * For N active-roster members the effective quorum is
    *     max( max(min_providers, ceil(N * threshold_pct / 100)), floor(N/3) + 1 )
    * The trailing floor(N/3)+1 is a Byzantine safety floor (see votesnaphash): an attestation
    * must always carry more than N/3 providers so a misconfigured-low threshold cannot let a
    * Byzantine minority attest an arbitrary snapshot. It is a no-op under the default
    * threshold_pct = 67 (ceil(0.67*N) >= floor(N/3)+1 for every N) and only raises the bar when
    * threshold_pct is set below ~33%. A single-provider chain (N = 1) attests with one vote.
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
 * Reconcile the bounded snapshot-registration candidate pool after a producer lifecycle change.
 *
 * This shared helper is called by snapshot-provider actions and every system-contract mutation of
 * producer activity or rank. It may remove ineligible registrations and refresh candidate state,
 * but it never mutates the stable active roster.
 */
void reconcile_snapshot_registrations(name self);

} // namespace sysiosystem
